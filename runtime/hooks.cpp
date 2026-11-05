#include "hooks.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "MinHook.h"
#include "level.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>

namespace {

struct Saved {
    uint8_t shadow[32], xmm[6][16];
    uint64_t ctx, r11, r10, r9, r8, rdx, rcx, ret;
};
static_assert(offsetof(Saved, xmm) == 0x20 && offsetof(Saved, ctx) == 0x80);
static_assert(offsetof(Saved, rcx) == 0xb0 && offsetof(Saved, ret) == 0xb8);

struct Buffer {
    std::unique_ptr<hk::Event[]> events;
    std::unique_ptr<uint8_t[]> payload;
    uint32_t count = 0, used = 0;
    void clear() { count = used = 0; }
};
struct Target;
struct Code {
    uintptr_t start = 0;
    uint8_t bytes[10]{};  // fn-5..fn+4: MinHook's five-byte patch at fn, or its hotpatch at fn-5..fn+1
};
struct Sub {
    Target* target = nullptr;
    std::string key;
    std::array<hk::Arg, hk::kMaxArgs> args{};
    uint32_t count = 0, capacity = 0, payloadSize = 0;
    SRWLOCK mx = SRWLOCK_INIT;
    bool active = false;
    std::atomic<uint64_t> generation{1};
    uint64_t delivered = 0, dropped = 0, discarded = 0, pending = 0, inDelivery = 0;
    uint32_t highWater = 0;
    std::atomic<uint64_t> lockWaits{0};
    Buffer write, read;
};
struct Target {
    void* trampoline = nullptr;  // assembly contract: offset zero
    uintptr_t fn = 0;
    void* thunk = nullptr;
    Code code;
    SRWLOCK mx = SRWLOCK_INIT;
    std::array<std::unique_ptr<Sub>, hk::kMaxSubs> subs;
    int count = 0;
    std::atomic<unsigned> live{0};
    std::atomic<bool> faulted{false}, faultPending{false}, timing{false};
    std::atomic<uint64_t> calls{0}, ticks{0}, timedCalls{0}, decoded{0}, lockWaits{0};
};
static_assert(offsetof(Target, trampoline) == 0);
struct Notice { std::string text; bool logged = false; };
std::mutex gMx;
std::array<std::unique_ptr<Target>, hk::kMaxTargets> gTargets;
struct Reserved { uintptr_t fn; std::string name; Code code; };
std::vector<Reserved> gReserved;
std::deque<Notice> gNotes;
size_t gBytes = 0;
int gCount = 0;
int64_t gEpoch = 0, gFreq = 1;
std::atomic<uint64_t> gSeq{0}, gProblems{0};
std::atomic<hk::Log> gLog{nullptr};

struct Exclusive {
    SRWLOCK& mx;
    explicit Exclusive(SRWLOCK& m) : mx(m) { AcquireSRWLockExclusive(&mx); }
    ~Exclusive() { ReleaseSRWLockExclusive(&mx); }
};

int64_t qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

void notice(const std::string& text) {
    if (gNotes.size() == 32) gNotes.pop_front();
    gNotes.push_back({text});
}

int fail(std::string& err, const std::string& text) {
    err = text;
    notice(text);
    ++gProblems;
    return -1;
}

Sub* lookup(int id) {
    if (id < 0 || id / hk::kMaxSubs >= gCount) return nullptr;
    auto& t = *gTargets[size_t(id / hk::kMaxSubs)];
    int i = id % hk::kMaxSubs;
    return i < t.count ? t.subs[size_t(i)].get() : nullptr;
}

bool same(const hk::Arg& a, const hk::Arg& b) {
    return a.kind == b.kind && (a.kind != hk::Snapshot || a.type.p == b.type.p);
}

uint32_t payloadSize(const hk::Arg& a) {
    return a.kind == hk::Snapshot ? a.type.size() : a.kind == hk::CStr ? hk::kMaxStr : 0;
}

bool validate(const std::vector<hk::Arg>& args, uint32_t capacity, std::string& err) {
    if (args.size() > hk::kMaxArgs || !capacity || capacity > hk::kMaxQueue) {
        fail(err, "hooks need 0..8 arguments and a queue capacity of 1..4096");
        return false;
    }
    for (const auto& a : args) {
        if (a.kind > hk::CStr) { fail(err, "unknown hook argument kind"); return false; }
        if (a.kind != hk::Snapshot) continue;
        if (!a.type || (a.type.prim() != refl::Struct && a.type.prim() != refl::StaticArray) ||
            !a.type.size() || a.type.size() > hk::kMaxSnapshot || !refl::plain(a.type)) {
            fail(err, "hook snapshots require a fixed-size struct or array of at most 1024 bytes");
            return false;
        }
    }
    return true;
}

void discard(Sub& s) {
    s.discarded += s.write.count + s.inDelivery;
    s.write.clear();
    s.inDelivery = s.pending = 0;
    ++s.generation;
}

void deactivate(Target& t, Sub& s) {
    Exclusive queue(s.mx);
    discard(s);
    if (s.active) {
        s.active = false;
        --t.live;
    }
}

void collectFaults() {
    for (int i = 0; i < gCount; ++i) {
        auto& t = *gTargets[size_t(i)];
        if (!t.faultPending.exchange(false)) continue;
        Exclusive meta(t.mx);
        for (int j = 0; j < t.count; ++j) deactivate(t, *t.subs[size_t(j)]);
        char line[128];
        snprintf(line, sizeof line, "function at %p disabled after an invalid argument read; restart to retry",
                 reinterpret_cast<void*>(t.fn));
        notice(line);
    }
}

std::unique_ptr<Sub> makeSub(Target& t, const std::string& key, const std::vector<hk::Arg>& args, uint32_t cap) {
    auto s = std::make_unique<Sub>();
    s->target = &t;
    s->key = key;
    s->capacity = cap;
    s->count = uint32_t(args.size());
    std::copy(args.begin(), args.end(), s->args.begin());
    for (const auto& a : args) s->payloadSize += payloadSize(a);
    for (Buffer* b : {&s->write, &s->read}) {
        b->events = std::make_unique<hk::Event[]>(cap);
        b->payload = std::make_unique<uint8_t[]>(size_t(cap) * s->payloadSize);
    }
    return s;
}

extern "C" void hk_common();

void restoreCopies(uintptr_t start, uint8_t* copy, size_t size) {
    auto restore = [&](const Code& c) {
        if (!c.start) return;
        for (size_t i = 0; i < sizeof c.bytes; ++i) {
            uintptr_t at = c.start + i;
            if (at >= start && at - start < size) copy[at - start] = c.bytes[i];
        }
    };
    for (const auto& r : gReserved) restore(r.code);
    for (int i = 0; i < gCount; ++i) restore(gTargets[size_t(i)]->code);
}

int readFault(DWORD code) {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ||
           code == STATUS_GUARD_PAGE_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

Code saveCode(uintptr_t fn) {
    Code c;
    __try {
        memcpy(c.bytes, reinterpret_cast<const void*>(fn - 5), sizeof c.bytes);
        c.start = fn - 5;
    } __except (readFault(GetExceptionCode())) {}
    // An adjacent retained target may already have patched part of this saved span.
    if (c.start) restoreCopies(c.start, c.bytes, sizeof c.bytes);
    return c;
}

bool install(Target& t, std::string& err) {
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        fail(err, std::string("MinHook initialization failed: ") + MH_StatusToString(status));
        return false;
    }
    t.code = saveCode(t.fn);
    if (!t.code.start) { fail(err, "hook entry is unreadable"); return false; }
    // No stack changes in the thunk; the common stub owns the unwindable frame.
    uint8_t code[] = {0x48,0xb8,0,0,0,0,0,0,0,0, 0xff,0x25,0,0,0,0, 0,0,0,0,0,0,0,0};
    auto ctx = reinterpret_cast<uintptr_t>(&t), common = reinterpret_cast<uintptr_t>(&hk_common);
    memcpy(code + 2, &ctx, 8);
    memcpy(code + 16, &common, 8);
    t.thunk = VirtualAlloc(nullptr, sizeof code, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!t.thunk) { fail(err, "could not allocate the hook thunk"); return false; }
    memcpy(t.thunk, code, sizeof code);
    DWORD old;
    if (!VirtualProtect(t.thunk, sizeof code, PAGE_EXECUTE_READ, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), t.thunk, sizeof code)) {
        VirtualFree(t.thunk, 0, MEM_RELEASE);
        t.thunk = nullptr;
        fail(err, "could not publish the hook thunk");
        return false;
    }
    status = MH_CreateHook(reinterpret_cast<void*>(t.fn), t.thunk, &t.trampoline);
    if (status != MH_OK) {
        VirtualFree(t.thunk, 0, MEM_RELEASE);
        t.thunk = nullptr;
        fail(err, std::string("could not create hook: ") + MH_StatusToString(status));
        return false;
    }
    return true;
}

