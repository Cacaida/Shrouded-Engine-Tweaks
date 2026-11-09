// Real detours on local functions: no game process is touched.
#include "hooks.h"

#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <thread>
#include <vector>

#include "contracts.h"
#include "rules.h"

extern "C" int hk_test_abi(void* target);
extern "C" void hk_common();
extern "C" uint64_t hk_adjacent_a(uint64_t);
extern "C" uint64_t hk_adjacent_b(uint64_t);
extern "C" uint64_t hk_foreign_target(uint64_t);
extern "C" uint64_t hk_foreign_detour(uint64_t);

namespace {

#define HCHECK(c) do { if (!(c)) { printf("FAIL hooks line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

__declspec(noinline) uint64_t integers(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                                     uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8;
}
auto volatile callInts = &integers;

__declspec(noinline) double mixed(int a, double b, int c, float d, double e, int f, float g, double h) {
    return a + b + c + d + e + f + g + h;
}
auto volatile callMixed = &mixed;

__declspec(noinline) uint64_t pointers(const void* a, const uint32_t* b, const char* c, uint64_t n) {
    return (a != nullptr) + (b != nullptr) * 2 + (c != nullptr) * 4 + n * 8;
}
auto volatile callPtrs = &pointers;

int sub(uintptr_t fn, const char* key, const std::vector<hk::Arg>& args, uint32_t capacity = 256) {
    std::string err;
    int id = hk::subscribe(fn, key, args, capacity, err);
    if (id < 0) printf("subscribe: %s\n", err.c_str());
    HCHECK(id >= 0);
    return id;
}

void checkScalarCapture() {
    auto fn = reinterpret_cast<uintptr_t>(&integers);
    int id = sub(fn, "native/scalars", std::vector<hk::Arg>(8, {hk::U64, {}}));
    HCHECK(callInts(1, 2, 3, 4, 5, 6, 7, 8) == 204);
    hk::Batch batch;
    HCHECK(hk::drain(id, batch) && batch.events.size() == 1);
    const auto& e = batch.events.front();
    for (int i = 0; i < 8; ++i) HCHECK(e.v[i].bits == uint64_t(i + 1));
    HCHECK(e.seq > 0 && e.qpc > 0 && e.dropped == 0);
    hk::unsubscribe(id);
    HCHECK(callInts(8, 7, 6, 5, 4, 3, 2, 1) == 120);
    HCHECK(!hk::drain(id, batch));
}

void checkMixedCapture() {
    int id = sub(reinterpret_cast<uintptr_t>(&mixed), "native/mixed",
                 {{hk::I32}, {hk::F64}, {hk::I32}, {hk::F32}, {hk::F64}, {hk::I32}, {hk::F32}, {hk::F64}});
    HCHECK(callMixed(-3, 1.25, 7, 2.5f, 8.25, -4, 3.5f, 16.0) == 31.5);
    hk::Batch b;
    HCHECK(hk::drain(id, b) && b.events.size() == 1);
    const auto& v = b.events[0].v;
    HCHECK(int64_t(v[0].bits) == -3 && v[2].bits == 7 && int64_t(v[5].bits) == -4);
    HCHECK(std::bit_cast<double>(v[1].bits) == 1.25 && std::bit_cast<double>(v[4].bits) == 8.25);
    HCHECK(std::bit_cast<float>(uint32_t(v[3].bits)) == 2.5f);
    HCHECK(std::bit_cast<float>(uint32_t(v[6].bits)) == 3.5f && std::bit_cast<double>(v[7].bits) == 16);
    hk::unsubscribe(id);
}

void checkPayloads() {
    auto type = refl::lookup("Health");
    std::vector<uint8_t> bytes(type.size(), 0x35);
    uint32_t entity = 0x12345678;
    int id = sub(reinterpret_cast<uintptr_t>(&pointers), "native/payload",
                 {{hk::Snapshot, type}, {hk::EntityPtr}, {hk::CStr}, {hk::U64}});
    HCHECK(callPtrs(bytes.data(), &entity, "copied", 9) == 79);
    std::fill(bytes.begin(), bytes.end(), uint8_t(0x99));
    hk::Batch b;
    HCHECK(hk::drain(id, b) && b.events.size() == 1);
    const auto& v = b.events[0].v;
    HCHECK(v[0].bits == 1 && v[0].len == type.size() && b.payload[v[0].off] == 0x35);
    HCHECK(v[1].bits == entity && v[1].len == 4);
    HCHECK(std::string(reinterpret_cast<char*>(b.payload.data() + v[2].off), v[2].len) == "copied");
    HCHECK(callPtrs(nullptr, nullptr, nullptr, 9) == 72);
    HCHECK(hk::drain(id, b) && b.events[0].v[0].bits == 0 && b.events[0].v[1].len == 0);
    HCHECK(b.events[0].v[2].bits == 0);
    hk::unsubscribe(id);
}

void checkQueues() {
    auto fn = reinterpret_cast<uintptr_t>(&integers);
    int id = sub(fn, "native/overflow", {{hk::U64}}, 2);
    for (int i = 0; i < 5; ++i) callInts(i, 0, 0, 0, 0, 0, 0, 0);
    hk::Batch b;
    HCHECK(hk::drain(id, b) && b.events.size() == 2 && b.events[0].dropped == 3);
    HCHECK(b.events[0].v[0].bits == 0 && b.events[1].v[0].bits == 1 && b.events[1].dropped == 0);
    HCHECK(hk::delivered(id, b.generation) && hk::delivered(id, b.generation));
    hk::unsubscribe(id);
    HCHECK(!hk::delivered(id, b.generation));
    size_t memory = hk::retainedBytes();
    for (int i = 0; i < 25; ++i) {
        HCHECK(sub(fn, "native/overflow", {{hk::U64}}, 2) == id);
        callInts(i, 0, 0, 0, 0, 0, 0, 0);
        hk::unsubscribeAll();
        HCHECK(!hk::drain(id, b));
    }
    HCHECK(hk::retainedBytes() == memory);
}

void checkProducers() {
    int id = sub(reinterpret_cast<uintptr_t>(&integers), "native/threads", {{hk::U64}}, 1024);
    std::vector<std::thread> workers;
    for (uint64_t t = 0; t < 8; ++t) workers.emplace_back([t] {
        for (uint64_t i = 0; i < 100; ++i) callInts(t * 100 + i, 0, 0, 0, 0, 0, 0, 0);
    });
    for (auto& w : workers) w.join();
    hk::Batch b;
    HCHECK(hk::drain(id, b) && b.events.size() == 800 && b.events[0].dropped == 0);
    std::vector<uint64_t> values;
    uint64_t last = 0;
    for (const auto& e : b.events) {
        HCHECK(e.seq > last);
        last = e.seq;
        values.push_back(e.v[0].bits);
    }
    std::sort(values.begin(), values.end());
    for (uint64_t i = 0; i < 800; ++i) HCHECK(values[size_t(i)] == i);
    hk::unsubscribe(id);
}

void checkFault() {
    int id = sub(reinterpret_cast<uintptr_t>(&pointers), "native/fault", {{hk::Snapshot, refl::lookup("Health")}});
    HCHECK(callPtrs(reinterpret_cast<void*>(1), nullptr, nullptr, 6) == 49);
    hk::Batch b;
    HCHECK(!hk::drain(id, b));
    bool faulted = false;
    for (const auto& t : hk::info()) if (t.fn == reinterpret_cast<uintptr_t>(&pointers)) faulted = t.faulted;
    HCHECK(faulted && callPtrs(nullptr, nullptr, nullptr, 6) == 48);
    hk::unsubscribe(id);
}

void checkWidths() {
    int id = sub(reinterpret_cast<uintptr_t>(&integers), "native/widths",
                 {{hk::U8}, {hk::I8}, {hk::U16}, {hk::I16}, {hk::U32}, {hk::I32}, {hk::U64}, {hk::I64}});
    callInts(0x1ff, 0x180, 0x1ffff, 0x18000, 0x1ffffffff, 0x180000000, UINT64_MAX, 0x8000000000000000);
    hk::Batch b;
    HCHECK(hk::drain(id, b));
    const auto& v = b.events[0].v;
    HCHECK(v[0].bits == 255 && int64_t(v[1].bits) == -128);
    HCHECK(v[2].bits == 65535 && int64_t(v[3].bits) == -32768);
    HCHECK(v[4].bits == UINT32_MAX && int64_t(v[5].bits) == INT32_MIN);
    HCHECK(v[6].bits == UINT64_MAX && int64_t(v[7].bits) == INT64_MIN);
    hk::unsubscribe(id);
    id = sub(reinterpret_cast<uintptr_t>(&integers), "native/bool", {{hk::Bool}, {hk::Entity}});
    callInts(0x100, 0x1abcdef01, 0, 0, 0, 0, 0, 0);
    HCHECK(hk::drain(id, b) && b.events[0].v[0].bits == 0 && b.events[0].v[1].bits == 0xabcdef01);
    callInts(2, 0, 0, 0, 0, 0, 0, 0);
    HCHECK(hk::drain(id, b) && b.events[0].v[0].bits == 1);
    hk::unsubscribe(id);
}

struct Returned { uint64_t a, b, c; };
__declspec(noinline) Returned aggregate(uint64_t a, double b, uint64_t c) {
    return {a + 3, uint64_t(b * 4), c + 7};
}
auto volatile callAggregate = &aggregate;

void checkAbi() {
    int id = sub(reinterpret_cast<uintptr_t>(&integers), "native/scalars", std::vector<hk::Arg>(8, {hk::U64, {}}));
    HCHECK(hk_test_abi(reinterpret_cast<void*>(&integers)) == 1);
    hk::unsubscribe(id);
    id = sub(reinterpret_cast<uintptr_t>(&aggregate), "native/sret", {{hk::Skip}, {hk::U64}, {hk::F64}, {hk::U64}});
    auto result = callAggregate(7, 2.5, 12);
    HCHECK(result.a == 10 && result.b == 10 && result.c == 19);
    hk::Batch b;
    HCHECK(hk::drain(id, b) && b.events[0].v[1].bits == 7);
    HCHECK(std::bit_cast<double>(b.events[0].v[2].bits) == 2.5 && b.events[0].v[3].bits == 12);
    hk::unsubscribe(id);
}

void checkSharedCapture() {
    auto fn = reinterpret_cast<uintptr_t>(&pointers);
    auto type = refl::lookup("Health");
    int a = sub(fn, "native/sharedA", {{hk::Snapshot, type}, {hk::Skip}, {hk::CStr}}, 2);
    int b = sub(fn, "native/sharedB", {{hk::Snapshot, type}}, 2);
    uint64_t before = 0;
    for (const auto& t : hk::info()) if (t.fn == fn) before = t.decoded;
    std::vector<uint8_t> hp(type.size(), 0x42);
    char longText[300];
    memset(longText, 'x', sizeof longText);
    callPtrs(hp.data(), nullptr, longText, 0);
    callPtrs(hp.data(), nullptr, "", 0);
    hk::Batch x, y;
    HCHECK(hk::drain(a, x) && hk::drain(b, y));
    HCHECK(x.events[0].seq == y.events[0].seq && x.events[0].v[2].len == 256);
    HCHECK(x.events[1].v[2].bits == 1 && x.events[1].v[2].len == 0);
    x.payload[0] = 0;
    HCHECK(y.payload[0] == 0x42 && hp[0] == 0x42);
    for (const auto& t : hk::info()) if (t.fn == fn) HCHECK(t.decoded == before + 2);
    std::string err;
    HCHECK(hk::subscribe(fn, "native/conflict", {{hk::U64}}, 1, err) < 0);
    HCHECK(hk::subscribe(fn, "native/sharedA", {{hk::Snapshot, type}}, 1, err) < 0);
    hk::unsubscribe(a);
    hk::unsubscribe(b);
}

HANDLE gReadHit, gReadGo;
uintptr_t gGuardPage;
std::atomic<bool> gUnwound{false};

bool unwindCommon(CONTEXT ctx) {
    for (int i = 0; i < 32 && ctx.Rip; ++i) {
        DWORD64 base;
        auto fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        if (!fn) { ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp); ctx.Rsp += 8; continue; }
        bool common = base + fn->BeginAddress == reinterpret_cast<uintptr_t>(&hk_common);
        void* data;
        DWORD64 frame, priorRsp = ctx.Rsp;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &data, &frame, nullptr);
        if (common) return ctx.Rsp > priorRsp && ctx.Rip != 0;
    }
    return false;
}

