#include "hooks.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "MinHook.h"
#include "level.h"
#include <algorithm>
#include <bit>
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
    uint8_t shadow[32];
    uint64_t out[4];  // stack arguments 5 to 8 when the stub calls the original itself (hk_entry returned 2)
    uint8_t xmm[6][16];
    uint64_t ctx, r11, r10, r9, r8, rdx, rcx;
    uint64_t afterOrder, afterMask;  // the entry's rule order and which of its positions change the return value
    uint64_t ret;
};
static_assert(offsetof(Saved, out) == 0x20 && offsetof(Saved, xmm) == 0x40 && offsetof(Saved, ctx) == 0xa0);
static_assert(offsetof(Saved, rcx) == 0xd0 && offsetof(Saved, afterOrder) == 0xd8 && offsetof(Saved, ret) == 0xe8);

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
// A retained rule registration. The calling threads read only rule, floatMask and declMask, which never change once
// the slot is published; the rest belongs to the control side, under gMx.
struct RuleSlot {
    std::unique_ptr<rules::Rule> rule;
    std::string key;
    std::array<hk::Arg, hk::kMaxArgs> args{};
    uint32_t count = 0;
    uint8_t floatMask = 0, declMask = 0;  // positions read from XMM; positions a rule reads at all
    uint32_t order = 0;
    uint64_t seq = 0, loggedFaults = 0;
    bool active = false, faulted = false;
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
    bool dead = false;  // MinHook couldn't enable its detour: nothing registers on it again
    std::atomic<uint64_t> calls{0}, ticks{0}, timedCalls{0}, decoded{0}, lockWaits{0};
    std::array<RuleSlot, rules::kMaxRules> rslots;
    int ruleCount = 0;
    std::atomic<uint64_t> order{0};  // the active rules, in order (rules::packOrder)
    std::atomic<bool> ruleFaultPending{false};
    std::atomic<uint64_t> ruleCalls{0}, ruleTicks{0}, ruleTimed{0};
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
uint64_t gRuleSeq = 0;
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

// The active, unfaulted rules sorted by mod order, then registration, as the target's order word. One atomic store, so
// a call sees either the old order or the new one.
void publishOrder(Target& t) {
    uint8_t slots[rules::kMaxRules];
    int n = 0;
    for (int i = 0; i < t.ruleCount; ++i)
        if (t.rslots[size_t(i)].active && !t.rslots[size_t(i)].faulted) slots[n++] = uint8_t(i);
    std::sort(slots, slots + n, [&](uint8_t a, uint8_t b) {
        const auto &x = t.rslots[a], &y = t.rslots[b];
        return x.order != y.order ? x.order < y.order : x.seq < y.seq;
    });
    t.order.store(rules::packOrder(slots, n), std::memory_order_release);
}

void collectRuleFaults(Target& t) {
    if (!t.ruleFaultPending.exchange(false)) return;
    for (int i = 0; i < t.ruleCount; ++i) {
        auto& r = t.rslots[size_t(i)];
        uint64_t faults = r.rule->faults;
        if (faults == r.loggedFaults) continue;
        r.loggedFaults = faults;
        r.faulted = true;
        notice("Rule " + r.key + " turned off after an invalid field read; restart to retry it");
    }
    publishOrder(t);
}

void collectFaults() {
    for (int i = 0; i < gCount; ++i) {
        auto& t = *gTargets[size_t(i)];
        collectRuleFaults(t);
        if (!t.faultPending.exchange(false)) continue;
        Exclusive meta(t.mx);
        for (int j = 0; j < t.count; ++j) deactivate(t, *t.subs[size_t(j)]);
        char line[128];
        snprintf(line, sizeof line, "Function at %p disabled after an invalid argument read; restart to retry",
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

// Every non-skipped position agrees with the target's active observers and rules.
bool agrees(Target& t, const std::vector<hk::Arg>& args, std::string& err) {
    auto check = [&](const hk::Arg* other, uint32_t n, const std::string& key) {
        for (size_t a = 0; a < args.size() && a < n; ++a)
            if (args[a].kind != hk::Skip && other[a].kind != hk::Skip && !same(args[a], other[a])) {
                fail(err, "conflicting hook declaration at argument " + std::to_string(a + 1) + " with " + key);
                return false;
            }
        return true;
    };
    for (int i = 0; i < t.count; ++i) {
        const auto& s = *t.subs[size_t(i)];
        if (s.active && !check(s.args.data(), s.count, s.key)) return false;
    }
    for (int i = 0; i < t.ruleCount; ++i) {
        const auto& r = t.rslots[size_t(i)];
        if (r.active && !check(r.args.data(), r.count, r.key)) return false;
    }
    return true;
}

int matchSub(Target& t, const std::string& key, const std::vector<hk::Arg>& args, uint32_t cap, std::string& err) {
    if (!agrees(t, args, err)) return -2;
    int found = -1;
    for (int i = 0; i < t.count; ++i) {
        const auto& s = *t.subs[size_t(i)];
        if (s.key == key && s.capacity == cap && s.count == args.size() &&
            std::equal(args.begin(), args.end(), s.args.begin(), same)) found = i;
    }
    return found;
}

bool keyActive(const std::string& key) {
    for (int i = 0; i < gCount; ++i) {
        auto& t = *gTargets[size_t(i)];
        for (int j = 0; j < t.count; ++j) if (t.subs[size_t(j)]->active && t.subs[size_t(j)]->key == key) return true;
        for (int j = 0; j < t.ruleCount; ++j) if (t.rslots[size_t(j)].active && t.rslots[size_t(j)].key == key) return true;
    }
    return false;
}

int addSub(int idx, const std::string& key, const std::vector<hk::Arg>& args, uint32_t cap, std::string& err) {
    auto& t = *gTargets[size_t(idx)];
    if (t.faulted) return fail(err, "this hook faulted; restart the game before trying it again");
    if (t.dead) return fail(err, "this hook couldn't be enabled; restart the game before trying it again");
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

// Kept out of hk_entry: removed hooks stay installed, and their calls shouldn't pay for the ~8.6 KB Capture frame.
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

// The saved frame is what hk_common restores before jumping to the original, and stack arguments sit in the caller's
// frame, which the original reads: a write to either is the argument the original gets.
void writeArg(Saved& s, int pos, bool fp, uint64_t bits) {
    if (pos >= 4) memcpy(reinterpret_cast<uint8_t*>(&s.ret) + 0x28 + (pos - 4) * 8, &bits, 8);
    else if (fp) memcpy(s.xmm[pos], &bits, 8);
    else {
        uint64_t* regs[] = {&s.rcx, &s.rdx, &s.r8, &s.r9};
        *regs[pos] = bits;
    }
}

// Stage, then commit: the rules run on a copy of the arguments, which is written back only if no rule faulted. The
// saved frame and the caller's stack slots can't fault, so a call gets all of its changes or none.
__declspec(noinline) void applyRules(Target& t, Saved& s, uint64_t word, rules::Result& res) noexcept {
    bool timing = t.timing;
    int64_t start = timing ? qpc() : 0;
    uint8_t slots[rules::kMaxRules];
    rules::Rule* rs[rules::kMaxRules];
    int n = rules::unpackOrder(word, slots);
    uint8_t fp = 0, used = 0;
    for (int i = 0; i < n; ++i) {
        const auto& r = t.rslots[slots[i]];
        rs[i] = r.rule.get();
        fp |= r.floatMask;
        used |= r.declMask;
    }
    rules::Frame in{}, out;
    for (int p = 0; p < hk::kMaxArgs; ++p) if (used >> p & 1) in.a[p] = rawArg(s, p, fp >> p & 1);
    ++t.ruleCalls;
    if (rules::apply(rs, n, in, out, &res) >= 0) {
        if (!t.ruleFaultPending.exchange(true)) ++gProblems;
    } else {
        for (int p = 0; p < hk::kMaxArgs; ++p) if (out.a[p] != in.a[p]) writeArg(s, p, fp >> p & 1, out.a[p]);
    }
    if (timing) { t.ruleTicks += uint64_t(qpc() - start); ++t.ruleTimed; }
}

}  // namespace

// hk_common, on every call of a hooked function: the rules first, so observers see the arguments the original gets.
// 0: run the original. 1: a rule blocked the call, and hk_common returns to the caller with RAX from the saved context
// slot and XMM0 from the saved XMM0, without running the original. 2: rules change the return value, so hk_common
// calls the original and then hk_after.
extern "C" int hk_entry(Target* target, Saved* saved) noexcept {
    uint64_t word = target->order.load(std::memory_order_acquire);
    rules::Result res;
    if (word >> 60) applyRules(*target, *saved, word, res);
    if (target->live.load(std::memory_order_acquire) && !target->faulted) capture(*target, *saved);
    if (!res.blocked) {
        if (!res.after) return 0;
        saved->afterOrder = word;
        saved->afterMask = res.after;
        return 2;
    }
    if (res.fp) {
        memset(saved->xmm[0], 0, 16);
        memcpy(saved->xmm[0], &res.ret, 8);
    } else {
        saved->ctx = res.ret;
    }
    return 1;
}

// After the original returned to hk_common: its result is in the R11 slot (RAX) and the saved XMM0. The rules are the
// ones the entry saw; their storage is never freed, so a rule removed meanwhile still finishes this call.
extern "C" void hk_after(Target* target, Saved* saved) noexcept {
    uint8_t slots[rules::kMaxRules];
    rules::Rule* rs[rules::kMaxRules];
    int n = rules::unpackOrder(saved->afterOrder, slots);
    for (int i = 0; i < n; ++i) rs[i] = target->rslots[slots[i]].rule.get();
    auto mask = uint16_t(saved->afterMask);
    const auto& at = rs[std::countr_zero(mask)]->shape.retAt;
    bool fp = at.num == rules::Float;
    uint64_t bits = saved->r11;
    if (fp) memcpy(&bits, saved->xmm[0], 8);
    bits = rules::after(rs, n, mask, bits);
    if (fp) memcpy(saved->xmm[0], &bits, 8);
    else saved->r11 = bits;
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

namespace {

// Refusals shared by observers and rules, before any target is made.
bool admissible(uintptr_t fn, const std::string& key, std::string& err) {
    if (!fn || key.empty()) { fail(err, "hook target and registration key must not be empty"); return false; }
    for (const auto& r : gReserved) if (r.fn == fn) { fail(err, "target is reserved by SET: " + r.name); return false; }
    if (nearEntry(fn)) { fail(err, "hook target overlaps the entry of another hooked function"); return false; }
    if (keyActive(key)) { fail(err, "hook name is already active: " + key); return false; }
    return true;
}

// The target for fn, made (not yet installed) if there is none; -1 with err set when all 64 are retained.
int makeTarget(uintptr_t fn, bool& fresh, std::string& err) {
    int idx = targetFor(fn);
    fresh = idx < 0;
    if (!fresh) return idx;
    if (gCount == hk::kMaxTargets) return fail(err, "64 hook targets are already retained; restart to reclaim them");
    auto t = std::make_unique<Target>();
    t->fn = fn;
    gTargets[size_t(gCount)] = std::move(t);
    return gCount++;
}

enum Installed { Ok, NotInstalled, NotEnabled };

// A fresh target's detour, after its first registration is in place. NotInstalled: nothing points at the Target, so
// the caller drops it. NotEnabled: MinHook's hook already points at its thunk, so it stays, and the caller turns its
// registration off.
Installed installFresh(Target& t, std::string& err) {
    if (!gEpoch) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); gFreq = f.QuadPart; gEpoch = qpc(); }
    if (!install(t, err)) return NotInstalled;
    auto status = MH_EnableHook(reinterpret_cast<void*>(t.fn));
    if (status == MH_OK) return Ok;
    fail(err, std::string("could not enable hook: ") + MH_StatusToString(status));
    return NotEnabled;
}

}  // namespace

int hk::subscribe(uintptr_t fn, const std::string& key, const std::vector<Arg>& args, uint32_t cap, std::string& err) {
    std::lock_guard lock(gMx);
    err.clear();
    collectFaults();
    if (!validate(args, cap, err) || !admissible(fn, key, err)) return -1;
    bool fresh = false;
    int idx = -1;
    try {
        idx = makeTarget(fn, fresh, err);
        if (idx < 0) return -1;
        int id = addSub(idx, key, args, cap, err);
        if (id < 0) { if (fresh) gTargets[size_t(--gCount)].reset(); return -1; }
        if (!fresh) return id;
        auto& t = *gTargets[size_t(idx)];
        Installed done = installFresh(t, err);
        if (done == Ok) return id;
        if (done == NotEnabled) {
            t.faulted = true;
            t.dead = true;
            Exclusive meta(t.mx);
            deactivate(t, *t.subs[0]);
            return -1;
        }
        gBytes -= 3 * size_t(cap) * (sizeof(Event) + t.subs[0]->payloadSize);
        gTargets[size_t(--gCount)].reset();
    } catch (const std::bad_alloc&) {
        if (fresh && idx >= 0 && gCount > idx && !gTargets[size_t(idx)]->trampoline) gTargets[size_t(--gCount)].reset();
        return fail(err, "could not allocate hook storage");
    }
    return -1;
}

namespace {

bool numLoc(uint8_t prim, uint32_t size, rules::Loc& l) {
    using namespace refl;
    if (prim == Bool) { l.num = rules::Boolean; l.width = 1; return true; }
    if (prim >= U8 && prim <= S64) {
        l.num = (prim - U8) % 2 ? rules::Signed : rules::Unsigned;
        l.width = uint8_t(1 << ((prim - U8) / 2));
        return true;
    }
    if (prim == F32 || prim == F64) { l.num = rules::Float; l.width = prim == F32 ? 4 : 8; return true; }
    if (prim >= Enum && prim <= Bitmask64 && (size == 1 || size == 2 || size == 4 || size == 8)) {
        l.num = rules::Unsigned;
        l.width = uint8_t(size);
        return true;
    }
    return false;
}

// A rule's value at l is the one its argument declaration describes.
bool declared(const rules::Loc& l, const std::vector<hk::Arg>& args, std::string& err) {
    if (l.arg >= args.size() || args[l.arg].kind == hk::Skip)
        return fail(err, "rule uses argument " + std::to_string(l.arg + 1) + ", which isn't declared"), false;
    rules::Loc want;
    bool ok;
    if (l.field) {
        const auto& a = args[l.arg];
        ok = (a.kind == hk::EntityPtr && l.offset == 0 && l.width == 4 && l.num == rules::Unsigned) ||
             (a.kind == hk::Snapshot && uint64_t(l.offset) + l.width <= a.type.size());
    } else {
        ok = hk::argLoc(args[l.arg], l.arg, want) && want == l;
    }
    if (!ok) fail(err, "rule value doesn't match argument " + std::to_string(l.arg + 1) + "'s declaration");
    return ok;
}

bool listed(const rules::Loc& l, const std::vector<rules::Loc>& fields) {
    return std::find(fields.begin(), fields.end(), l) != fields.end();
}

bool checkRule(const hk::RuleDecl& d, std::string& err) {
    if (!validate(d.args, 1, err)) return false;
    const auto& s = d.shape;
    if (s.conds > rules::kMaxConds || s.changes > rules::kMaxChanges || d.params.size() > rules::kMaxParams ||
        d.list.size() > rules::kMaxList)
        return fail(err, "rules hold at most 8 conditions, 8 changes, 8 params and 64 ids"), false;
    auto operand = [&](const rules::Operand& o) {
        return o.src != rules::Param || o.param < d.params.size() ? true : (fail(err, "rule uses an unset param"), false);
    };
    if (s.block) {
        if (!d.block) return fail(err, "blocking a call needs a catalog target that allows it"), false;
        if (s.changes || s.afters) return fail(err, "a rule that blocks changes nothing: the original doesn't run"), false;
        if (!operand(s.ret)) return false;
    } else if (!s.changes && !s.afters) {
        return fail(err, "a rule needs at least one change, a block or an after"), false;
    }
    if (s.afters > rules::kMaxChanges) return fail(err, "rules hold at most 8 after changes"), false;
    if (s.afters && !d.after) return fail(err, "changing a return value needs a catalog target that allows it"), false;
    for (int i = 0; i < s.afters; ++i) {
        if (!(s.after[i].at == s.retAt)) return fail(err, "after changes take the return kind"), false;
        if (!operand(s.after[i].v)) return false;
    }
    for (int i = 0; i < s.conds; ++i)
        if ((!listed(s.cond[i].at, d.fields) && !listed(s.cond[i].at, d.reads) && !declared(s.cond[i].at, d.args, err)) ||
            (s.cond[i].cmp != rules::In && !operand(s.cond[i].v)))
            return false;
    for (int i = 0; i < s.changes; ++i) {
        const auto& at = s.change[i].at;
        if (at.field && !listed(at, d.fields))
            return fail(err, d.fields.empty() ? "changing a field needs a catalog target"
                                              : "the catalog doesn't let rules change that field"), false;
        if ((!at.field && !declared(at, d.args, err)) || !operand(s.change[i].v)) return false;
    }
    return true;
}

std::string modOf(const std::string& key) { return key.substr(0, key.find('/')); }

// Another mod's active rule on t that changes a value slot n changes too.
std::string conflictWith(const Target& t, int n) {
    const auto& me = t.rslots[size_t(n)];
    for (int i = 0; i < t.ruleCount; ++i) {
        const auto& r = t.rslots[size_t(i)];
        if (i != n && r.active && modOf(r.key) != modOf(me.key) && rules::writesSame(r.rule->shape, me.rule->shape))
            return r.key;
    }
    return {};
}

// A retained slot with the same key, arguments and shape is reused; otherwise a new one is made.
int ruleSlot(Target& t, const hk::RuleDecl& d, std::string& err) {
    for (int i = 0; i < t.ruleCount; ++i) {
        auto& r = t.rslots[size_t(i)];
        if (r.active || r.key != d.key || r.count != d.args.size() || !(r.rule->shape == d.shape) ||
            !std::equal(d.args.begin(), d.args.end(), r.args.begin(), same))
            continue;
        if (r.faulted) return fail(err, "this rule faulted; restart the game before trying it again");
        return i;
    }
    if (t.ruleCount == rules::kMaxRules) return fail(err, "hook target has 15 retained rules; restart to reclaim them");
    if (sizeof(rules::Rule) > hk::kMemoryBudget - gBytes) return fail(err, "rule storage would exceed the 64 MiB budget");
    auto& r = t.rslots[size_t(t.ruleCount)];
    r.rule = std::make_unique<rules::Rule>();
    r.rule->shape = d.shape;
    r.key = d.key;
    r.count = uint32_t(d.args.size());
    std::copy(d.args.begin(), d.args.end(), r.args.begin());
    for (size_t p = 0; p < d.args.size(); ++p) {
        if (d.args[p].kind == hk::F32 || d.args[p].kind == hk::F64) r.floatMask |= uint8_t(1 << p);
        if (d.args[p].kind != hk::Skip) r.declMask |= uint8_t(1 << p);
    }
    for (const auto& f : d.fields) r.declMask |= uint8_t(1 << f.arg);  // a catalog field's pointer may be a skip
    for (const auto& f : d.reads) r.declMask |= uint8_t(1 << f.arg);
    gBytes += sizeof(rules::Rule);
    return t.ruleCount++;
}

int addRuleSlot(int idx, const hk::RuleDecl& d, std::string& err, std::string& conflict) {
    auto& t = *gTargets[size_t(idx)];
    if (t.dead) return fail(err, "this hook couldn't be enabled; restart the game before trying it again");
    if (!agrees(t, d.args, err)) return -1;
    int n = ruleSlot(t, d, err);
    if (n < 0) return -1;
    auto& r = t.rslots[size_t(n)];
    for (int i = 0; i < rules::kMaxParams; ++i) rules::setParam(*r.rule, i, size_t(i) < d.params.size() ? d.params[size_t(i)] : 0);
    rules::setList(*r.rule, d.list.data(), uint32_t(d.list.size()));
    r.order = d.order;
    r.seq = ++gRuleSeq;
    r.active = true;
    r.rule->active = true;
    conflict = conflictWith(t, n);
    publishOrder(t);
    return idx * rules::kMaxRules + n;
}

RuleSlot* ruleById(int id) {
    if (id < 0 || id / rules::kMaxRules >= gCount) return nullptr;
    auto& t = *gTargets[size_t(id / rules::kMaxRules)];
    int i = id % rules::kMaxRules;
    return i < t.ruleCount ? &t.rslots[size_t(i)] : nullptr;
}

void deactivateRules(Target& t) {
    for (int i = 0; i < t.ruleCount; ++i) {
        t.rslots[size_t(i)].active = false;
        t.rslots[size_t(i)].rule->active = false;
    }
    publishOrder(t);
}

}  // namespace

bool hk::validRule(const RuleDecl& d, std::string& err) {
    std::lock_guard lock(gMx);
    err.clear();
    return checkRule(d, err);
}

bool hk::kindOf(std::string_view name, Kind& out) {
    static const std::pair<const char*, Kind> kinds[] = {
        {"skip", Skip}, {"bool", Bool}, {"u8", U8}, {"u16", U16}, {"u32", U32}, {"u64", U64}, {"ptrsize", U64},
        {"i8", I8}, {"i16", I16}, {"i32", I32}, {"i64", I64}, {"f32", F32}, {"f64", F64}, {"entity", Entity},
        {"entity*", EntityPtr}, {"cstr", CStr}};
    for (const auto& k : kinds)
        if (name == k.first) { out = k.second; return true; }
    return false;
}

bool hk::argLoc(const Arg& a, uint8_t pos, rules::Loc& out) {
    out = {};
    out.arg = pos;
    switch (a.kind) {
    case Bool: return numLoc(refl::Bool, 1, out);
    case U8: case U16: case U32: case U64: return numLoc(uint8_t(refl::U8 + 2 * (a.kind - U8)), 0, out);
    case I8: case I16: case I32: case I64: return numLoc(uint8_t(refl::S8 + 2 * (a.kind - I8)), 0, out);
    case F32: return numLoc(refl::F32, 4, out);
    case F64: return numLoc(refl::F64, 8, out);
    case Entity: return numLoc(refl::U32, 4, out);
    default: return false;
    }
}

bool hk::fieldLoc(const Arg& a, uint8_t pos, std::string_view path, rules::Loc& out, std::string& err) {
    out = {};
    out.arg = pos;
    out.field = true;
    if (a.kind == EntityPtr && path.empty()) return numLoc(refl::U32, 4, out);
    if (a.kind != Snapshot) { err = "argument " + std::to_string(pos + 1) + " doesn't point at a reflected type"; return false; }
    refl::Type leaf;
    if (!refl::path(a.type, path, out.offset, leaf)) {
        err = std::string(a.type.qname()) + " has no field '" + std::string(path) + "'";
        return false;
    }
    if (numLoc(leaf.prim(), leaf.size(), out)) return true;
    err = "field '" + std::string(path) + "' isn't a number";
    return false;
}

int hk::addRule(uintptr_t fn, const RuleDecl& d, std::string& err, std::string& conflict) {
    std::lock_guard lock(gMx);
    err.clear();
    conflict.clear();
    collectFaults();
    if (!checkRule(d, err) || !admissible(fn, d.key, err)) return -1;
    bool fresh = false;
    int idx = -1;
    try {
        idx = makeTarget(fn, fresh, err);
        if (idx < 0) return -1;
        int id = addRuleSlot(idx, d, err, conflict);
        if (id < 0) { if (fresh) gTargets[size_t(--gCount)].reset(); return -1; }
        if (!fresh) return id;
        auto& t = *gTargets[size_t(idx)];
        Installed done = installFresh(t, err);
        if (done == Ok) return id;
        if (done == NotEnabled) { t.dead = true; deactivateRules(t); return -1; }
        gBytes -= sizeof(rules::Rule);
        gTargets[size_t(--gCount)].reset();
    } catch (const std::bad_alloc&) {
        if (fresh && idx >= 0 && gCount > idx && !gTargets[size_t(idx)]->trampoline) gTargets[size_t(--gCount)].reset();
        return fail(err, "could not allocate rule storage");
    }
    return -1;
}

void hk::removeRule(int id) {
    std::lock_guard lock(gMx);
    RuleSlot* r = ruleById(id);
    if (!r || !r->active) return;
    r->active = false;
    r->rule->active = false;
    publishOrder(*gTargets[size_t(id / rules::kMaxRules)]);
}

bool hk::setParam(int id, int i, double v) {
    std::lock_guard lock(gMx);
    RuleSlot* r = ruleById(id);
    if (!r || !r->active || i < 0 || i >= rules::kMaxParams) return false;
    rules::setParam(*r->rule, i, v);
    return true;
}

bool hk::setList(int id, const std::vector<uint32_t>& ids) {
    std::lock_guard lock(gMx);
    RuleSlot* r = ruleById(id);
    if (!r || !r->active || ids.size() > rules::kMaxList) return false;
    rules::setList(*r->rule, ids.data(), uint32_t(ids.size()));
    return true;
}

void hk::unsubscribe(int id) {
    std::lock_guard lock(gMx);
    if (auto s = lookup(id)) { Exclusive meta(s->target->mx); deactivate(*s->target, *s); }
}

void hk::unsubscribeAll() {
    std::lock_guard lock(gMx);
    for (int i = 0; i < gCount; ++i) {
        auto& t = *gTargets[size_t(i)];
        deactivateRules(t);
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
        item.ruleCalls = t.ruleCalls;
        item.ruleTicks = t.ruleTicks;
        item.ruleTimed = t.ruleTimed;
        for (int j = 0; j < t.ruleCount; ++j) {
            const auto& r = t.rslots[size_t(j)];
            item.rules.push_back({r.key, r.active ? conflictWith(t, j) : "", r.active, r.faulted, r.rule->hits, r.rule->faults, r.rule->refused});
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
