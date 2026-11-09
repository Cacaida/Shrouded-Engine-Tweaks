#include "contracts.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>

#include "hooks.h"

namespace {

std::mutex gMx;
std::vector<contracts::Status> gChecked;

bool knownRet(std::string_view r) {
    for (const char* k : {"void", "bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "hidden"})
        if (r == k) return true;
    return false;
}

bool kindKnown(const char* name, hk::Kind& k) { return name && hk::kindOf(name, k); }

std::string badField(const contracts::Entry& e, size_t i) {
    const auto& f = e.fields[i];
    hk::Kind k;
    rules::Loc l;
    if (f.arg >= e.args.size() || !f.name || !*f.name) return "field on an undeclared argument";
    if (!kindKnown(f.kind, k) || !hk::argLoc({k, {}}, f.arg, l)) return "field kind isn't a number";
    if (f.offset > 4096) return "field offset past 4096";
    for (size_t j = 0; j < i; ++j) {
        const auto& g = e.fields[j];
        if (g.arg == f.arg && (!strcmp(g.name, f.name) || (g.offset < f.offset + l.width && f.offset < g.offset + contracts::loc(g).width)))
            return "fields overlap or share a name";
    }
    return {};
}

// Empty if the entry itself is consistent.
std::string malformed(const contracts::Entry& e) {
    using namespace contracts;
    if (!e.name || !*e.name) return "no name";
    if (e.args.size() > 8) return "more than eight argument positions";
    hk::Kind k;
    for (const char* a : e.args)
        if (!kindKnown(a, k)) return "unknown argument kind";
    if (!e.ret || !knownRet(e.ret)) return "unknown return kind";
    if (!e.caps || e.caps > (Args | Fields | Block | After)) return "no or unknown capabilities";
    if ((e.caps & (Block | After)) && std::string_view(e.ret) == "hidden") return "hidden return can't be blocked or changed";
    if ((e.caps & After) && std::string_view(e.ret) == "void") return "a void return can't be changed";
    bool writable = std::any_of(e.fields.begin(), e.fields.end(), [](const Field& f) { return !f.readOnly; });
    if (bool(e.caps & Fields) != writable) return "fields capability and writable fields disagree";
    if (e.fields.size() > rules::kMaxWrites) return "more than eight fields";
    int shared = 0;
    for (size_t i = 0; i < e.fields.size(); ++i) {
        if (auto why = badField(e, i); !why.empty()) return why;
        shared += !e.fields[i].stack && !e.fields[i].readOnly;
    }
    if (shared > 1) return "more than one field off the stack";
    if (!e.note || !*e.note) return "no RE note";
    return {};
}

// Empty if b's pattern and anchors check out on this image; fn gets the function.
std::string locate(uintptr_t base, const contracts::Build& b, uintptr_t& fn) {
    std::string err;
    fn = hk::findPattern(base, b.pattern, err);
    if (!fn) return "Pattern: " + err;
    for (size_t i = 0; i < b.anchors.size(); ++i)
        if (!hk::matchAt(base, fn + b.anchors[i].offset, b.anchors[i].bytes, err)) {
            fn = 0;
            return "Anchor " + std::to_string(i + 1) + (err.empty() ? std::string(" differs") : ": " + err);
        }
    return {};
}

contracts::Status checkEntry(uintptr_t base, uint32_t build, const contracts::Entry& e, bool anyBuild) {
    contracts::Status s{e.name ? e.name : "", 0, malformed(e), 0, &e};
    if (!s.why.empty()) return s;
    s.why = "Untested build";
    for (auto b = e.builds.rbegin(); b != e.builds.rend(); ++b) {
        if (!anyBuild && b->build != build) continue;
        s.why = locate(base, *b, s.fn);
        if (s.why.empty()) { s.contract = b->build; break; }
    }
    return s;
}

}  // namespace

// Each entry comes with the RE that verified it (docs/architecture.md, Interception rules).
const std::vector<contracts::Entry>& contracts::catalog() {
    // Pattern and anchors are the same on every listed build. Anchors: the two reads of argument 2, and the last ret.
    static const char* xp = "4C 8B 54 24 28 0F 28 E2 4D 8B 42 20 4D 85 C0 75 03 33 C0 C3 80 7A 04 00 74 04 8B 02 EB 02 "
                            "8B C1 F3 0F 10 1D ?? ?? ?? ?? 0F 57 C9 F3 41 0F 10 90 A4 00 00 00";
    static const std::vector<Anchor> xpAnchors = {{0x14, "80 7A 04 00"}, {0x1A, "8B 02"}, {0xEC, "C3 CC"}};
    // Anchors: the healthChange read, the HitEvent post (hash, size) and healthChange copied into it.
    static const char* hit = "4C 89 44 24 18 48 89 54 24 10 55 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 "
                             "EC ?? ?? ?? ?? 49 8B 38 4C 8B E1 49 8B 48 38 4D 8B E9 48 89 7C 24 ?? 4D 8B F8 4C 8B F2 E8 "
                             "?? ?? ?? ?? 84 C0 75";
    static const std::vector<Anchor> hitAnchors = {
        {0x3E7, "41 8B B5 BC 00 00 00"}, {0x861, "BA E0 57 0D CC 41 B8 F8 00 00 00"}, {0x94F, "89 B3 BC 00 00 00"}};
    static const std::vector<Entry> entries = {
        // Levels and player count are u32: the function zero-extends them into a 64-bit convert, so a negative
        // level reads as about 4.29 billion and hits the XP cap. u32 rules stop at 0 instead. The return is u32 too:
        // the function returns rax, but the caller keeps eax and stores it as a dword, so 64-bit values would wrap.
        {"combat_xp", {"u32", "skip", "f32", "u32", "skip"}, "u32", Args | Fields | Block | After,
         {{1, "level", 0, "u32", true}, {1, "isSet", 4, "bool", true}},
         {{1792148057, xp, xpAnchors}, {1792507660, xp, xpAnchors}, {1793791052, xp, xpAnchors}},
         "combat XP for one player and one kill: (player level, the victim's optional level, the victim's "
         "ExperienceSource.xpMod, players sharing, context). Leaf without calls, locks or writes. Its only caller (combat_experience_source) passes "
         "argument 2 as a slot in its own frame, rewritten before every call and not read after it. Block: nothing "
         "else happens inside it, and the caller only adds the returned eax to the player's XP and posts it in an "
         "XPGainedEvent. After: a leaf that makes no calls, so nothing in it reads its return address or walks the "
         "stack, and the deeper stack can't matter."},
        {"hit", {"skip", "skip", "skip", "skip"}, "void", Fields,
         {{3, "healthChange", 0xBC, "i32", true},
          {3, "targetId", 0x94, "entity", true, true},
          {3, "rootTargetId", 0xA0, "entity", true, true},
          {3, "sourceId", 0xA4, "entity", true, true},
          {3, "rootSourceId", 0x90, "entity", true, true}},
         {{1792148057, hit, hitAnchors}, {1792507660, hit, hitAnchors}, {1793791052, hit, hitAnchors}},
         "one hit landing: (out flag, the target's attributes, the impact context, the pending hit). Its only caller, "
         "impact_hit_conclusion, passes argument 4 as a copy in its own frame (lea r9, [rbp+240h] at all three call "
         "sites), laid out like a HitEvent. healthChange is read once, added to the target's health attribute and "
         "only then copied into the HitEvent it posts, so a change reaches both. The entity ids are read-only: the "
         "caller looked the target up before the call, and nothing shows what changing them would do."},
    };
    return entries;
}

std::vector<contracts::Status> contracts::check(uintptr_t base, uint32_t build, const std::vector<Entry>& entries,
                                               bool anyBuild) {
    std::vector<Status> out;
    for (const auto& e : entries) out.push_back(checkEntry(base, build, e, anyBuild));
    return out;
}

void contracts::setChecked(std::vector<Status> s) {
    std::lock_guard lock(gMx);
    gChecked = std::move(s);
}

std::vector<contracts::Status> contracts::checked() {
    std::lock_guard lock(gMx);
    return gChecked;
}

std::vector<hk::Arg> contracts::args(const Entry& e) {
    std::vector<hk::Arg> out(e.args.size());
    for (size_t i = 0; i < e.args.size(); ++i) hk::kindOf(e.args[i], out[i].kind);
    return out;
}

rules::Loc contracts::loc(const Field& f) {
    hk::Kind k = hk::Skip;
    rules::Loc l;
    hk::kindOf(f.kind, k);
    hk::argLoc({k, {}}, f.arg, l);
    l.field = true;
    l.offset = f.offset;
    l.stack = f.stack;
    return l;
}

rules::Loc contracts::retLoc(const Entry& e) {
    hk::Kind k = hk::Skip;
    rules::Loc l{0, 0};
    if (std::string_view(e.ret) != "void" && hk::kindOf(e.ret, k)) hk::argLoc({k, {}}, 0, l);
    return l;
}

bool contracts::find(std::string_view name, Status& out) {
    std::lock_guard lock(gMx);
    for (const auto& s : gChecked)
        if (s.name == name) { out = s; return true; }
    return false;
}