LONG CALLBACK readBarrier(EXCEPTION_POINTERS* ex) {
    if (ex->ExceptionRecord->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t at = ex->ExceptionRecord->ExceptionInformation[1];
    if (at < gGuardPage || at - gGuardPage >= 4096) return EXCEPTION_CONTINUE_SEARCH;
    // Only stack memory is inspected here; touching the watched page would fault recursively.
    gUnwound = unwindCommon(*ex->ContextRecord);
    SetEvent(gReadHit);
    return WaitForSingleObject(gReadGo, 10000) == WAIT_OBJECT_0 ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

void checkPausedCapture() {
    auto fn = reinterpret_cast<uintptr_t>(&pointers);
    std::vector<hk::Arg> args = {{hk::Snapshot, refl::lookup("Health")}};
    int id = sub(fn, "native/paused", args, 2);
    auto page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HCHECK(page);
    memset(page, 0x27, 4096);
    gGuardPage = reinterpret_cast<uintptr_t>(page);
    gReadHit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    gReadGo = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto handler = AddVectoredExceptionHandler(1, readBarrier);
    DWORD old;
    HCHECK(handler && VirtualProtect(page, 4096, PAGE_READWRITE | PAGE_GUARD, &old));
    std::thread worker([&] { callPtrs(page, nullptr, nullptr, 0); });
    HCHECK(WaitForSingleObject(gReadHit, 5000) == WAIT_OBJECT_0);
    hk::unsubscribeAll();
    HCHECK(sub(fn, "native/paused", args, 2) == id);
    SetEvent(gReadGo);
    worker.join();
    hk::Batch b;
    HCHECK(gUnwound && !hk::drain(id, b));
    callPtrs(page, nullptr, nullptr, 0);
    HCHECK(hk::drain(id, b) && b.events.size() == 1 && b.payload[0] == 0x27);
    hk::discardPending();
    HCHECK(!hk::delivered(id, b.generation));
    hk::unsubscribe(id);
    RemoveVectoredExceptionHandler(handler);
    CloseHandle(gReadHit);
    CloseHandle(gReadGo);
    VirtualFree(page, 0, MEM_RELEASE);
}

void checkSwaps() {
    auto fn = reinterpret_cast<uintptr_t>(&pointers);
    int id = sub(fn, "native/swaps", {{hk::Skip}, {hk::Skip}, {hk::CStr}, {hk::U64}}, 1024);
    std::barrier phase(9);
    std::atomic<int> finished{0};
    std::vector<std::thread> workers;
    for (uint64_t t = 0; t < 8; ++t) workers.emplace_back([&, t] {
        for (uint64_t round = 0; round < 20; ++round) {
            phase.arrive_and_wait();
            for (uint64_t i = 0; i < 100; ++i) {
                uint64_t n = round * 800 + t * 100 + i;
                char text[32];
                snprintf(text, sizeof text, "%llu", n);
                callPtrs(nullptr, nullptr, text, n);
            }
            ++finished;
            phase.arrive_and_wait();
        }
    });
    hk::Batch b;
    std::vector<bool> seen(16000, false);
    auto consume = [&] {
        hk::drain(id, b);
        for (const auto& e : b.events) {
            size_t n = size_t(e.v[3].bits);
            std::string text(reinterpret_cast<char*>(b.payload.data() + e.v[2].off), e.v[2].len);
            HCHECK(n < seen.size() && !seen[n] && std::stoull(text) == n && e.dropped == 0);
            seen[n] = true;
            HCHECK(hk::delivered(id, b.generation));
        }
    };
    for (int round = 0; round < 20; ++round) {
        finished = 0;
        phase.arrive_and_wait();
        while (finished != 8) consume();
        phase.arrive_and_wait();
        consume();
    }
    for (auto& w : workers) w.join();
    HCHECK(std::all_of(seen.begin(), seen.end(), [](bool v) { return v; }));
    hk::unsubscribe(id);
}

__declspec(noinline) uint64_t installTarget(uint64_t n) { return n * 91 + 55; }
auto volatile callInstalled = &installTarget;

void checkConcurrentInstall() {
    int active = sub(reinterpret_cast<uintptr_t>(&integers), "native/threads", {{hk::U64}}, 1024);
    std::atomic<bool> stop{false}, started{false}, intact{true};
    std::thread worker([&] {
        do {
            if (callInts(1, 2, 3, 4, 5, 6, 7, 8) != 204) intact = false;
            started = true; started.notify_one();
        } while (!stop);
    });
    started.wait(false);
    int added = sub(reinterpret_cast<uintptr_t>(&installTarget), "native/install", {{hk::U64}}, 1);
    HCHECK(callInstalled(9) == 874);
    stop = true;
    worker.join();
    hk::Batch b;
    HCHECK(intact && hk::drain(added, b) && b.events[0].v[0].bits == 9);
    hk::unsubscribe(active); hk::unsubscribe(added);
}

__declspec(noinline) uint64_t benchTarget(uint64_t n, const void* p) {
    return n * 13 + (p != nullptr) * 17;
}
auto volatile callBench = &benchTarget;

void benchCalls(const char* label, const void* p, const std::vector<int>& ids) {
    using Clock = std::chrono::steady_clock;
    int64_t elapsed = 0;
    uint64_t sum = 0;
    hk::Batch batch;
    for (int block = 0; block < 200; ++block) {
        auto start = Clock::now();
        for (uint64_t i = 0; i < 128; ++i) sum += callBench(i, p);
        elapsed += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        for (int id : ids) {
            HCHECK(hk::drain(id, batch) && batch.events.size() == 128 && batch.events[0].dropped == 0);
            for (size_t i = 0; i < batch.events.size(); ++i) HCHECK(hk::delivered(id, batch.generation));
        }
    }
    printf("hook benchmark %-19s %8.1f ns/call (checksum %llu)\n", label, double(elapsed) / 25600, sum);
}

void checkBenchmark() {
    auto fn = reinterpret_cast<uintptr_t>(&benchTarget);
    auto type = refl::lookup("Health");
    std::vector<uint8_t> hp(type.size(), 0);
    benchCalls("unhooked", hp.data(), {});
    int scalar = sub(fn, "bench/scalar", {{hk::U64}});
    hk::unsubscribe(scalar);
    benchCalls("inactive detour", hp.data(), {});
    HCHECK(sub(fn, "bench/scalar", {{hk::U64}}) == scalar);
    benchCalls("scalar capture", hp.data(), {scalar});
    hk::unsubscribe(scalar);
    std::vector<hk::Arg> args = {{hk::U64}, {hk::Snapshot, type}};
    int a = sub(fn, "bench/snapshot", args);
    benchCalls("snapshot capture", hp.data(), {a});
    int b = sub(fn, "bench/second", args), c = sub(fn, "bench/third", args);
    benchCalls("three subscribers", hp.data(), {a, b, c});
    hk::unsubscribeAll();
    printf("Microbenchmark only: excludes Lua/drains and does not establish game performance.\n");
}

// Another mod (a plugin's MinHook) hooked the target first: its entry is now a jump to that mod's detour. The pattern
// still finds the target (scans use the file's bytes), and SET's detour chains in front of the jump.
void checkForeignHook() {
    auto t = reinterpret_cast<uint8_t*>(&hk_foreign_target);
    auto d = reinterpret_cast<uint8_t*>(&hk_foreign_detour);
    std::string pattern, err;
    for (int i = 0; i < 8; ++i) {
        char byte[4];
        snprintf(byte, sizeof byte, "%02X ", t[i]);
        pattern += byte;
    }
    HCHECK(hk_foreign_target(1) == 0x12345679);
    DWORD old;
    HCHECK(VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old));
    t[0] = 0xE9;  // jmp rel32
    int32_t rel = static_cast<int32_t>(d - (t + 5));
    memcpy(t + 1, &rel, 4);
    VirtualProtect(t, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
    HCHECK(hk_foreign_target(1) == 3);
    auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    HCHECK(hk::findPattern(image, pattern, err) == reinterpret_cast<uintptr_t>(t));
    int id = sub(reinterpret_cast<uintptr_t>(t), "foreign/target", {{hk::U64}});
    HCHECK(hk_foreign_target(1) == 3);  // the other mod's detour still runs
    hk::Batch b;
    HCHECK(hk::drain(id, b) && b.events.size() == 1 && b.events[0].v[0].bits == 1);
    hk::unsubscribe(id);
}

void checkAdjacent() {
    auto a = reinterpret_cast<uintptr_t>(&hk_adjacent_a), b = reinterpret_cast<uintptr_t>(&hk_adjacent_b);
    HCHECK(b - a == 8);
    std::string pattern, err;
    for (int i = 0; i < 8; ++i) {
        char byte[4];
        snprintf(byte, sizeof byte, "%02X ", reinterpret_cast<uint8_t*>(a)[i]);
        pattern += byte;
    }
    auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    HCHECK(hk::findPattern(image, pattern, err) == a);
    int first = sub(a, "adjacent/first", {{hk::U64}});
    int second = sub(b, "adjacent/second", {{hk::U64}});
    HCHECK(hk_adjacent_a(3) == 20 && hk_adjacent_b(3) == 37);
    HCHECK(hk::findPattern(image, pattern, err) == a);
    hk::unsubscribe(first); hk::unsubscribe(second);
}

}  // namespace

void checkPattern(uintptr_t base) {
    std::string err;
    HCHECK(hk::findPattern(base, "40 55 53 56 41 55 48 8D AC 24 F8 FF", err) == base + 0xa1e2e0);
    HCHECK(hk::findPattern(base, "40 55 53 56 41 55 48 8d ac 24 ?? ff", err) == base + 0xa1e2e0);
    HCHECK(!hk::findPattern(base, "", err) && !err.empty());
    HCHECK(!hk::findPattern(base, "48 ?F", err) && err.find("malformed") != std::string::npos);
    HCHECK(!hk::findPattern(base, "?? ??", err));
    HCHECK(!hk::findPattern(base, "90", err) && err.find("ambiguous") != std::string::npos);
    HCHECK(!hk::findPattern(base, "01 23 45 67 89 AB CD EF 01 23 45 67 89 AB CD EF", err));
}

rules::Loc arg(uint8_t i, uint8_t width = 4, rules::Num num = rules::Signed) { return {i, width, num}; }
rules::Change change(rules::Loc at, rules::Op op, double v) { return {at, op, {rules::Const, 0, v}}; }
rules::Cond cond(rules::Loc at, rules::Cmp cmp, double v) { return {at, cmp, {rules::Const, 0, v}}; }

std::unique_ptr<rules::Rule> rule(std::initializer_list<rules::Cond> cs, std::initializer_list<rules::Change> ch) {
    auto r = std::make_unique<rules::Rule>();
    for (const auto& c : cs) r->shape.cond[r->shape.conds++] = c;
    for (const auto& c : ch) r->shape.change[r->shape.changes++] = c;
    r->active = true;
    return r;
}

uint64_t applied(std::initializer_list<rules::Rule*> rs, rules::Frame in, int pos = 0) {
    rules::Frame out;
    std::vector<rules::Rule*> v(rs);
    HCHECK(rules::apply(v.data(), int(v.size()), in, out) == -1);
    return out.a[pos];
}

// Order, the original values in conditions, inactive rules, params and hit counts.
void checkRuleOrder() {
    auto half = rule({}, {change(arg(0), rules::Mul, 0.5)}), plus = rule({}, {change(arg(0), rules::Add, 20)});
    rules::Frame in{{100}};
    HCHECK(applied({half.get(), plus.get()}, in) == 70 && applied({plus.get(), half.get()}, in) == 60);
    auto five = rule({cond(arg(0), rules::Eq, 100)}, {change(arg(0), rules::Set, 5)});
    auto one = rule({cond(arg(0), rules::Eq, 100)}, {change(arg(0), rules::Add, 1)});
    HCHECK(applied({five.get(), one.get()}, in) == 6);  // one still saw 100
    one->active = false;
    HCHECK(applied({five.get(), one.get()}, in) == 5 && one->hits == 1 && five->hits == 2);
    auto scaled = rule({}, {{arg(0), rules::Mul, {rules::Param, 2, 0}}});
    rules::setParam(*scaled, 2, 3);
    HCHECK(applied({scaled.get()}, in) == 300);
    rules::setParam(*scaled, 2, -0.25);
    HCHECK(applied({scaled.get()}, in) == uint32_t(-25));  // an i32 position: only the low 4 bytes change
    uint8_t slots[15], back[15];
    for (int i = 0; i < 15; ++i) slots[i] = uint8_t(14 - i);
    HCHECK(rules::unpackOrder(rules::packOrder(slots, 15), back) == 15 && memcmp(slots, back, 15) == 0);
    HCHECK(rules::unpackOrder(rules::packOrder(slots, 0), back) == 0);
    HCHECK(rules::writesSame(half->shape, plus->shape) && !rules::writesSame(half->shape, rule({}, {})->shape));
}

// Widths and number kinds: saturation, rounding, NaN, floats keeping the register's other bits.
void checkRuleValues() {
    auto add = [](rules::Loc at, double v, uint64_t in) { auto r = rule({}, {change(at, rules::Add, v)}); return applied({r.get()}, {{in}}); };
    HCHECK(add(arg(0, 1, rules::Unsigned), 300, 0xAB0A) == 0xABFF);       // u8 saturates, upper bits kept
    HCHECK(add(arg(0, 1, rules::Signed), -200, 0x05) == 0x80);           // i8: -128
    HCHECK(add(arg(0, 8, rules::Signed), 1e30, 0) == uint64_t(INT64_MAX));
    HCHECK(add(arg(0, 8, rules::Unsigned), -5, 3) == 0);
    auto mul = rule({}, {change(arg(0), rules::Mul, 0.5)});
    HCHECK(applied({mul.get()}, {{uint64_t(uint32_t(-7))}}) == uint32_t(-4));  // -3.5 rounds away from zero
    auto nan = rule({}, {change(arg(0), rules::Set, std::nan(""))});
    HCHECK(applied({nan.get()}, {{77}}) == 0);
    auto f = rule({}, {change(arg(1, 4, rules::Float), rules::Mul, 2)});
    uint64_t out = applied({f.get()}, {{0, 0xDEADBEEF00000000ull | std::bit_cast<uint32_t>(1.5f)}}, 1);
    HCHECK(out >> 32 == 0xDEADBEEF && std::bit_cast<float>(uint32_t(out)) == 3.0f);
    auto d = rule({}, {change(arg(4, 8, rules::Float), rules::Max, 2.5)});  // a stack position
    HCHECK(std::bit_cast<double>(applied({d.get()}, {{0, 0, 0, 0, std::bit_cast<uint64_t>(9.0)}}, 4)) == 2.5);
    auto b = rule({}, {change(arg(2, 1, rules::Boolean), rules::Set, 7)});
    HCHECK(applied({b.get()}, {{0, 0, 0xFF00}}, 2) == 0xFF01);
}

// Player and list conditions, fields through a pointer, and a fault that leaves nothing behind.
void checkRuleConds() {
    auto mine = rule({{arg(0), rules::Eq, {rules::Player}}}, {change(arg(1), rules::Set, 1)});
    rules::setPlayer(0);
    HCHECK(applied({mine.get()}, {{0, 0}}, 1) == 0);  // no player: never matches, even an id of 0
    rules::setPlayer(42);
    HCHECK(applied({mine.get()}, {{42, 0}}, 1) == 1 && applied({mine.get()}, {{43, 0}}, 1) == 0);
    rules::setPlayer(0);
    auto listed = rule({{arg(0, 4, rules::Unsigned), rules::In, {}}}, {change(arg(1), rules::Set, 1)});
    uint32_t ids[] = {1, 2, 3};
    rules::setList(*listed, ids, 3);
    HCHECK(applied({listed.get()}, {{2, 0}}, 1) == 1 && applied({listed.get()}, {{4, 0}}, 1) == 0);
    struct { uint32_t a; float b; } s{9, 1.5f};
    rules::Loc fb{2, 4, rules::Float, true, 4};
    auto field = rule({cond(fb, rules::Gt, 1.0)}, {change(arg(0), rules::Set, 5)});
    HCHECK(applied({field.get()}, {{0, 0, reinterpret_cast<uint64_t>(&s)}}) == 5);
    HCHECK(applied({field.get()}, {{0, 0, 0}}) == 0 && field->faults == 0);  // null: no value, no fault
    auto page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    auto first = rule({}, {change(arg(0), rules::Add, 1)});
    rules::Rule* rs[] = {first.get(), field.get()};
    rules::Frame in{{0, 0, reinterpret_cast<uint64_t>(page)}}, out;
    uint64_t hits = first->hits;
    HCHECK(rules::apply(rs, 2, in, out) == 1 && !field->active && field->faults == 1 && first->hits == hits);
    HCHECK(rules::apply(rs, 2, in, out) == -1 && out.a[0] == 1);  // the faulted rule is off; the other still runs
    VirtualFree(page, 0, MEM_RELEASE);
}

rules::Loc fieldAt(uint8_t i, uint32_t off, bool stack, uint8_t width = 4, rules::Num num = rules::Signed) {
    rules::Loc l{i, width, num, true, off};
    l.stack = stack;
    return l;
}

// Field changes: in order over staged values, only on this thread's stack when the field says so, written all or none.
void checkRuleFields() {
    struct { int32_t level; bool set; } opt{10, true};
    auto lv = fieldAt(0, 0, true);
    auto twice = rule({}, {change(lv, rules::Mul, 2)});
    auto plus = rule({cond(lv, rules::Eq, 10)}, {change(lv, rules::Add, 1)});  // its condition sees the original 10
    rules::Rule* rs[] = {twice.get(), plus.get()};
    rules::Frame in{{reinterpret_cast<uint64_t>(&opt)}}, out;
    HCHECK(rules::apply(rs, 2, in, out) == -1 && opt.level == 21 && opt.set && twice->hits == 1 && plus->hits == 1);
    // Off this thread's stack (or null), a stack field can't change: the whole rule doesn't apply.
    auto heap = std::make_unique<int32_t[]>(2);
    heap[0] = 10;
    auto both = rule({}, {change(arg(1), rules::Set, 7), change(lv, rules::Set, 5)});
    rules::Rule* one[] = {both.get()};
    in = {{reinterpret_cast<uint64_t>(heap.get()), 0}};
    HCHECK(rules::apply(one, 1, in, out) == -1 && heap[0] == 10 && out.a[1] == 0 && both->refused == 1 && !both->hits);
    in.a[0] = 0;
    HCHECK(rules::apply(one, 1, in, out) == -1 && both->refused == 2);
    auto owned = rule({}, {change(fieldAt(0, 0, false), rules::Set, 5)});  // thread-owned: no stack check
    rules::Rule* o[] = {owned.get()};
    in.a[0] = reinterpret_cast<uint64_t>(heap.get());
    HCHECK(rules::apply(o, 1, in, out) == -1 && heap[0] == 5);
    // A faulting write: the write before it is undone, nothing counts, and only the rule that wrote the field turns off.
    auto page = static_cast<int32_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    page[0] = 3;
    DWORD old;
    HCHECK(VirtualProtect(page, 4096, PAGE_READONLY, &old));
    opt.level = 10;
    auto first = rule({}, {change(lv, rules::Set, 99)}), ro = rule({}, {change(fieldAt(1, 0, false), rules::Add, 1)});
    rules::Rule* rw[] = {first.get(), ro.get()};
    in = {{reinterpret_cast<uint64_t>(&opt), reinterpret_cast<uint64_t>(page)}};
    HCHECK(rules::apply(rw, 2, in, out) == 1 && opt.level == 10 && page[0] == 3);
    HCHECK(first->active && !first->hits && !ro->active && ro->faults == 1);
    VirtualFree(page, 0, MEM_RELEASE);
}

std::unique_ptr<rules::Rule> blocker(std::initializer_list<rules::Cond> cs, rules::Loc retAt, rules::Operand v) {
    auto r = rule(cs, {});
    r->shape.block = true;
    r->shape.ret = v;
    r->shape.retAt = retAt;
    return r;
}
rules::Operand num(double v) { return {rules::Const, 0, v}; }

uint64_t blockedRet(rules::Loc retAt, double v, bool fp = false) {
    auto r = blocker({}, retAt, num(v));
    rules::Rule* rs[] = {r.get()};
    rules::Frame in{}, out;
    rules::Result res;
    HCHECK(rules::apply(rs, 1, in, out, &res) == -1 && res.blocked && res.fp == fp);
    return res.ret;
}

// Blocking: the first matching block wins, nothing before it is committed, and the value takes the return kind.
void checkRuleBlock() {
    auto add = rule({}, {change(arg(0), rules::Add, 1)});
    auto stop = blocker({}, arg(0, 4), num(-2)), later = blocker({}, arg(0, 8), num(9));
    rules::Rule* rs[] = {add.get(), stop.get(), later.get()};
    rules::Frame in{{5}}, out;
    rules::Result res;
    HCHECK(rules::apply(rs, 3, in, out, &res) == -1 && res.blocked && res.ret == uint64_t(-2) && out.a[0] == 5);
    HCHECK(!add->hits && stop->hits == 1 && !later->hits);
    auto onSix = blocker({cond(arg(0), rules::Eq, 6)}, arg(0, 8), num(1));  // sees the original 5, not add's 6
    rules::Rule* cs[] = {add.get(), onSix.get()};
    HCHECK(rules::apply(cs, 2, in, out, &res) == -1 && !res.blocked && out.a[0] == 6 && add->hits == 1);
    HCHECK(blockedRet(arg(0, 1, rules::Unsigned), 300) == 0xFF && blockedRet(arg(0, 2, rules::Signed), -1) == ~0ull);
    HCHECK(blockedRet(arg(0, 1, rules::Boolean), 7) == 1 && blockedRet({0, 0}, 5) == 0);
    HCHECK(blockedRet(arg(0, 4, rules::Float), 1.5, true) == std::bit_cast<uint32_t>(1.5f));
    HCHECK(blockedRet(arg(0, 8, rules::Float), -2.25, true) == std::bit_cast<uint64_t>(-2.25));
    auto mine = blocker({}, arg(0, 8), {rules::Player});
    rules::Rule* p[] = {mine.get()};
    rules::setPlayer(0);
    HCHECK(rules::apply(p, 1, in, out, &res) == -1 && !res.blocked);  // no player: no value, no block
    auto param = blocker({}, arg(0, 8), {rules::Param, 1, 0});
    rules::setParam(*param, 1, 33);
    rules::Rule* q[] = {param.get()};
    HCHECK(rules::apply(q, 1, in, out, &res) == -1 && res.ret == 33);
    HCHECK(rules::writesSame(stop->shape, later->shape) && !rules::writesSame(stop->shape, add->shape));
}

std::unique_ptr<rules::Rule> afterRule(std::initializer_list<rules::Cond> cs, rules::Loc retAt, rules::Op op, double v) {
    auto r = rule(cs, {});
    r->shape.retAt = retAt;
    r->shape.after[r->shape.afters++] = change(retAt, op, v);
    return r;
}

// Return changes: only matching rules mark their position; they apply in order, converted to the return kind.
void checkRuleAfter() {
    auto twice = afterRule({}, arg(0, 4), rules::Mul, 2), plus = afterRule({cond(arg(0), rules::Eq, 1)}, arg(0, 4), rules::Add, 5);
    rules::Rule* rs[] = {twice.get(), plus.get()};
    rules::Frame in{{1}}, out;
    rules::Result res;
    HCHECK(rules::apply(rs, 2, in, out, &res) == -1 && !res.blocked && res.after == 3 && twice->hits == 1);
    HCHECK(rules::after(rs, 2, res.after, 10) == 25);  // (10 * 2) + 5
    in.a[0] = 2;
    HCHECK(rules::apply(rs, 2, in, out, &res) == -1 && res.after == 1 && rules::after(rs, 2, res.after, 10) == 20);
    HCHECK(rules::after(rs, 2, 1, 0xDEAD00000000ull | 0x7fffffff) == (0xDEAD00000000ull | 0x7fffffff));  // i32 saturates
    auto f = afterRule({}, arg(0, 4, rules::Float), rules::Mul, 2);
    rules::Rule* fs[] = {f.get()};
    uint64_t bits = rules::after(fs, 1, 1, 0xFFFFFFFF00000000ull | std::bit_cast<uint32_t>(1.5f));
    HCHECK(bits >> 32 == 0xFFFFFFFF && std::bit_cast<float>(uint32_t(bits)) == 3.0f);
    HCHECK(rules::writesSame(twice->shape, plus->shape) && !rules::writesSame(twice->shape, rule({}, {})->shape));
    // A later rule faulting drops the earlier rule's return change too: a call gets all of its changes or none.
    auto page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    auto bad = rule({cond(rules::Loc{1, 4, rules::Float, true, 0}, rules::Gt, 1.0)}, {change(arg(0), rules::Set, 5)});
    rules::Rule* fr[] = {twice.get(), bad.get()};
    in = {{1, reinterpret_cast<uint64_t>(page)}};
    HCHECK(rules::apply(fr, 2, in, out, &res) == 1 && !res.after && !bad->active);
    VirtualFree(page, 0, MEM_RELEASE);
}

// The list changes while 4 threads read it: a call sees the old or the new list, never an id from neither.
void checkRuleListRace() {
    auto r = rule({{arg(0, 4, rules::Unsigned), rules::In, {}}}, {change(arg(1), rules::Set, 1)});
    std::vector<uint32_t> a(64, 7), b(64, 8);
    std::atomic<bool> stop{false};
    std::atomic<int> started{0};
    std::atomic<uint64_t> seen{0}, wrong{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) readers.emplace_back([&] {
        rules::Rule* rs[] = {r.get()};
        rules::Frame out;
        ++started;
        while (!stop) {
            for (uint64_t id : {7, 8, 9}) {
                rules::apply(rs, 1, {{id, 0}}, out);
                if (out.a[1]) (id == 9 ? wrong : seen)++;
            }
        }
    });
    while (started < 4) std::this_thread::yield();
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    for (int i = 0; std::chrono::steady_clock::now() < end; ++i) rules::setList(*r, (i & 1 ? b : a).data(), 64);
    stop = true;
    for (auto& t : readers) t.join();
    HCHECK(seen > 0 && wrong == 0);
}

// Catalog entries against the real exe (build 1792148057): build, pattern and anchors, each entry on its own.
void checkCatalog(uintptr_t base) {
    using contracts::Entry;
    const char* tick = "40 55 53 56 41 55 48 8D AC 24 F8 FF";
    std::vector<Entry> entries = {
        {"ok", {"u64", "u8"}, "void", contracts::Args, {}, {{1792148057, tick, {{0, "40 55"}, {6, "48 8D ?? 24"}}}}, "test"},
        {"anchor", {"u64"}, "void", contracts::Args, {}, {{1792148057, tick, {{0, "41 55"}}}}, "test"},
        {"outside", {"u64"}, "void", contracts::Args, {}, {{1792148057, tick, {{0x7fffffff, "90"}}}}, "test"},
        {"other build", {"u64"}, "void", contracts::Args, {}, {{1, tick, {}}}, "test"},
        {"ambiguous", {"u64"}, "void", contracts::Args, {}, {{1792148057, "90", {}}}, "test"},
        {"hidden", {"u64"}, "hidden", contracts::Block, {}, {{1792148057, tick, {}}}, "test"},
        {"fields", {"u64"}, "void", contracts::Fields, {}, {{1792148057, tick, {}}}, "test"},
        {"no note", {"u64"}, "void", contracts::Args, {}, {{1792148057, tick, {}}}, ""},
        {"kind", {"u64", "skip"}, "void", contracts::Fields, {{1, "s", 0, "cstr", true}}, {{1792148057, tick, {}}}, "test"},
        {"overlap", {"u64", "skip"}, "void", contracts::Fields, {{1, "a", 0, "i32", true}, {1, "b", 2, "i16", true}},
         {{1792148057, tick, {}}}, "test"},
        {"shared", {"u64", "skip"}, "void", contracts::Fields, {{1, "a", 0, "i32", false}, {1, "b", 4, "i32", false}},
         {{1792148057, tick, {}}}, "test"},
        {"arg kind", {"Health*"}, "void", contracts::Args, {}, {{1792148057, tick, {}}}, "test"},
        {"read only", {"u64", "skip"}, "void", contracts::Fields, {{1, "a", 0, "entity", true, true}},
         {{1792148057, tick, {}}}, "test"},
        {"reads off stack", {"u64", "skip"}, "void", contracts::Fields,
         {{1, "a", 0, "i32", false}, {1, "b", 4, "entity", false, true}}, {{1792148057, tick, {}}}, "test"},
    };
    auto s = contracts::check(base, 1792148057, entries);
    HCHECK(s.size() == 14 && s[12].why.find("writable") != std::string::npos && s[13].why.empty() && s[0].fn == base + 0xA1E2E0 && s[0].why.empty() && s[0].contract == 1792148057);
    HCHECK(!s[1].fn && s[1].why == "Anchor 1 differs" && !s[2].fn && s[2].why == "Anchor 1: outside .text");
    HCHECK(!s[3].fn && s[3].why == "Untested build" && s[4].why.find("ambiguous") != std::string::npos);
    HCHECK(s[5].why.find("hidden") == 0 && s[6].why.find("fields") == 0 && s[7].why == "no RE note");
    HCHECK(s[8].why == "field kind isn't a number" && s[9].why.find("overlap") != std::string::npos);
    HCHECK(s[10].why == "more than one field off the stack" && s[11].why == "unknown argument kind" && s[0].entry == &entries[0]);
    auto forced = contracts::check(base, 2, entries);  // a build no entry lists: every entry off
    for (const auto& f : forced) HCHECK(!f.fn);
    auto any = contracts::check(base, 1, entries, true);
    HCHECK(any[0].fn && any[3].fn == base + 0xA1E2E0 && any[3].contract == 1);
    contracts::setChecked(s);
    contracts::Status found;
    HCHECK(contracts::find("ok", found) && found.fn == s[0].fn && !contracts::find("missing", found));
    contracts::setChecked({});
    auto real = contracts::check(base, 1792148057, contracts::catalog());
    for (const auto& e : real) HCHECK(e.why.empty());
    HCHECK(real.size() == 2 && real[0].name == "combat_xp" && real[0].fn == base + 0xE76530);
    HCHECK(real[1].name == "hit" && real[1].fn == base + 0x3E8B40);
    for (const auto& e : contracts::check(base, 2, contracts::catalog())) HCHECK(!e.fn && e.why == "Untested build");
}

void checkRules(uintptr_t base) {
    checkRuleOrder();
    checkRuleValues();
    checkRuleConds();
    checkRuleFields();
    checkRuleBlock();
    checkRuleAfter();
    checkRuleListRace();
    checkCatalog(base);
    printf("rules and catalog ok\n");
}

// Rules on real detours.

int addRule(uintptr_t fn, const char* key, std::vector<hk::Arg> args, std::initializer_list<rules::Cond> cs,
            std::initializer_list<rules::Change> ch, uint32_t order = 0, std::vector<double> params = {},
            std::string* conflict = nullptr) {
    hk::RuleDecl d{key, std::move(args), rule(cs, ch)->shape, order, std::move(params), {}};
    std::string err, c;
    int id = hk::addRule(fn, d, err, c);
    if (id < 0) printf("addRule %s: %s\n", key, err.c_str());
    HCHECK(id >= 0);
    if (conflict) *conflict = c;
    return id;
}

std::string refused(uintptr_t fn, const hk::RuleDecl& d) {
    std::string err, c;
    HCHECK(hk::addRule(fn, d, err, c) < 0 && !err.empty());
    return err;
}

rules::Loc loc(hk::Kind k, uint8_t pos) {
    rules::Loc l;
    HCHECK(hk::argLoc({k, {}}, pos, l));
    return l;
}

// Register, XMM and stack positions, and observers seeing what the original gets.
void checkRuleArgs() {
    auto fn = reinterpret_cast<uintptr_t>(&mixed);
    std::vector<hk::Arg> args = {{hk::I32}, {hk::F64}, {hk::I32}, {hk::F32}, {hk::F64}, {hk::I32}, {hk::F32}, {hk::F64}};
    int id = addRule(fn, "args/mixed", args, {},
                     {change(loc(hk::F64, 1), rules::Mul, 2), change(loc(hk::F32, 3), rules::Set, 10),
                      change(loc(hk::I32, 5), rules::Add, 100), change(loc(hk::F64, 7), rules::Set, 0)});
    int obs = sub(fn, "args/observer", args);
    HCHECK(callMixed(-3, 1.25, 7, 2.5f, 8.25, -4, 3.5f, 16.0) == 124.25);
    hk::Batch b;
    HCHECK(hk::drain(obs, b) && std::bit_cast<double>(b.events[0].v[1].bits) == 2.5 && int64_t(b.events[0].v[5].bits) == 96);
    hk::removeRule(id);
    HCHECK(callMixed(-3, 1.25, 7, 2.5f, 8.25, -4, 3.5f, 16.0) == 31.5);
    hk::unsubscribe(obs);
    std::string err, c;
    hk::RuleDecl wrong{"args/wrong", {{hk::U64}, {hk::U64}}, rule({}, {change(loc(hk::U64, 1), rules::Add, 1)})->shape};
    HCHECK(hk::addRule(fn, wrong, err, c) >= 0);  // nothing else declared now, so it registers
    hk::RuleDecl clash{"args/clash", args, rule({}, {change(loc(hk::F64, 1), rules::Add, 1)})->shape};
    HCHECK(refused(fn, clash).find("conflicting hook declaration") != std::string::npos);
    hk::unsubscribeAll();
}

// Three mods' rules on one value run in mod order, whatever order they registered in; same-value changes by different
// mods are reported.
void checkRuleComposition() {
    auto fn = reinterpret_cast<uintptr_t>(&integers);
    std::vector<hk::Arg> u64(1, {hk::U64});
    std::string conflict;
    addRule(fn, "a/last", u64, {}, {change(loc(hk::U64, 0), rules::Add, -1)}, 2);
    addRule(fn, "b/first", u64, {}, {change(loc(hk::U64, 0), rules::Add, 10)}, 0, {}, &conflict);
    HCHECK(conflict == "a/last");
    addRule(fn, "c/middle", u64, {}, {change(loc(hk::U64, 0), rules::Mul, 3)}, 1);
    HCHECK(callInts(1, 2, 3, 4, 5, 6, 7, 8) == 204 - 1 + 32);  // ((1 + 10) * 3) - 1
    addRule(fn, "a/same mod", u64, {}, {change(loc(hk::U64, 0), rules::Add, 0)}, 2, {}, &conflict);
    HCHECK(conflict == "b/first");  // another mod still does
    hk::unsubscribeAll();
    HCHECK(callInts(1, 2, 3, 4, 5, 6, 7, 8) == 204);
}

__declspec(noinline) uint64_t countdown(uint64_t n, uint64_t acc);
auto volatile callCountdown = &countdown;
__declspec(noinline) uint64_t countdown(uint64_t n, uint64_t acc) { return n ? callCountdown(n - 1, acc + 1) : acc; }
__declspec(noinline) uint64_t outer(uint64_t x) { return callInts(x, 0, 0, 0, 0, 0, 0, 0); }
auto volatile callOuter = &outer;

// A recursive target, and a hooked target whose original calls another hooked target: each entry applies its own rules.
void checkRuleReentry() {
    std::vector<hk::Arg> two = {{hk::U64}, {hk::U64}}, one(1, {hk::U64});
    addRule(reinterpret_cast<uintptr_t>(&countdown), "reentry/count", two, {cond(loc(hk::U64, 0), rules::Gt, 0)},
            {change(loc(hk::U64, 1), rules::Add, 1)});
    HCHECK(callCountdown(5, 0) == 10);
    addRule(reinterpret_cast<uintptr_t>(&outer), "reentry/outer", one, {}, {change(loc(hk::U64, 0), rules::Add, 1)});
    addRule(reinterpret_cast<uintptr_t>(&integers), "reentry/inner", one, {}, {change(loc(hk::U64, 0), rules::Mul, 10)});
    HCHECK(callOuter(1) == 20);
    hk::unsubscribeAll();
    HCHECK(callOuter(1) == 1 && callCountdown(5, 0) == 5);
}

// A field read that faults takes nothing with it: no argument changes for that call, not even another mod's, and only
// the faulting rule turns off.
void checkRuleFault() {
    auto fn = reinterpret_cast<uintptr_t>(&pointers);
    auto health = refl::lookup("Health");
    std::vector<hk::Arg> args = {{hk::Snapshot, health}, {hk::Skip}, {hk::Skip}, {hk::U64}};
    rules::Loc hp;
    std::string err;
    HCHECK(hk::fieldLoc(args[0], 0, "dataStorage[0]", hp, err) && hp.offset == 8 && hp.width == 4 && hp.num == rules::Signed);
    HCHECK(!hk::fieldLoc(args[0], 0, "dataStorage[8]", hp, err) && !hk::fieldLoc(args[3], 3, "x", hp, err));
    HCHECK(hk::fieldLoc(args[0], 0, "dataStorage[0]", hp, err));
    addRule(fn, "x/always", args, {}, {change(loc(hk::U64, 3), rules::Add, 100)});
    addRule(fn, "y/healthy", args, {cond(hp, rules::Gt, 0)}, {change(loc(hk::U64, 3), rules::Add, 1)}, 1);
    std::vector<uint8_t> h(health.size(), 0);
    int32_t five = 5;
    memcpy(h.data() + 8, &five, 4);
    HCHECK(callPtrs(h.data(), nullptr, nullptr, 0) == 1 + 101 * 8);
    HCHECK(callPtrs(nullptr, nullptr, nullptr, 0) == 100 * 8);  // a null pointer has no field: no match, no fault
    auto page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    uint64_t problems = hk::problems();
    HCHECK(callPtrs(page, nullptr, nullptr, 0) == 1);  // nothing committed
    HCHECK(hk::problems() == problems + 1);
    HCHECK(callPtrs(h.data(), nullptr, nullptr, 0) == 1 + 100 * 8);  // only the faulted rule is off
    bool seen = false;
    for (const auto& t : hk::info())
        for (const auto& r : t.rules) seen |= r.key == "y/healthy" && r.faulted && r.faults == 1;
    HCHECK(seen);
    hk::unsubscribeAll();
    hk::RuleDecl again{"y/healthy", args, rule({cond(hp, rules::Gt, 0)}, {change(loc(hk::U64, 3), rules::Add, 1)})->shape, 1};
    HCHECK(refused(fn, again).find("faulted") != std::string::npos);
    VirtualFree(page, 0, MEM_RELEASE);
}

__declspec(noinline) uint64_t pairSum(uint64_t a, uint64_t b) { return a * 1000003 + b; }
auto volatile callPair = &pairSum;

// 8 threads call while the main thread registers, changes params, removes and re-registers: every call gets all of a
// rule's changes or none, and a param seen is one that was set.
void checkRuleRace() {
    auto fn = reinterpret_cast<uintptr_t>(&pairSum);
    std::vector<hk::Arg> two = {{hk::U64}, {hk::U64}};
    std::atomic<bool> stop{false};
    std::atomic<int> started{0};
    std::atomic<uint64_t> plain{0}, both{0}, wrong{0};
    std::vector<std::thread> callers;
    for (int i = 0; i < 8; ++i) callers.emplace_back([&] {
        ++started;
        while (!stop) {
            uint64_t r = callPair(5, 7);
            if (r == 5 * 1000003 + 7) ++plain;
            else if (r == 6 * 1000003 + 8 || r == 6 * 1000003 + 9) ++both;
            else ++wrong;
        }
    });
    while (started < 8) std::this_thread::yield();
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    for (int i = 0; std::chrono::steady_clock::now() < end; ++i) {
        int id = addRule(fn, "race/pair", two, {},
                         {change(loc(hk::U64, 0), rules::Add, 1), {loc(hk::U64, 1), rules::Add, {rules::Param, 0, 0}}},
                         0, {1});
        hk::setParam(id, 0, (i & 1) + 1);
        std::this_thread::yield();
        hk::removeRule(id);
    }
    stop = true;
    for (auto& t : callers) t.join();
    HCHECK(plain > 0 && both > 0 && wrong == 0);
    hk::unsubscribeAll();
}

struct Opt {
    int32_t level;
    bool set;
};
__declspec(noinline) int64_t optLevel(int32_t level, const Opt* o) { return o->set ? o->level : level; }
auto volatile callOptLevel = &optLevel;
__declspec(noinline) int64_t optOnStack(int32_t level, int32_t other) {
    Opt o{other, true};
    return callOptLevel(level, &o);
}
Opt gOpt{5, true};

// Catalog fields on a real detour: a rule changes the struct the caller passed on its stack, the same struct elsewhere
// is refused per call, and a field the catalog doesn't list, or lists read-only, is refused at registration.
void checkRuleFieldsNative() {
    auto fn = reinterpret_cast<uintptr_t>(&optLevel);
    auto lv = fieldAt(1, 0, true), set = fieldAt(1, 4, true, 1, rules::Boolean);
    hk::RuleDecl d{"fields/level", {{hk::I32}, {hk::Skip}}, rule({cond(set, rules::Eq, 1)}, {change(lv, rules::Add, 10)})->shape};
    d.fields = {lv};
    d.reads = {set};
    std::string err, c;
    int id = hk::addRule(fn, d, err, c);
    HCHECK(id >= 0);
    HCHECK(optOnStack(1, 5) == 15 && optOnStack(1, 5) == 15);  // each call starts from its own original
    HCHECK(callOptLevel(1, &gOpt) == 5 && gOpt.level == 5);    // not on the stack: refused
    uint64_t refusedCalls = 0;
    for (const auto& t : hk::info())
        for (const auto& r : t.rules) if (r.key == "fields/level") refusedCalls = r.refused;
    HCHECK(refusedCalls == 1);
    hk::removeRule(id);
    HCHECK(optOnStack(1, 5) == 5);
    hk::RuleDecl other = d;
    other.key = "fields/other";
    other.shape = rule({}, {change(fieldAt(1, 8, true), rules::Set, 1)})->shape;
    HCHECK(refused(fn, other).find("doesn't let") != std::string::npos);
    other.shape = rule({}, {change(set, rules::Set, 0)})->shape;  // read-only: conditions only
    HCHECK(refused(fn, other).find("doesn't let") != std::string::npos);
    hk::unsubscribeAll();
}

__declspec(noinline) int32_t smallInt(int32_t x) { return x + 1; }
auto volatile callSmallWide = reinterpret_cast<uint64_t (*)(int32_t)>(&smallInt);  // reads all of RAX
__declspec(noinline) bool isEven(uint64_t x) { return x % 2 == 0; }
auto volatile callIsEven = &isEven;
__declspec(noinline) float halfF(float x) { return x / 2; }
auto volatile callHalfF = &halfF;
__declspec(noinline) double halfD(double x) { return x / 2; }
auto volatile callHalfD = &halfD;
__declspec(noinline) void bump(int* p) { ++*p; }
auto volatile callBump = &bump;
__declspec(noinline) uint64_t tripled(uint64_t x) { return x * 3; }
auto volatile callTripled = &tripled;

int addBlock(uintptr_t fn, const char* key, std::vector<hk::Arg> args, std::initializer_list<rules::Cond> cs,
             rules::Loc retAt, double v) {
    hk::RuleDecl d{key, std::move(args), blocker(cs, retAt, num(v))->shape};
    d.block = true;
    std::string err, c;
    int id = hk::addRule(fn, d, err, c);
    if (id < 0) printf("addBlock %s: %s\n", key, err.c_str());
    HCHECK(id >= 0);
    return id;
}

__declspec(noinline) void throwAfterBlocked() {
    if (callSmallWide(1) == ~0ull) throw std::runtime_error("after a blocked call");
}

// Blocked calls on real detours: each return kind, nonvolatile registers, the caller unwinding right after, observers.
void checkRuleBlockNative() {
    int id = addBlock(reinterpret_cast<uintptr_t>(&smallInt), "block/int", {{hk::I32}}, {cond(loc(hk::I32, 0), rules::Lt, 10)},
                      loc(hk::I32, 0), -1);
    int obs = sub(reinterpret_cast<uintptr_t>(&smallInt), "block/observer", {{hk::I32}});
    HCHECK(callSmallWide(1) == ~0ull && uint32_t(callSmallWide(20)) == 21);  // sign-extended to all of RAX
    hk::Batch b;
    HCHECK(hk::drain(obs, b) && b.events.size() == 2);  // observers see blocked calls too
    bool caught = false;
    try { throwAfterBlocked(); } catch (const std::runtime_error&) { caught = true; }
    HCHECK(caught);
    hk::removeRule(id);
    HCHECK(uint32_t(callSmallWide(1)) == 2);
    addBlock(reinterpret_cast<uintptr_t>(&isEven), "block/bool", {}, {}, loc(hk::Bool, 0), 1);
    HCHECK(callIsEven(3));
    addBlock(reinterpret_cast<uintptr_t>(&halfF), "block/f32", {}, {}, loc(hk::F32, 0), 2.5);
    addBlock(reinterpret_cast<uintptr_t>(&halfD), "block/f64", {}, {}, loc(hk::F64, 0), -8);
    HCHECK(callHalfF(10) == 2.5f && callHalfD(10) == -8);
    int n = 1;
    addBlock(reinterpret_cast<uintptr_t>(&bump), "block/void", {}, {}, {0, 0}, 0);
    callBump(&n);
    HCHECK(n == 1);
    std::vector<hk::Arg> eight(8, {hk::U64});
    int abi = addBlock(reinterpret_cast<uintptr_t>(&integers), "block/abi", eight, {}, loc(hk::U64, 0), 204);
    HCHECK(hk_test_abi(reinterpret_cast<void*>(&integers)) == 1);
    uint64_t hits = 0;
    for (const auto& t : hk::info())
        for (const auto& r : t.rules) if (r.key == "block/abi") hits = r.hits;
    HCHECK(hits == 1);
    hk::removeRule(abi);
    hk::RuleDecl uncataloged{"block/pattern", {}, blocker({}, loc(hk::U64, 0), num(1))->shape};
    HCHECK(refused(reinterpret_cast<uintptr_t>(&tripled), uncataloged).find("catalog") != std::string::npos);
    hk::RuleDecl both{"block/both", {{hk::U64}}, blocker({}, loc(hk::U64, 0), num(1))->shape};
    both.shape.change[both.shape.changes++] = change(loc(hk::U64, 0), rules::Set, 1);
    both.block = true;
    HCHECK(refused(reinterpret_cast<uintptr_t>(&tripled), both).find("changes nothing") != std::string::npos);
    hk::unsubscribeAll();
    HCHECK(callHalfF(10) == 5 && !callIsEven(3));
}

// 8 threads call while a conditional block registers and goes away: x = 2 gets 1 (blocked) or 6, x = 3 always 9.
void checkRuleBlockRace() {
    auto fn = reinterpret_cast<uintptr_t>(&tripled);
    std::atomic<bool> stop{false};
    std::atomic<int> started{0};
    std::atomic<uint64_t> blocked{0}, ran{0}, wrong{0};
    std::vector<std::thread> callers;
    for (int i = 0; i < 8; ++i) callers.emplace_back([&] {
        ++started;
        for (uint64_t x = 2; !stop; x = 5 - x) {
            uint64_t r = callTripled(x);
            if (x == 2 && r == 1) ++blocked;
            else if (r == x * 3) ++ran;
            else ++wrong;
        }
    });
    while (started < 8) std::this_thread::yield();
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < end) {
        int id = addBlock(fn, "blockrace/two", {{hk::U64}}, {cond(loc(hk::U64, 0), rules::Eq, 2)}, loc(hk::U64, 0), 1);
        std::this_thread::yield();
        hk::removeRule(id);
    }
    stop = true;
    for (auto& t : callers) t.join();
    HCHECK(blocked > 0 && ran > 0 && wrong == 0);
    hk::unsubscribeAll();
}

