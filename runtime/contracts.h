// The interception catalog: game functions SET reverse-engineered itself, with everything a rule needs to know about
// each and what a rule may do to it. Pointer-field writes, blocking and return changes work only on these targets; a
// mod's own pattern gets observation and scalar argument rules (docs/architecture.md).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hooks.h"

namespace contracts {

enum Cap : uint8_t { Args = 1, Fields = 2, Block = 4, After = 8 };

// Bytes at fn + offset, written like a pattern ("8B 8A BC 00 00 00", ?? allowed). They tie the entry to what the RE
// found inside the function (the instruction reading a field, a call to a known callee), not only to its prologue.
struct Anchor {
    uint32_t offset;
    const char* bytes;
};

struct Build {
    uint32_t build;
    const char* pattern;
    std::vector<Anchor> anchors;
};

// A field a rule may read and change, through the pointer in argument arg (0-based): its offset and kind (bool, i8..u64,
// f32, f64, entity) come from the RE, not from reflection. stack: the pointee is on the calling thread's stack, checked
// at every call. Otherwise the RE showed the calling thread owns it; an entry lists at most one such writable field, so
// no call changes more than one field another thread could see. readOnly: conditions only, never changed.
struct Field {
    uint8_t arg;
    const char* name;
    uint32_t offset;
    const char* kind;
    bool stack;
    bool readOnly = false;
};

struct Entry {
    const char* name;
    std::vector<const char*> args;  // every position, as Hooks.observe declares them (no reflected Type*)
    const char* ret;                // void, bool, i8..u64, f32, f64, or hidden (an aggregate returned through a pointer)
    uint8_t caps;
    std::vector<Field> fields;
    std::vector<Build> builds;
    const char* note;  // from the RE: why each capability is safe on this function
};

const std::vector<Entry>& catalog();

struct Status {
    std::string name;
    uintptr_t fn = 0;        // 0 while off
    std::string why;         // empty when the entry checked out
    uint32_t contract = 0;   // the listed build whose pattern and anchors matched
    const Entry* entry = nullptr;
};

// Every entry on this image and build: well formed, the build listed, the pattern found exactly once, every anchor
// matching. A failing entry is off; the others are unaffected. anyBuild (runtime_selftest --patterns, after a game
// update) tries every listed build's pattern and anchors instead, newest first, to show which contract still matches.
std::vector<Status> check(uintptr_t imageBase, uint32_t build, const std::vector<Entry>& entries, bool anyBuild = false);

// The startup check's result, for rules, the Status window and Features().
void setChecked(std::vector<Status> s);
std::vector<Status> checked();
bool find(std::string_view name, Status& out);

// What a rule on e works with: its argument declarations, and its fields as rule values.
std::vector<hk::Arg> args(const Entry& e);
rules::Loc loc(const Field& f);
rules::Loc retLoc(const Entry& e);  // the return kind; width 0 for void

}  // namespace contracts