int targetFor(uintptr_t fn) {
    for (int i = 0; i < gCount; ++i) if (gTargets[size_t(i)]->fn == fn) return i;
    return -1;
}

// Patterns match the original bytes, so a target 1-4 bytes from a detoured entry would relocate that detour's jump.
bool nearEntry(uintptr_t fn) {
    auto hit = [fn](uintptr_t at) { return at && at != fn && (fn > at ? fn - at : at - fn) < 5; };
    for (const auto& r : gReserved) if (hit(r.fn)) return true;
    for (int i = 0; i < gCount; ++i) if (hit(gTargets[size_t(i)]->fn)) return true;
    return false;
}

int matchSub(Target& t, const std::string& key, const std::vector<hk::Arg>& args, uint32_t cap, std::string& err) {
    int found = -1;
    for (int i = 0; i < t.count; ++i) {
        const auto& s = *t.subs[size_t(i)];
        if (s.active) for (size_t a = 0; a < args.size(); ++a) {
            if (args[a].kind != hk::Skip && s.args[a].kind != hk::Skip && !same(args[a], s.args[a])) {
                fail(err, "conflicting hook declaration at argument " + std::to_string(a + 1) + " with " + s.key);
                return -2;
            }
        }
        if (s.key == key && s.capacity == cap && s.count == args.size() &&
            std::equal(args.begin(), args.end(), s.args.begin(), same)) found = i;
    }
    return found;
}