__declspec(noinline) uint64_t zeroArgs() { return 7; }
auto volatile callZeroArgs = &zeroArgs;
__declspec(noinline) uint64_t thrower(uint64_t x) {
    if (x) throw std::runtime_error("inside the original");
    return 1;
}
auto volatile callThrower = &thrower;
__declspec(noinline) uint64_t readAt(const uint64_t* p) { return *p; }
auto volatile callReadAt = &readAt;

int addAfter(uintptr_t fn, const char* key, std::vector<hk::Arg> args, std::initializer_list<rules::Cond> cs,
             rules::Loc retAt, rules::Op op, double v) {
    hk::RuleDecl d{key, std::move(args), afterRule(cs, retAt, op, v)->shape};
    d.after = true;
    std::string err, c;
    int id = hk::addRule(fn, d, err, c);
    if (id < 0) printf("addAfter %s: %s\n", key, err.c_str());
    HCHECK(id >= 0);
    return id;
}

uint64_t ruleHits(const char* key) {
    for (const auto& t : hk::info())
        for (const auto& r : t.rules) if (r.key == key) return r.hits;
    return 0;
}

// An access violation inside an original the stub called: the caller's own handler still catches it.
int faultThroughStub() {
    __try {
        return int(callReadAt(nullptr));
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return -1;
    }
}

