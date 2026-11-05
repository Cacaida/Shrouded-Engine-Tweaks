// Real detours on local functions: no game process is touched.
#include "hooks.h"

#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

extern "C" int hk_test_abi(void* target);
extern "C" void hk_common();
extern "C" uint64_t hk_adjacent_a(uint64_t);
extern "C" uint64_t hk_adjacent_b(uint64_t);

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

void checkHooks(uintptr_t base) {
    checkAdjacent();
    checkPattern(base);
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
