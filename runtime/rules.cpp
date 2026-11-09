#include "rules.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bit>
#include <cmath>
#include <cstring>

namespace {

std::atomic<uint32_t> gPlayer{0};

int readFault(DWORD code) {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ||
           code == STATUS_GUARD_PAGE_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

uint64_t mask(const rules::Loc& l) { return l.width == 8 ? ~0ull : (1ull << (8 * l.width)) - 1; }

double toNum(uint64_t b, const rules::Loc& l) {
    switch (l.num) {
    case rules::Float: return l.width == 4 ? double(std::bit_cast<float>(uint32_t(b))) : std::bit_cast<double>(b);
    case rules::Boolean: return (b & 0xff) ? 1 : 0;
    case rules::Signed: {
        int sh = 64 - 8 * l.width;
        return double(int64_t(b << sh) >> sh);
    }
    default: return double(b & mask(l));
    }
}

// Integers round half away from zero and saturate at the width's range; NaN becomes 0.
uint64_t fromNum(double v, const rules::Loc& l) {
    if (l.num == rules::Float) return l.width == 4 ? std::bit_cast<uint32_t>(float(v)) : std::bit_cast<uint64_t>(v);
    if (v != v) v = 0;
    if (l.num == rules::Boolean) return v != 0;
    double r = std::round(v);
    int bits = 8 * l.width;
    if (l.num == rules::Signed) {
        double lim = std::ldexp(1.0, bits - 1);
        int64_t lo = bits == 64 ? INT64_MIN : -(int64_t(1) << (bits - 1)), hi = bits == 64 ? INT64_MAX : -lo - 1;
        int64_t i = r >= lim ? hi : r < -lim ? lo : int64_t(r);
        return uint64_t(i) & mask(l);
    }
    if (r <= 0) return 0;
    return r >= std::ldexp(1.0, bits) ? mask(l) : uint64_t(r);
}

bool operand(const rules::Rule& r, const rules::Operand& o, double& v) {
    switch (o.src) {
    case rules::Param: v = std::bit_cast<double>(r.param[o.param].load(std::memory_order_relaxed)); return true;
    case rules::Player: {
        uint32_t id = gPlayer.load(std::memory_order_relaxed);
        v = id;
        return id != 0;
    }
    default: v = o.value; return true;
    }
}

bool listHas(const rules::Rule& r, double v) {
    if (!(v >= 0 && v <= 4294967295.0) || v != std::floor(v)) return false;
    auto id = uint32_t(v);
    uint32_t seq = r.listSeq.load(std::memory_order_acquire);
    if (seq & 1) return false;
    uint32_t n = r.listCount.load(std::memory_order_relaxed);
    bool hit = false;
    for (uint32_t i = 0; i < n && i < rules::kMaxList; ++i) hit |= r.list[i].load(std::memory_order_relaxed) == id;
    std::atomic_thread_fence(std::memory_order_acquire);
    return hit && r.listSeq.load(std::memory_order_relaxed) == seq;
}

// A field of a null pointer has no value: the condition doesn't hold. Any other unreadable pointer faults.
bool read(const rules::Loc& l, const rules::Frame& f, uint64_t& bits) {
    if (!l.field) { bits = f.a[l.arg]; return true; }
    auto p = reinterpret_cast<const uint8_t*>(f.a[l.arg]);
    if (!p) return false;
    bits = 0;
    memcpy(&bits, p + l.offset, l.width);
    return true;
}

bool holds(const rules::Rule& r, const rules::Cond& c, const rules::Frame& in) {
    uint64_t bits;
    if (!read(c.at, in, bits)) return false;
    double x = toNum(bits, c.at), v;
    if (c.cmp == rules::In) return listHas(r, x);
    if (!operand(r, c.v, v)) return false;
    switch (c.cmp) {
    case rules::Eq: return x == v;
    case rules::Ne: return x != v;
    case rules::Lt: return x < v;
    case rules::Le: return x <= v;
    case rules::Gt: return x > v;
    default: return x >= v;
    }
}

bool matches(const rules::Rule& r, const rules::Frame& in) {
    for (int i = 0; i < r.shape.conds; ++i)
        if (!holds(r, r.shape.cond[i], in)) return false;
    return true;
}

// The fields one call changes, each staged with the value memory held before any rule ran.
struct Write {
    uint8_t* at;
    uint8_t width;
    uint64_t bits, old;
    uint16_t rules;  // which of the call's rules changed it, to switch off if the write faults
};
struct Writes {
    int n = 0;
    Write w[rules::kMaxWrites];
};

bool onStack(const uint8_t* p, size_t n) {
    auto tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    auto a = reinterpret_cast<uintptr_t>(p);
    return a >= uintptr_t(tib->StackLimit) && a + n <= uintptr_t(tib->StackBase);
}

// The staged write for field l, staged on first use (reading memory may fault: the caller is under __try). nullptr if
// this call can't change it.
Write* staged(const rules::Loc& l, const rules::Frame& in, Writes& ws) {
    auto p = reinterpret_cast<uint8_t*>(in.a[l.arg]);
    if (!p) return nullptr;
    p += l.offset;
    if (l.stack && !onStack(p, l.width)) return nullptr;
    for (int i = 0; i < ws.n; ++i)
        if (ws.w[i].at == p) return &ws.w[i];
    if (ws.n == rules::kMaxWrites) return nullptr;
    Write& w = ws.w[ws.n++];
    w = {p, l.width, 0, 0, 0};
    memcpy(&w.old, p, l.width);
    w.bits = w.old;
    return &w;
}

void change(const rules::Rule& r, const rules::Change& c, uint64_t& bits) {
    double v, x = toNum(bits, c.at);
    if (!operand(r, c.v, v)) return;
    switch (c.op) {
    case rules::Set: x = v; break;
    case rules::Add: x += v; break;
    case rules::Mul: x *= v; break;
    case rules::Min: x = x < v ? v : x; break;
    default: x = x > v ? v : x; break;
    }
    uint64_t m = mask(c.at);
    bits = (bits & ~m) | (fromNum(x, c.at) & m);
}

// What a blocked call returns for v: an integer sign- or zero-extended to 64 bits, a float's bits.
uint64_t retBits(double v, const rules::Loc& at) {
    if (!at.width) return 0;
    uint64_t b = fromNum(v, at);
    if (at.num != rules::Signed || at.width == 8) return b;
    int sh = 64 - 8 * at.width;
    return uint64_t(int64_t(b << sh) >> sh);
}

// Rule k on one call: 1 if it applied, 2 if it blocks the call (res filled), 0 if it didn't match or was refused, -1
// if reading memory faulted.
int stage(rules::Rule& r, int k, const rules::Frame& in, rules::Frame& out, Writes& ws, rules::Result& res) {
    const auto& s = r.shape;
    __try {
        if (!matches(r, in)) return 0;
        if (s.block) {
            double v;
            if (!operand(r, s.ret, v)) return 0;
            res = {true, s.retAt.num == rules::Float && s.retAt.width != 0, retBits(v, s.retAt), 0};
            return 2;
        }
        for (int i = 0; i < s.changes; ++i)
            if (s.change[i].at.field && !staged(s.change[i].at, in, ws)) {
                r.refused.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
        for (int i = 0; i < s.changes; ++i) {
            const auto& c = s.change[i];
            if (!c.at.field) { change(r, c, out.a[c.at.arg]); continue; }
            Write* w = staged(c.at, in, ws);
            change(r, c, w->bits);
            w->rules |= uint16_t(1u << k);
        }
        if (s.afters) res.after |= uint16_t(1u << k);
        return 1;
    } __except (readFault(GetExceptionCode())) {
        return -1;
    }
}

// Writes every changed field. On a fault, the ones already written get their old values back and *failed is the
// write that faulted.
bool commit(const Writes& ws, int* failed) {
    volatile int i = 0;
    __try {
        for (; i < ws.n; i = i + 1)
            if (ws.w[i].bits != ws.w[i].old) memcpy(ws.w[i].at, &ws.w[i].bits, ws.w[i].width);
        return true;
    } __except (readFault(GetExceptionCode())) {
        *failed = i;
    }
    for (int j = 0; j < *failed; ++j)
        if (ws.w[j].bits != ws.w[j].old) memcpy(ws.w[j].at, &ws.w[j].old, ws.w[j].width);  // just written: can't fault
    return false;
}

int faulted(rules::Rule& r, int k) {
    r.active.store(false, std::memory_order_relaxed);
    ++r.faults;
    return k;
}

}  // namespace

void rules::setPlayer(uint32_t id) { gPlayer.store(id, std::memory_order_relaxed); }

void rules::setParam(Rule& r, int i, double v) { r.param[i].store(std::bit_cast<uint64_t>(v), std::memory_order_relaxed); }

void rules::setList(Rule& r, const uint32_t* ids, uint32_t n) {
    uint32_t seq = r.listSeq.load(std::memory_order_relaxed);
    r.listSeq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    if (n > kMaxList) n = kMaxList;
    for (uint32_t i = 0; i < n; ++i) r.list[i].store(ids[i], std::memory_order_relaxed);
    r.listCount.store(n, std::memory_order_relaxed);
    r.listSeq.store(seq + 2, std::memory_order_release);
}

uint64_t rules::packOrder(const uint8_t* slots, int n) {
    uint64_t w = uint64_t(n) << 60;
    for (int i = 0; i < n; ++i) w |= uint64_t(slots[i] & 15) << (4 * i);
    return w;
}

int rules::unpackOrder(uint64_t w, uint8_t* slots) {
    int n = int(w >> 60);
    for (int i = 0; i < n; ++i) slots[i] = uint8_t((w >> (4 * i)) & 15);
    return n;
}

int rules::apply(Rule* const* rs, int n, const Frame& in, Frame& out, Result* result) {
    out = in;
    Writes ws;
    Result local;
    Result& res = result ? *result : local;
    res = {};
    uint32_t matched = 0;
    for (int k = 0; k < n; ++k) {
        Rule& r = *rs[k];
        if (!r.active.load(std::memory_order_relaxed)) continue;
        int m = stage(r, k, in, out, ws, res);
        if (m < 0) {
            res = {};  // no return change either: a faulted call gets none of the rules' changes
            return faulted(r, k);
        }
        if (m == 2) {
            out = in;
            r.hits.fetch_add(1, std::memory_order_relaxed);
            return -1;
        }
        if (m) matched |= 1u << k;
    }
    int bad = 0;
    if (!commit(ws, &bad)) {
        res = {};
        int first = -1;
        for (int k = n - 1; k >= 0; --k)
            if (ws.w[bad].rules >> k & 1) first = faulted(*rs[k], k);
        return first;
    }
    // Counted only once the whole call went through, so a fault leaves no partial counts either.
    for (int k = 0; k < n; ++k) if (matched & (1u << k)) rs[k]->hits.fetch_add(1, std::memory_order_relaxed);
    return -1;
}

uint64_t rules::after(Rule* const* rs, int n, uint16_t mask, uint64_t bits) {
    for (int k = 0; k < n; ++k) {
        if (!(mask >> k & 1)) continue;
        const auto& s = rs[k]->shape;
        for (int i = 0; i < s.afters; ++i) change(*rs[k], s.after[i], bits);
    }
    return bits;
}

bool rules::writesSame(const Shape& a, const Shape& b) {
    if ((a.block && b.block) || (a.afters && b.afters)) return true;
    for (int i = 0; i < a.changes; ++i)
        for (int j = 0; j < b.changes; ++j)
            if (a.change[i].at == b.change[j].at) return true;
    return false;
}