// Return changes on real detours: arity 0 and 8 with mixed kinds (the originals use every argument), nonvolatile
// registers, exceptions inside the original reaching the caller, recursion, float returns.
void checkRuleAfterNative() {
    addAfter(reinterpret_cast<uintptr_t>(&zeroArgs), "after/zero", {}, {}, loc(hk::U64, 0), rules::Add, 1);
    HCHECK(callZeroArgs() == 8);
    addAfter(reinterpret_cast<uintptr_t>(&integers), "after/ints", {}, {}, loc(hk::U64, 0), rules::Add, 1);
    HCHECK(callInts(1, 2, 3, 4, 5, 6, 7, 8) == 205);
    hk::unsubscribeAll();
    addAfter(reinterpret_cast<uintptr_t>(&integers), "after/abi", {}, {}, loc(hk::U64, 0), rules::Add, 0);
    HCHECK(hk_test_abi(reinterpret_cast<void*>(&integers)) == 1 && ruleHits("after/abi") == 1);
    addAfter(reinterpret_cast<uintptr_t>(&mixed), "after/mixed", {}, {}, loc(hk::F64, 0), rules::Mul, 2);
    HCHECK(callMixed(-3, 1.25, 7, 2.5f, 8.25, -4, 3.5f, 16.0) == 63);
    addAfter(reinterpret_cast<uintptr_t>(&halfF), "after/f32", {}, {}, loc(hk::F32, 0), rules::Mul, 3);
    HCHECK(callHalfF(10) == 15);
    addAfter(reinterpret_cast<uintptr_t>(&thrower), "after/throw", {}, {}, loc(hk::U64, 0), rules::Add, 1);
    bool caught = false;
    try { callThrower(1); } catch (const std::runtime_error&) { caught = true; }
    HCHECK(caught && callThrower(0) == 2);
    addAfter(reinterpret_cast<uintptr_t>(&readAt), "after/fault", {}, {}, loc(hk::U64, 0), rules::Add, 1);
    uint64_t v = 41;
    HCHECK(faultThroughStub() == -1 && callReadAt(&v) == 42);
    addAfter(reinterpret_cast<uintptr_t>(&countdown), "after/recursive", {}, {}, loc(hk::U64, 0), rules::Add, 1);
    HCHECK(callCountdown(5, 0) == 11);  // 5, plus 1 at each of the 6 returns
    hk::RuleDecl uncataloged{"after/pattern", {}, afterRule({}, loc(hk::U64, 0), rules::Add, 1)->shape};
    HCHECK(refused(reinterpret_cast<uintptr_t>(&tripled), uncataloged).find("catalog") != std::string::npos);
    hk::unsubscribeAll();
    HCHECK(callCountdown(5, 0) == 5 && callHalfF(10) == 5 && callZeroArgs() == 7);
}