bool keyActive(const std::string& key) {
    for (int i = 0; i < gCount; ++i) {
        auto& t = *gTargets[size_t(i)];
        for (int j = 0; j < t.count; ++j) if (t.subs[size_t(j)]->active && t.subs[size_t(j)]->key == key) return true;
    }
    return false;
}

int addSub(int idx, const std::string& key, const std::vector<hk::Arg>& args, uint32_t cap, std::string& err) {
    auto& t = *gTargets[size_t(idx)];
    if (t.faulted) return fail(err, "this hook faulted; restart the game before trying it again");
    int n = matchSub(t, key, args, cap, err);
    if (n == -2) return -1;
    size_t bytes = 0;
    std::unique_ptr<Sub> fresh;
    if (n < 0) {
        if (t.count == hk::kMaxSubs) return fail(err, "hook target has eight retained registrations; restart to reclaim them");
        size_t payload = 0;
        for (const auto& a : args) payload += payloadSize(a);
        bytes = 3 * size_t(cap) * (sizeof(hk::Event) + payload);
        if (bytes > hk::kMemoryBudget - gBytes) return fail(err, "hook queue storage would exceed the 64 MiB budget");
        fresh = makeSub(t, key, args, cap);
        n = t.count;
    }
    Exclusive meta(t.mx);
    if (fresh) {
        t.subs[size_t(n)] = std::move(fresh);
        ++t.count;
        gBytes += bytes;
    }
    auto& s = *t.subs[size_t(n)];
    Exclusive queue(s.mx);
    discard(s);
    s.active = true;
    ++t.live;
    return idx * hk::kMaxSubs + n;
}

struct Recipient { Sub* sub; uint64_t gen; };
struct Capture {
    std::array<hk::Arg, hk::kMaxArgs> args{};
    Recipient subs[hk::kMaxSubs]{};
    int count = 0;
    hk::Event event{};
    uint8_t payload[hk::kMaxArgs * hk::kMaxSnapshot];
    uint32_t used = 0;
};

