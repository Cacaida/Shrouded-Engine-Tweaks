#include "writelog.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <vector>

#include "game.h"
#include "level.h"

namespace {

struct Rec {
    wlog::Kind kind;
    const void* base;
    std::vector<uint8_t> orig, latest;  // same size: the record's bytes
    std::vector<std::string> writers;
};
std::map<uintptr_t, Rec> gRecs;  // by first byte; records never overlap
size_t gBytes;
size_t gBudget = 16 << 20;  // far beyond what a mod changes: every item's every field is a few MB

size_t cost(size_t n) { return 2 * n + 64; }
uintptr_t stop(const std::pair<const uintptr_t, Rec>& r) { return r.first + r.second.orig.size(); }

// The bytes there now. False if the value can't be reached any more: the options of a user who signed out since
// (checked, unless a write to it is about to happen anyway).
bool read(const Rec& r, uintptr_t at, uint8_t* out, bool check = true) {
    if (r.kind == wlog::Sys) {
        out[0] = sys::isOn(*static_cast<const sys::System*>(r.base));
        return true;
    }
    if (check && r.kind == wlog::Opt && r.base != opts::machine() && r.base != opts::user()) return false;
    memcpy(out, reinterpret_cast<const void*>(at), r.orig.size());
    return true;
}

bool restore(const Rec& r, uintptr_t at) {
    if (r.kind == wlog::Sys) {
        sys::setOn(*static_cast<const sys::System*>(r.base), r.orig[0] != 0);
        return true;
    }
    if (r.kind == wlog::Opt) {
        memcpy(reinterpret_cast<void*>(at), r.orig.data(), r.orig.size());
        return true;
    }
    uint32_t prot = 0;
    return res::write(reinterpret_cast<void*>(at), r.orig.data(), r.orig.size(), prot);
}

// For log lines: "system fall_damage_infliction", "GameOptions.user() at +0x2c", "ItemInfo Test_Wood_Log at +0x10".
std::string what(const Rec& r, uintptr_t at) {
    if (r.kind == wlog::Sys) return "System " + std::string(static_cast<const sys::System*>(r.base)->name);
    char off[32];
    snprintf(off, sizeof off, " at +0x%llx", static_cast<unsigned long long>(at - reinterpret_cast<uintptr_t>(r.base)));
    if (r.kind == wlog::Opt) return (r.base == opts::user() ? "GameOptions.user()" : "GameOptions.machine()") + std::string(off);
    for (const auto& [t, n] : res::types())
        for (const res::Entry* e : res::ofType(t.hash()))
            if (res::data(*e) == r.base) {
                std::string_view l = res::label(*e);
                return std::string(t.qname()) + " " + (l.empty() ? res::guidStr(e->guid) : std::string(l)) + off;
            }
    return "a resource" + std::string(off);
}

// The last writer left: the original goes back if the value is still the latest logged one. 1 if it was restored.
int finish(uintptr_t at, const Rec& r, wlog::Sink sink) {
    std::vector<uint8_t> now(r.orig.size());
    std::string why;
    if (!read(r, at, now.data())) why = " can't be reached any more: Not restored";
    else if (now == r.orig) return 0;
    else if (now != r.latest) why = " was changed outside the mods (console, overlay or the game): Left as it is";
    else if (!restore(r, at)) why = " couldn't be restored";
    else return 1;
    sink((LV_WARN "Write Log: " + what(r, at) + why).c_str());
    return 0;
}

void summary(const std::string& who, int n, wlog::Sink sink) {
    if (n) sink((LV_OK "Write Log: " + who + std::to_string(n) + " change(s) undone").c_str());
}

}  // namespace

bool wlog::reserve(Kind k, const void* base, const uint8_t* p, uint32_t n, const std::string& mod) {
    uintptr_t lo = reinterpret_cast<uintptr_t>(p), hi = lo + n;
    auto last = gRecs.lower_bound(hi), first = last;  // [first, last): the records overlapping [lo, hi)
    while (first != gRecs.begin() && stop(*std::prev(first)) > lo) --first;
    if (first != last && std::next(first) == last && first->first <= lo && hi <= stop(*first)) {
        auto& w = first->second.writers;  // inside one record: nothing new to save
        if (std::find(w.begin(), w.end(), mod) == w.end()) w.push_back(mod);
        return true;
    }
    uintptr_t a = first == last ? lo : std::min(lo, first->first), b = first == last ? hi : std::max(hi, stop(*std::prev(last)));
    size_t old = 0;
    for (auto it = first; it != last; ++it) old += cost(it->second.orig.size());
    if (gBytes - old + cost(b - a) > gBudget) return false;
    Rec m{k, base, std::vector<uint8_t>(b - a), {}, {mod}};
    read(m, a, m.orig.data(), false);  // the game's original wherever no record had it yet
    m.latest = m.orig;
    for (auto it = first; it != last; ++it) {
        const Rec& r = it->second;
        std::copy(r.orig.begin(), r.orig.end(), m.orig.begin() + (it->first - a));
        std::copy(r.latest.begin(), r.latest.end(), m.latest.begin() + (it->first - a));
        for (const std::string& w : r.writers)
            if (std::find(m.writers.begin(), m.writers.end(), w) == m.writers.end()) m.writers.push_back(w);
    }
    gRecs.erase(first, last);
    gBytes += cost(b - a) - old;
    gRecs.emplace(a, std::move(m));
    return true;
}

void wlog::wrote(const uint8_t* p, const void* src, uint32_t n) {
    auto at = reinterpret_cast<uintptr_t>(p);
    auto it = gRecs.upper_bound(at);
    if (it == gRecs.begin() || stop(*--it) < at + n) return;  // reserve wasn't called: not logged
    memcpy(it->second.latest.data() + (at - it->first), src, n);
}

void wlog::leave(const std::string& mod, Sink sink) {
    int n = 0;
    for (auto it = gRecs.begin(); it != gRecs.end();) {
        auto& w = it->second.writers;
        auto mine = std::find(w.begin(), w.end(), mod);
        if (mine == w.end()) { ++it; continue; }
        w.erase(mine);
        if (!w.empty()) {  // the value stays as the others left it; it goes back when the last of them leaves
            std::string others;
            for (const std::string& o : w) others += (others.empty() ? "" : ", ") + o;
            sink((LV_WARN "Write Log: " + what(it->second, it->first) + " stays changed: " + others + " changed it too")
                     .c_str());
            ++it;
            continue;
        }
        n += finish(it->first, it->second, sink);
        gBytes -= cost(it->second.orig.size());
        it = gRecs.erase(it);
    }
    summary(mod + ": ", n, sink);
}

void wlog::leaveAll(Sink sink) {
    int n = 0;
    for (const auto& [at, r] : gRecs) n += finish(at, r, sink);
    gRecs.clear();
    gBytes = 0;
    summary("", n, sink);
}

size_t wlog::records() { return gRecs.size(); }
void wlog::setBudget(size_t bytes) { gBudget = bytes; }