// 8 threads call while a return change registers and goes away: every result is 3x or 3x + 1.
void checkRuleAfterRace() {
    auto fn = reinterpret_cast<uintptr_t>(&tripled);
    std::atomic<bool> stop{false};
    std::atomic<int> started{0};
    std::atomic<uint64_t> changed{0}, plain{0}, wrong{0};
    std::vector<std::thread> callers;
    for (int i = 0; i < 8; ++i) callers.emplace_back([&, i] {
        ++started;
        for (uint64_t x = uint64_t(i); !stop; ++x) {
            uint64_t r = callTripled(x);
            if (r == x * 3 + 1) ++changed;
            else if (r == x * 3) ++plain;
            else ++wrong;
        }
    });
    while (started < 8) std::this_thread::yield();
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < end) {
        int id = addAfter(fn, "afterrace/plus", {}, {}, loc(hk::U64, 0), rules::Add, 1);
        std::this_thread::yield();
        hk::removeRule(id);
    }
    stop = true;
    for (auto& t : callers) t.join();
    HCHECK(changed > 0 && plain > 0 && wrong == 0);
    hk::unsubscribeAll();
}

__declspec(noinline) uint64_t retained(uint64_t x) { return x + 1; }
auto volatile callRetained = &retained;

// Changed declarations use new slots up to 15; identical ones reuse theirs; the 16th change fails cleanly.
void checkRuleLimits() {
    auto fn = reinterpret_cast<uintptr_t>(&retained);
    std::vector<hk::Arg> one(1, {hk::U64});
    for (int i = 0; i < rules::kMaxRules; ++i) {
        hk::removeRule(addRule(fn, "limits/rule", one, {}, {change(loc(hk::U64, 0), rules::Add, i)}));
        size_t bytes = hk::retainedBytes();
        hk::removeRule(addRule(fn, "limits/rule", one, {}, {change(loc(hk::U64, 0), rules::Add, i)}));
        HCHECK(hk::retainedBytes() == bytes);
    }
    hk::RuleDecl d{"limits/rule", one, rule({}, {change(loc(hk::U64, 0), rules::Add, 99)})->shape};
    HCHECK(refused(fn, d).find("15 retained") != std::string::npos);
    int id = addRule(fn, "limits/rule", one, {}, {change(loc(hk::U64, 0), rules::Add, 3)});
    HCHECK(callRetained(1) == 5);
    hk::removeRule(id);
    HCHECK(callRetained(1) == 2);
}