void recipients(Target& t, Capture& c) {
    if (!TryAcquireSRWLockShared(&t.mx)) { ++t.lockWaits; AcquireSRWLockShared(&t.mx); }
    if (!t.faulted) for (int i = 0; i < t.count; ++i) {
        auto& s = *t.subs[size_t(i)];
        if (!s.active) continue;
        c.subs[c.count++] = {&s, s.generation};
        for (uint32_t a = 0; a < s.count; ++a) if (s.args[a].kind != hk::Skip) c.args[a] = s.args[a];
    }
    ReleaseSRWLockShared(&t.mx);
}

uint64_t rawArg(const Saved& s, int pos, bool fp) {
    uint64_t bits;
    if (pos >= 4) memcpy(&bits, reinterpret_cast<const uint8_t*>(&s.ret) + 0x28 + (pos - 4) * 8, 8);
    else if (fp) memcpy(&bits, s.xmm[pos], 8);
    else {
        const uint64_t regs[] = {s.rcx, s.rdx, s.r8, s.r9};
        bits = regs[pos];
    }
    return bits;
}

uint64_t scalar(uint64_t bits, hk::Kind k) {
    switch (k) {
    case hk::Bool: return uint8_t(bits) != 0;
    case hk::U8: return uint8_t(bits);
    case hk::U16: return uint16_t(bits);
    case hk::U32: case hk::Entity: case hk::F32: return uint32_t(bits);
    case hk::I8: return uint64_t(int64_t(int8_t(bits)));
    case hk::I16: return uint64_t(int64_t(int16_t(bits)));
    case hk::I32: return uint64_t(int64_t(int32_t(bits)));
    default: return bits;
    }
}

void readArg(Capture& c, const Saved& saved, int i) {
    const auto& arg = c.args[size_t(i)];
    if (arg.kind == hk::Skip) return;
    uint64_t bits = rawArg(saved, i, arg.kind == hk::F32 || arg.kind == hk::F64);
    auto& v = c.event.v[i];
    if (arg.kind != hk::EntityPtr && arg.kind != hk::Snapshot && arg.kind != hk::CStr) {
        v.bits = scalar(bits, arg.kind);
        return;
    }
    if (!bits) return;
    const auto* p = reinterpret_cast<const uint8_t*>(bits);
    if (arg.kind == hk::EntityPtr) {
        uint32_t id;
        memcpy(&id, p, 4);
        v.bits = id;
        v.len = 4;
        return;
    }
    v.bits = 1;
    v.off = c.used;
    if (arg.kind == hk::Snapshot) {
        v.len = arg.type.size();
        memcpy(c.payload + c.used, p, v.len);
    } else while (v.len < hk::kMaxStr) {
        uint8_t ch = p[v.len];
        if (!ch) break;
        c.payload[c.used + v.len++] = ch;
    }
    c.used += v.len;
}

bool readArgs(Capture& c, const Saved& saved) {
    __try {
        for (int i = 0; i < hk::kMaxArgs; ++i) readArg(c, saved, i);
        return true;
    } __except (readFault(GetExceptionCode())) {
        return false;
    }
}

void append(Sub& s, const Capture& c) {
    auto& b = s.write;
    if (b.count == s.capacity) { ++s.dropped; ++s.pending; return; }
    auto& e = b.events[b.count++];
    e = c.event;
    for (int i = 0; i < hk::kMaxArgs; ++i) {
        auto kind = s.args[size_t(i)].kind;
        if (kind == hk::Skip) { e.v[i] = {}; continue; }
        if (kind != hk::Snapshot && kind != hk::CStr) continue;
        auto& v = e.v[i];
        if (v.len) memcpy(b.payload.get() + b.used, c.payload + v.off, v.len);
        v.off = b.used;
        b.used += v.len;
    }
    s.highWater = std::max(s.highWater, b.count);
}

