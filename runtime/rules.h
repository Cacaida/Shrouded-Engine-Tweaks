// Interception rules (docs/architecture.md): conditions and value changes a mod declares once, applied by C++ on
// whichever thread calls the hooked function. At call time a rule takes no lock, allocates nothing and calls no Lua.
// A rule's shape is fixed when it registers; only its params, its id list and its active flag change afterwards, each
// atomically, so nothing is ever replaced under a running call and nothing needs reclaiming.
#pragma once
#include <atomic>
#include <cstdint>

namespace rules {

inline constexpr int kMaxConds = 8, kMaxChanges = 8, kMaxParams = 8, kMaxList = 64;
inline constexpr int kMaxRules = 15;  // per target: the order word holds 15 slot indices and a count
inline constexpr int kMaxWrites = 8;  // distinct fields one call changes: a catalog entry lists at most 8

enum Num : uint8_t { Signed, Unsigned, Float, Boolean };

// A value: an argument position, or a field of the struct an argument points to.
struct Loc {
    uint8_t arg = 0;    // 0..7
    uint8_t width = 8;  // 1, 2, 4 or 8 bytes
    Num num = Signed;
    bool field = false;
    uint32_t offset = 0;  // from the pointer, when field
    bool stack = false;   // a field change: the pointee must be on the calling thread's stack
    bool operator==(const Loc&) const = default;
};

// Player: SET's current player entity; a rule using it doesn't match while there is none.
enum Src : uint8_t { Const, Param, Player };
struct Operand {
    Src src = Const;
    uint8_t param = 0;
    double value = 0;
    bool operator==(const Operand&) const = default;
};

// In: the value is one of the rule's ids (setList).
enum Cmp : uint8_t { Eq, Ne, Lt, Le, Gt, Ge, In };
struct Cond {
    Loc at;
    Cmp cmp = Eq;
    Operand v;
    bool operator==(const Cond&) const = default;
};

// Min: at least v. Max: at most v.
enum Op : uint8_t { Set, Add, Mul, Min, Max };
struct Change {
    Loc at;
    Op op = Set;
    Operand v;
    bool operator==(const Change&) const = default;
};

struct Shape {
    uint8_t conds = 0, changes = 0;
    Cond cond[kMaxConds]{};
    Change change[kMaxChanges]{};
    bool block = false;  // a matching call doesn't run: it returns ret, converted to retAt (width 0: void)
    Operand ret;
    Loc retAt{0, 0};
    uint8_t afters = 0;  // changes to the return value of a matching call, each at retAt
    Change after[kMaxChanges]{};
    bool operator==(const Shape&) const = default;
};

struct Rule {
    Shape shape;
    std::atomic<uint64_t> param[kMaxParams]{};  // the doubles' bits
    std::atomic<uint32_t> listSeq{0}, listCount{0};
    std::atomic<uint32_t> list[kMaxList]{};
    std::atomic<bool> active{false};
    std::atomic<uint64_t> hits{0}, faults{0}, refused{0};
};

// The argument positions of one call as raw bits: an integer register, the low bits of an XMM register, or a stack
// slot, whichever the position's declared kind uses.
struct Frame {
    uint64_t a[8];
};

// A blocked call's return value: the bits for RAX (integers extended to 64 bits), or for XMM0 when fp.
struct Result {
    bool blocked = false, fp = false;
    uint64_t ret = 0;
    uint16_t after = 0;  // when not blocked: the positions of matching rules that change the return value
};

void setPlayer(uint32_t id);  // 0: none
void setParam(Rule& r, int i, double v);
// One writer at a time (the thread running the mods). A call reading the list while it changes doesn't match.
void setList(Rule& r, const uint32_t* ids, uint32_t n);

uint64_t packOrder(const uint8_t* slots, int n);  // n <= kMaxRules, each slot < 16
int unpackOrder(uint64_t word, uint8_t* slots);

// Applies rules[0..n) to one call, in that order. Every condition reads in, the original values; changes apply in
// order to staged copies, so a later rule changes what an earlier one left. Argument changes land in out, which starts
// as a copy of in; field changes are written to memory at the end, all of them or, if a write faults, none.
// A rule whose field change can't be made in this call (a null pointer, or a stack field whose pointee isn't on the
// calling thread's stack) doesn't apply at all and counts as refused.
// The first matching rule that blocks ends the call there: res says what it returns, and nothing else is committed
// (no argument or field change from the rules before it; only the blocking rule counts a hit).
// Returns -1, or the index of a rule whose field read or write faulted: it is switched off, nothing was written, and
// out must not be committed.
int apply(Rule* const* rules, int n, const Frame& in, Frame& out, Result* res = nullptr);

// Applies the after changes of rules[k], for each position k in mask, in order, to a return value's bits (RAX, or
// XMM0 for a float return). The same rules array the call's apply had.
uint64_t after(Rule* const* rules, int n, uint16_t mask, uint64_t bits);

// Both shapes change the same value or the return value, or both block: two mods doing that is reported as a conflict.
bool writesSame(const Shape& a, const Shape& b);

}  // namespace rules