// Declarations a rule may not make.
void checkRuleRefusals() {
    auto fn = reinterpret_cast<uintptr_t>(&outer);
    auto shape = [](std::initializer_list<rules::Cond> cs, std::initializer_list<rules::Change> ch) { return rule(cs, ch)->shape; };
    std::vector<hk::Arg> one(1, {hk::U64});
    auto health = refl::lookup("Health");
    rules::Loc hp;
    std::string err;
    HCHECK(hk::fieldLoc({hk::Snapshot, health}, 0, "dataStorage[0]", hp, err));
    HCHECK(refused(fn, {"no/field", {{hk::Snapshot, health}}, shape({}, {change(hp, rules::Set, 1)})}).find("catalog") != std::string::npos);
    HCHECK(refused(fn, {"no/undeclared", one, shape({}, {change(loc(hk::U64, 1), rules::Set, 1)})}).find("isn't declared") != std::string::npos);
    HCHECK(refused(fn, {"no/kind", one, shape({}, {change(loc(hk::I32, 0), rules::Set, 1)})}).find("doesn't match") != std::string::npos);
    HCHECK(refused(fn, {"no/param", one, shape({}, {{loc(hk::U64, 0), rules::Set, {rules::Param, 0, 0}}})}).find("param") != std::string::npos);
    HCHECK(refused(fn, {"no/change", one, shape({cond(loc(hk::U64, 0), rules::Eq, 1)}, {})}).find("at least one change") != std::string::npos);
    HCHECK(refused(0, {"no/target", one, shape({}, {change(loc(hk::U64, 0), rules::Set, 1)})}).find("empty") != std::string::npos);
    int id = addRule(fn, "dup/name", one, {}, {change(loc(hk::U64, 0), rules::Set, 1)});
    HCHECK(refused(fn, {"dup/name", one, shape({}, {change(loc(hk::U64, 0), rules::Set, 2)})}).find("already active") != std::string::npos);
    hk::removeRule(id);
    HCHECK(!hk::setParam(id, 0, 1) && !hk::setList(id, {1}));  // a removed rule takes no more updates
}