void publish(const Capture& c) {
    for (int i = 0; i < c.count; ++i) {
        auto& r = c.subs[i];
        auto& s = *r.sub;
        if (!TryAcquireSRWLockExclusive(&s.mx)) { ++s.lockWaits; AcquireSRWLockExclusive(&s.mx); }
        if (s.active && s.generation == r.gen && !s.target->faulted) append(s, c);
        else ++s.discarded;
        ReleaseSRWLockExclusive(&s.mx);
    }
}

void fault(Target& t) {
    if (!t.faulted.exchange(true)) {
        t.faultPending = true;
        ++gProblems;
    }
}

// Kept out of hk_capture: removed hooks stay installed, and their calls shouldn't pay for the ~8.6 KB Capture frame.
__declspec(noinline) void capture(Target& t, const Saved& saved) noexcept {
    Capture c;
    c.event.qpc = qpc();
    recipients(t, c);
    if (!c.count) return;
    bool timing = t.timing;
    c.event.seq = ++gSeq;
    ++t.calls;
    if (readArgs(c, saved)) { ++t.decoded; publish(c); }
    else fault(t);
    if (timing) { t.ticks += uint64_t(qpc() - c.event.qpc); ++t.timedCalls; }
}

}  // namespace

extern "C" void hk_capture(Target* target, const Saved* saved) noexcept {
    if (target->live.load(std::memory_order_acquire) && !target->faulted) capture(*target, *saved);
}

void hk::setLog(Log log) { gLog = log; }

void hk::refuse(const std::string& message) {
    std::lock_guard lock(gMx);
    notice(message);
    ++gProblems;
}

void hk::flushLogs() {
    std::vector<std::string> lines;
    {
        std::lock_guard lock(gMx);
        collectFaults();
        for (auto& n : gNotes) if (!std::exchange(n.logged, true)) lines.push_back(n.text);
    }
    if (auto log = gLog.load()) for (const auto& line : lines) log((LV_BAD "Hooks: " + line).c_str());
}

std::vector<std::string> hk::diagnostics() {
    std::lock_guard lock(gMx);
    collectFaults();
    std::vector<std::string> result;
    for (const auto& n : gNotes) result.push_back(n.text);
    return result;
}

uint64_t hk::problems() { return gProblems; }
double hk::seconds(int64_t stamp) { return double(stamp - gEpoch) / double(gFreq); }
size_t hk::retainedBytes() { std::lock_guard lock(gMx); return gBytes; }

void hk::reserve(uintptr_t fn, const char* what) {
    if (!fn) return;
    std::lock_guard lock(gMx);
    gReserved.push_back({fn, what, saveCode(fn)});
}

void hk::restoreCode(uintptr_t start, uint8_t* copy, size_t size) {
    std::lock_guard lock(gMx);
    restoreCopies(start, copy, size);
}

int hk::subscribe(uintptr_t fn, const std::string& key, const std::vector<Arg>& args, uint32_t cap, std::string& err) {
    std::lock_guard lock(gMx);
    err.clear();
    collectFaults();
    if (!fn || key.empty()) return fail(err, "hook target and registration key must not be empty");
    if (!validate(args, cap, err)) return -1;
    for (const auto& r : gReserved) if (r.fn == fn) return fail(err, "target is reserved by SET: " + r.name);
    if (nearEntry(fn)) return fail(err, "hook target overlaps the entry of another hooked function");
    if (keyActive(key)) return fail(err, "hook name is already active: " + key);
    int idx = targetFor(fn);
    bool fresh = idx < 0;
    try {
        if (fresh) {
            if (gCount == kMaxTargets) return fail(err, "64 hook targets are already retained; restart to reclaim them");
            idx = gCount;
            auto t = std::make_unique<Target>();
            t->fn = fn;
            gTargets[size_t(idx)] = std::move(t);
            ++gCount;
        }
        int id = addSub(idx, key, args, cap, err);
        if (id < 0) { if (fresh) gTargets[size_t(--gCount)].reset(); return -1; }
        if (!fresh) return id;
        auto& t = *gTargets[size_t(idx)];
        if (!gEpoch) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); gFreq = f.QuadPart; gEpoch = qpc(); }
        if (install(t, err)) {
            auto status = MH_EnableHook(reinterpret_cast<void*>(fn));
            if (status == MH_OK) return id;
            fail(err, std::string("could not enable hook: ") + MH_StatusToString(status));
            // MinHook's hook already points at this Target's thunk, so the Target stays, marked faulted.
            t.faulted = true;
            Exclusive meta(t.mx);
            deactivate(t, *t.subs[0]);
            return -1;
        }
        gBytes -= 3 * size_t(cap) * (sizeof(Event) + t.subs[0]->payloadSize);
        gTargets[size_t(--gCount)].reset();
    } catch (const std::bad_alloc&) {
        if (fresh && gCount > idx && !gTargets[size_t(idx)]->trampoline) gTargets[size_t(--gCount)].reset();
        return fail(err, "could not allocate hook storage");
    }
    return -1;
}

