// Hooks on game functions (docs/architecture.md). A detour first applies the target's interception rules (rules.h) to
// the call's arguments, natively, then captures them for observers and returns to the original at once: the hooked
// thread never waits on Lua, and Lua never runs inside the call. Captured events wait in a bounded queue per
// subscriber until the mods' next tick drains them.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "game.h"
#include "rules.h"

namespace hk {

// What a declared argument position holds, and how it's captured.
enum Kind : uint8_t {
    Skip, Bool, U8, U16, U32, U64, I8, I16, I32, I64, F32, F64,
    Entity,     // a u32 entity id passed by value
    EntityPtr,  // a pointer to a u32 entity id, read at call time
    Snapshot,   // a pointer to a fixed-size reflected type, copied at call time
    CStr,       // a pointer to a NUL-terminated string, copied at call time (up to kMaxStr)
};
inline constexpr int kMaxArgs = 8, kMaxSubs = 8, kMaxTargets = 64;
inline constexpr uint32_t kMaxStr = 256, kMaxSnapshot = 1024;
inline constexpr uint32_t kMaxQueue = 4096;
inline constexpr size_t kMemoryBudget = 64 * 1024 * 1024;

struct Arg {
    Kind kind = Skip;
    refl::Type type;  // Snapshot only
};
// A kind by its Hooks.observe name ("i32", "entity*", ...); false for a reflected "Type*" or an unknown name.
bool kindOf(std::string_view name, Kind& out);

// One captured argument. Scalars sit in bits (f32/f64 as their bit patterns); a snapshot or string is len bytes at
// off in its batch's payload. Snapshots/strings use bits=0 for nil, bits=1 for a copy. EntityPtr holds the id in bits
// and len=4 (len=0 means nil). No pointer address is exposed.
struct Value {
    uint64_t bits;
    uint32_t off, len;
};

struct Event {
    uint64_t seq;      // capture attempt order, not gameplay order or queue publication order
    int64_t qpc;       // QueryPerformanceCounter at capture
    uint64_t dropped;  // overflow since the last nonempty drain, on the first event only
    Value v[kMaxArgs];
};

// One subscriber's events since its last drain, sorted by seq.
struct Batch {
    uint64_t generation = 0;
    std::vector<Event> events;
    std::vector<uint8_t> payload;
};

using Log = void (*)(const char* line);

void setLog(Log log);
void flushLogs();  // logs new refusals and faults; never called from a capture, which only records them
std::vector<std::string> diagnostics();  // last 32 refusals/faults
uint64_t problems();
double seconds(int64_t qpc);
size_t retainedBytes();
// A detour SET installs itself (world tick, admin consumer...): observing it is refused with a clear message.
void reserve(uintptr_t fn, const char* what);
// The address of the only match of a pattern like "48 89 5C 24 ?? 57" in imageBase's .text, matched against the
// original bytes from the image's file (no detour, SET's or another mod's, hides a target); 0 with err set if none,
// several or a bad pattern.
uintptr_t findPattern(uintptr_t imageBase, std::string_view pattern, std::string& err);
// Whether the original bytes at `at` (inside imageBase's .text) match the pattern. err is set only for a bad pattern
// or an address outside .text.
bool matchAt(uintptr_t imageBase, uintptr_t at, std::string_view pattern, std::string& err);
void restoreCode(uintptr_t start, uint8_t* copy, size_t size);  // scanner's private copy, never executable memory

// Subscribes key ("<mod>/<name>") to the function at fn. A key subscribed before (an earlier Lua state) is reused when
// fn, args and capacity match, so reloads don't grow memory. Returns a subscriber id, or -1 with err set. Every
// position must agree with the target's other subscribers (same kind, or Skip on either side). Refused: SET's own
// detours and targets 1-4 bytes from another hooked entry.
int subscribe(uintptr_t fn, const std::string& key, const std::vector<Arg>& args, uint32_t capacity, std::string& err);
void unsubscribe(int sub);
void unsubscribeAll();  // a Lua state closes: every subscriber off, queued events discarded
void discardPending();  // world transition: subscribers stay active, captured and queued events are discarded
// The subscriber's events since its last drain. Single consumer: the thread running the mods.
bool drain(int sub, Batch& out);
bool delivered(int sub, uint64_t generation);  // immediately before invoking a handler; false if now stale
void refuse(const std::string& message);       // record a Lua registration failure too

// Interception rules on the function at fn. Same target and key rules as subscribe; a target holds 15 retained rules.
struct RuleDecl {
    std::string key;  // "<mod>/<name>"
    std::vector<Arg> args;  // every position up to the last one a condition or change uses
    rules::Shape shape;
    uint32_t order = 0;     // the mod's load order: rules on a target run by it, then in registration order
    std::vector<double> params;  // initial values, one per Param operand index
    std::vector<uint32_t> list;  // initial ids for In
    std::vector<rules::Loc> fields;  // a catalog target's fields: the only ones a change may write
    std::vector<rules::Loc> reads;   // its read-only fields: conditions only
    bool block = false;              // the catalog lets rules block the call (shape.retAt is its return kind)
    bool after = false;              // the catalog lets rules change the return value
};
// A rule value for argument position pos declared as a; false for a pointer or string kind.
bool argLoc(const Arg& a, uint8_t pos, rules::Loc& out);
// A number inside what argument a points to: a field path of its reflected type, or "" for an entity*.
bool fieldLoc(const Arg& a, uint8_t pos, std::string_view path, rules::Loc& out, std::string& err);
// Whether d is a rule addRule would take, apart from its target: checked before anything else, on every build.
bool validRule(const RuleDecl& d, std::string& err);
// A rule id, or -1 with err set. conflict names another mod's active rule changing one of the same values.
int addRule(uintptr_t fn, const RuleDecl& d, std::string& err, std::string& conflict);
void removeRule(int id);
bool setParam(int id, int i, double v);
bool setList(int id, const std::vector<uint32_t>& ids);

// The overlay's Hooks window and the selftest, from any thread.
struct SubInfo {
    std::string key;
    bool active;
    uint64_t delivered, dropped, discarded, lockWaits;
    uint32_t highWater, capacity;
};
struct RuleInfo {
    std::string key, conflict;
    bool active, faulted;
    uint64_t hits, faults, refused;  // refused: calls whose field change wasn't allowed (pointee not on the stack)
};
struct TargetInfo {
    uintptr_t fn;
    bool faulted, timing;
    uint64_t calls, captureTicks, timedCalls, decoded, lockWaits;
    std::vector<SubInfo> subs;
    uint64_t ruleCalls = 0, ruleTicks = 0, ruleTimed = 0;
    std::vector<RuleInfo> rules;
};
std::vector<TargetInfo> info();
void setTiming(uintptr_t fn, bool on);

}  // namespace hk