void checkRuleBenchmark() {
    auto fn = reinterpret_cast<uintptr_t>(&benchTarget);
    auto health = refl::lookup("Health");
    std::vector<uint8_t> hp(health.size(), 0);
    std::vector<hk::Arg> args = {{hk::U64}, {hk::Snapshot, health}};
    rules::Loc field;
    std::string err;
    HCHECK(hk::fieldLoc(args[1], 1, "dataStorage[0]", field, err));
    hk::removeRule(addRule(fn, "bench/off", args, {}, {change(loc(hk::U64, 0), rules::Mul, 1)}));
    benchCalls("rule removed", hp.data(), {});
    int id = addRule(fn, "bench/nomatch", args, {cond(loc(hk::U64, 0), rules::Gt, 1e12)}, {change(loc(hk::U64, 0), rules::Mul, 1)});
    benchCalls("rule, no match", hp.data(), {});
    hk::removeRule(id);
    id = addRule(fn, "bench/scalar", args, {cond(loc(hk::U64, 0), rules::Ge, 0)}, {change(loc(hk::U64, 0), rules::Mul, 1)});
    benchCalls("rule, scalar", hp.data(), {});
    hk::removeRule(id);
    id = addRule(fn, "bench/field", args, {cond(field, rules::Ge, 0)}, {change(loc(hk::U64, 0), rules::Mul, 1)});
    benchCalls("rule, field read", hp.data(), {});
    hk::removeRule(id);
    uint8_t local[64] = {};  // on this thread's stack, as a catalog stack field requires
    hk::RuleDecl write{"bench/write", {{hk::U64}, {hk::Skip}}, rule({}, {change(fieldAt(1, 8, true), rules::Add, 1)})->shape};
    write.fields = {fieldAt(1, 8, true)};
    std::string c;
    HCHECK(hk::addRule(fn, write, err, c) >= 0);
    benchCalls("rule, field write", local, {});
    int32_t written;
    memcpy(&written, local + 8, 4);
    HCHECK(written == 200 * 128);
    hk::unsubscribeAll();
    addBlock(fn, "bench/block", {{hk::U64}}, {cond(loc(hk::U64, 0), rules::Ge, 0)}, loc(hk::U64, 0), 0);
    benchCalls("rule, block", local, {});
    hk::unsubscribeAll();
    addAfter(fn, "bench/after", {{hk::U64}}, {cond(loc(hk::U64, 0), rules::Ge, 0)}, loc(hk::U64, 0), rules::Add, 0);
    benchCalls("rule, after", local, {});
    hk::unsubscribeAll();
}