void hk::unsubscribe(int id) {
    std::lock_guard lock(gMx);
    if (auto s = lookup(id)) { Exclusive meta(s->target->mx); deactivate(*s->target, *s); }
}

void hk::unsubscribeAll() {
    std::lock_guard lock(gMx);
    for (int i = 0; i < gCount; ++i) {
        auto& t = *gTargets[size_t(i)];
        Exclusive meta(t.mx);
        for (int j = 0; j < t.count; ++j) deactivate(t, *t.subs[size_t(j)]);
    }
}

void hk::discardPending() {
    std::lock_guard lock(gMx);
    for (int i = 0; i < gCount; ++i) {
        auto& t = *gTargets[size_t(i)];
        Exclusive meta(t.mx);
        for (int j = 0; j < t.count; ++j) { Exclusive queue(t.subs[size_t(j)]->mx); discard(*t.subs[size_t(j)]); }
    }
}

bool hk::drain(int id, Batch& out) {
    Sub* s;
    { std::lock_guard lock(gMx); s = lookup(id); }
    out.events.clear();
    out.payload.clear();
    if (!s) return false;
    uint32_t count, used;
    uint64_t lost;
    {
        Exclusive queue(s->mx);
        if (!s->active || s->target->faulted) { discard(*s); return false; }
        s->discarded += std::exchange(s->inDelivery, 0);
        std::swap(s->write, s->read);
        s->write.clear();
        count = s->read.count;
        used = s->read.used;
        out.generation = s->generation;
        lost = count ? std::exchange(s->pending, 0) : 0;
        s->inDelivery = count;
    }
    // The single consumer owns read until its next drain. Workers only touch write.
    out.events.assign(s->read.events.get(), s->read.events.get() + count);
    if (used) out.payload.assign(s->read.payload.get(), s->read.payload.get() + used);
    std::sort(out.events.begin(), out.events.end(), [](const Event& a, const Event& b) { return a.seq < b.seq; });
    if (count) out.events[0].dropped = lost;
    return count != 0;
}

bool hk::delivered(int id, uint64_t gen) {
    Sub* s;
    { std::lock_guard lock(gMx); s = lookup(id); }
    if (!s) return false;
    Exclusive queue(s->mx);
    if (!s->active || s->target->faulted || s->generation != gen || !s->inDelivery) return false;
    --s->inDelivery;
    ++s->delivered;
    return true;
}

std::vector<hk::TargetInfo> hk::info() {
    std::lock_guard lock(gMx);
    collectFaults();
    std::vector<TargetInfo> result;
    for (int i = 0; i < gCount; ++i) {
        auto& t = *gTargets[size_t(i)];
        TargetInfo item{t.fn, t.faulted, t.timing, t.calls, t.ticks, t.timedCalls, t.decoded, t.lockWaits, {}};
        for (int j = 0; j < t.count; ++j) {
            auto& s = *t.subs[size_t(j)];
            Exclusive queue(s.mx);
            item.subs.push_back({s.key, s.active, s.delivered, s.dropped, s.discarded, s.lockWaits, s.highWater, s.capacity});
        }
        result.push_back(std::move(item));
    }
    return result;
}

void hk::setTiming(uintptr_t fn, bool on) {
    std::lock_guard lock(gMx);
    int i = targetFor(fn);
    if (i >= 0) gTargets[size_t(i)]->timing = on;
}