void checkNativeRules() {
    checkRuleArgs();
    checkRuleComposition();
    checkRuleReentry();
    checkRuleFault();
    checkRuleFieldsNative();
    checkRuleBlockNative();
    checkRuleBlockRace();
    checkRuleAfterNative();
    checkRuleAfterRace();
    checkRuleRace();
    checkRuleLimits();
    checkRuleRefusals();
    checkRuleBenchmark();
    printf("native rules ok\n");
}

void checkHooks(uintptr_t base) {
    checkRules(base);
    checkAdjacent();
    checkPattern(base);
    checkForeignHook();
    std::string pattern, err;
    for (int i = 0; i < 24; ++i) {
        char byte[4];
        snprintf(byte, sizeof byte, "%02X ", reinterpret_cast<uint8_t*>(&integers)[i]);
        pattern += byte;
    }
    auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    HCHECK(hk::findPattern(image, pattern, err) == reinterpret_cast<uintptr_t>(&integers));
    checkScalarCapture();
    HCHECK(hk::findPattern(image, pattern, err) == reinterpret_cast<uintptr_t>(&integers));
    checkMixedCapture();
    checkPayloads();
    checkWidths();
    checkAbi();
    checkSharedCapture();
    checkPausedCapture();
    checkSwaps();
    checkQueues();
    checkProducers();
    checkConcurrentInstall();
    checkBenchmark();
    checkFault();
    checkNativeRules();
    printf("native hooks ok\n");
}

int checkHookLimits() {
    auto page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    HCHECK(page);
    memset(page, 0x90, 8192);
    for (uint32_t i = 0; i < 65; ++i) {
        uint8_t code[] = {0x48, 0x89, 0xc8, 0x48, 0x05, 0, 0, 0, 0, 0xc3};
        memcpy(code + 5, &i, 4);
        memcpy(page + 16 + i * 64, code, sizeof code);
    }
    DWORD old;
    HCHECK(VirtualProtect(page, 8192, PAGE_EXECUTE_READ, &old));
    HCHECK(FlushInstructionCache(GetCurrentProcess(), page, 8192));
    auto at = [&](int i) { return reinterpret_cast<uintptr_t>(page + 16 + i * 64); };
    std::string err;
    auto reserved = reinterpret_cast<uintptr_t>(&integers);
    hk::reserve(reserved, "test reserved");
    HCHECK(hk::subscribe(reserved, "limits/reserved", {}, 1, err) < 0 && err.find("reserved") != std::string::npos);
    HCHECK(hk::subscribe(reserved + 2, "limits/inside", {}, 1, err) < 0 && err.find("overlaps") != std::string::npos);
    HCHECK(hk::subscribe(at(0), "limits/zero", {}, 0, err) < 0);
    HCHECK(hk::subscribe(at(0), "limits/large", {}, 4097, err) < 0);
    HCHECK(hk::subscribe(at(0), "limits/args", std::vector<hk::Arg>(9), 1, err) < 0);
    auto large = std::vector<hk::Arg>(8, {hk::CStr});
    int a = sub(at(0), "limits/budgetA", large, 4096);
    int b = sub(at(1), "limits/budgetB", large, 4096);
    size_t bytes = hk::retainedBytes();
    HCHECK(hk::subscribe(at(2), "limits/budgetC", large, 4096, err) < 0 && err.find("64 MiB") != std::string::npos);
    HCHECK(hk::retainedBytes() == bytes && hk::info().size() == 2);
    HCHECK(hk::subscribe(at(1) - 3, "limits/before", {}, 1, err) < 0 && err.find("overlaps") != std::string::npos);
    HCHECK(hk::info().size() == 2);
    hk::unsubscribe(a); hk::unsubscribe(b);
    for (uint32_t cap = 1; cap <= 8; ++cap) {
        int id = sub(at(2), "limits/changed", {{hk::U64}}, cap);
        HCHECK(reinterpret_cast<uint64_t (*)(uint64_t)>(at(2))(11) == 13);
        hk::unsubscribe(id);
    }
    HCHECK(hk::subscribe(at(2), "limits/changed", {{hk::U64}}, 9, err) < 0 && err.find("eight retained") != std::string::npos);
    bytes = hk::retainedBytes();
    hk::unsubscribe(sub(at(2), "limits/changed", {{hk::U64}}, 1));
    HCHECK(hk::retainedBytes() == bytes);
    for (int i = 3; i < 64; ++i) {
        int id = sub(at(i), ("limits/target" + std::to_string(i)).c_str(), {{hk::U64}}, 1);
        HCHECK(reinterpret_cast<uint64_t (*)(uint64_t)>(at(i))(11) == uint64_t(11 + i));
        hk::unsubscribe(id);
    }
    HCHECK(hk::subscribe(at(64), "limits/extra", {}, 1, err) < 0 && err.find("64 hook targets") != std::string::npos);
    for (int i = 0; i < 40; ++i) hk::refuse("bounded diagnostic");
    HCHECK(hk::diagnostics().size() == 32 && hk::retainedBytes() <= hk::kMemoryBudget);
    printf("hook limits ok\n");
    return 0;  // Retained synthetic executable targets live until this child exits.
}

int checkHookReadFault() {
    SetErrorMode(SEM_NOGPFAULTERRORBOX);
    auto page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HCHECK(page);
    int id = sub(reinterpret_cast<uintptr_t>(&benchTarget), "fault/guard", {{hk::Skip}, {hk::CStr}}, 1);
    DWORD old;
    HCHECK(VirtualProtect(page, 4096, PAGE_READWRITE | PAGE_GUARD, &old));
    uint64_t problems = hk::problems();
    HCHECK(callBench(1, page) == 30);
    hk::Batch batch;
    HCHECK(hk::problems() == problems + 1 && !hk::drain(id, batch));
    HCHECK(hk::info()[0].faulted);
    VirtualFree(page, 0, MEM_RELEASE);
    printf("hook guarded read fault ok\n");
    return 0;
}
