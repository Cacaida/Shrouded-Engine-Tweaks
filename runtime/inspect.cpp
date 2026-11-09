#include "inspect.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

namespace {

struct Edit {
    uint64_t serial;
    uint32_t comp, off;
    refl::Type leaf;
    uint8_t old[8], value[8];
    std::string label;
};

// What the window asked for since the last tick.
struct Wants {
    bool selNew = false, filterNew = false;
    uint32_t sel = 0, search = 0;
    std::string filter;
    std::set<uint32_t> open;
    std::vector<insp::Pin> add;
    std::vector<insp::Pin> drop;  // by id, comp, off
    std::vector<Edit> edits;
};

std::mutex gMx;  // gWants and gView; never held while the game is read
Wants gWants;
std::shared_ptr<const insp::View> gView = std::make_shared<insp::View>();
std::atomic<bool> gOpen, gOff;
uint32_t gListMs = 500, gDetailMs = 100;
size_t gBudget = 256 << 10;
constexpr double kSliceMs = 0.25;       // list work per tick
constexpr uint64_t kSlotsPerStep = 512;  // entity-map slots between time checks

// One walk of the entity map for the list, spread over ticks.
struct Pass {
    bool active = false;
    uint64_t cursor = 0, cap = 0;
    std::vector<insp::Row> rows;
    uint32_t matched = 0;
    int filter = -1;  // component index the rows must have; -1 none, -2 a name the world doesn't have
    double player[3] = {};
    bool havePlayer = false;
};

// The server thread's state.
struct State {
    uintptr_t world = 0;
    uint64_t session = 0, serial = 0;
    std::vector<uint32_t> hashes;  // component index -> type hash, this session
    int pos = -1, hp = -1, faction = -1, input = -1;
    uint32_t player = 0, sel = 0, search = 0;
    bool selGone = false, shownGone = false;  // shownGone: what the last published view said
    std::string filter, note;
    std::set<uint32_t> open;
    std::vector<insp::Pin> pins;
    Pass pass;
    std::vector<insp::Row> rows;
    uint32_t matched = 0;
    std::vector<insp::Comp> comps;
    size_t rot = 0;  // where the next detail refresh starts copying, so a truncated component gets its turn
    double lastPass = -1e18, lastDetail = -1e18;
};
State gS;
std::atomic<uint64_t> gSerials{0};

double nowMs() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
}

int indexOf(const char* name) {
    refl::Type t = refl::byName(name);
    return t ? ecs::compIndex(gS.world, t.hash()) : -1;
}

bool scalar(refl::Type u) {
    return u && u.prim() >= refl::Bool && u.prim() <= refl::Bitmask64 && u.size() >= 1 && u.size() <= 8;
}

// A new session, another world, or none (world 0): everything the window held refers to the old one.
void reset(uintptr_t world, uint64_t session) {
    gS.world = world;
    gS.session = session;
    gS.hashes = world ? ecs::compHashes(world) : std::vector<uint32_t>();
    gS.pos = world ? indexOf("CurrentTransform") : -1;
    gS.hp = world ? indexOf("Health") : -1;
    gS.faction = world ? indexOf("Faction") : -1;
    gS.input = world ? indexOf("PlayerInput") : -1;
    gS.player = 0;
    gS.selGone = gS.sel != 0;
    for (insp::Pin& p : gS.pins) p.gone = true;
    gS.pass = {};
    gS.rows.clear();
    gS.matched = 0;
    gS.comps.clear();
}

bool samePin(const insp::Pin& a, const insp::Pin& b) { return a.id == b.id && a.comp == b.comp && a.off == b.off; }

std::vector<Edit> takeWants() {
    Wants w;
    {
        std::lock_guard lock(gMx);
        w = std::move(gWants);
        gWants = {};
        gWants.open = w.open;
        gWants.filter = w.filter;
        gWants.search = w.search;
    }
    if (w.selNew) {
        gS.sel = w.sel;
        gS.serial = ++gSerials;
        gS.selGone = false;
        gS.comps.clear();
        gS.lastDetail = -1e18;
    }
    if (w.filterNew) {
        gS.filter = w.filter;
        gS.search = w.search;
        gS.pass = {};
        gS.lastPass = -1e18;
    }
    gS.open = std::move(w.open);
    for (const insp::Pin& d : w.drop)
        std::erase_if(gS.pins, [&](const insp::Pin& p) { return samePin(p, d); });
    for (insp::Pin& a : w.add) {
        bool dup = std::any_of(gS.pins.begin(), gS.pins.end(), [&](const insp::Pin& p) { return samePin(p, a); });
        if (!dup && gS.pins.size() < insp::kMaxPins) gS.pins.push_back(std::move(a));
    }
    return std::move(w.edits);
}

// Every tick: an id missing once is gone for good, whatever comes back under it later.
void presence() {
    if (gS.sel && !gS.selGone && !ecs::record(gS.world, gS.sel)) gS.selGone = true;
    for (insp::Pin& p : gS.pins)
        if (!p.gone && !ecs::record(gS.world, p.id)) p.gone = true;
}

std::string applyEdit(const Edit& e) {
    std::string what = e.label + ": ";
    if (e.serial != gS.serial || !gS.sel || gS.selGone)
        return what + "Not changed, that entity is gone (it left, or another world started)";
    if (!scalar(e.leaf)) return what + "Not changed, only numbers, bools and enums can be edited";
    refl::Type t = refl::byHash(e.comp);
    int idx = ecs::compIndex(gS.world, e.comp);
    uint8_t* p = idx >= 0 ? ecs::component(gS.world, gS.sel, idx) : nullptr;
    uint32_t n = e.leaf.size();
    if (!p || !t || e.off + n > t.size()) return what + "Not changed, the entity no longer has that component";
    if (memcmp(p + e.off, e.old, n) != 0)
        return what + "Not changed, it changed since you looked (now " + refl::text(p + e.off, e.leaf) + ")";
    memcpy(p + e.off, e.value, n);
    return what + "Set to " + refl::text(p + e.off, e.leaf);
}

insp::Row makeRow(uint32_t id, uintptr_t rec, const Pass& pass) {
    insp::Row r{id, -1.0f, {}, uint16_t(ecs::recordComps(rec).size()), false, 0, -1};
    if (const uint8_t* t = gS.pos >= 0 ? ecs::inRecord(rec, gS.pos) : nullptr) {
        for (int i = 0; i < 3; i++) r.pos[i] = double(reinterpret_cast<const int64_t*>(t)[i]) / 4294967296.0;
        if (pass.havePlayer) {
            double d2 = 0;
            for (int i = 0; i < 3; i++) d2 += (r.pos[i] - pass.player[i]) * (r.pos[i] - pass.player[i]);
            r.dist = float(std::sqrt(d2));
        }
    }
    if (const uint8_t* h = gS.hp >= 0 ? ecs::inRecord(rec, gS.hp) : nullptr) {
        r.hasHp = true;
        memcpy(&r.hp, h + 8, 4);  // dataStorage[0]
    }
    if (const uint8_t* f = gS.faction >= 0 ? ecs::inRecord(rec, gS.faction) : nullptr) r.faction = *f;
    return r;
}

void startPass() {
    Pass& p = gS.pass;
    p = {};
    p.active = true;
    if (!gS.filter.empty()) {
        refl::Type t = refl::lookup(gS.filter);
        p.filter = t ? ecs::compIndex(gS.world, t.hash()) : -1;
        if (p.filter < 0) p.filter = -2;
    }
    if (gS.input >= 0 && (!gS.player || !ecs::record(gS.world, gS.player))) gS.player = ecs::firstWith(gS.world, gS.input);
    uintptr_t prec = gS.player ? ecs::record(gS.world, gS.player) : 0;
    if (const uint8_t* t = prec && gS.pos >= 0 ? ecs::inRecord(prec, gS.pos) : nullptr) {
        for (int i = 0; i < 3; i++) p.player[i] = double(reinterpret_cast<const int64_t*>(t)[i]) / 4294967296.0;
        p.havePlayer = true;
    }
}

void finishPass(double now) {
    Pass& p = gS.pass;
    auto nearer = [](const insp::Row& a, const insp::Row& b) {
        if ((a.dist < 0) != (b.dist < 0)) return b.dist < 0;
        return a.dist != b.dist ? a.dist < b.dist : a.id < b.id;
    };
    size_t keep = (std::min)(p.rows.size(), insp::kMaxRows);
    std::partial_sort(p.rows.begin(), p.rows.begin() + ptrdiff_t(keep), p.rows.end(), nearer);
    p.rows.resize(keep);
    gS.rows = std::move(p.rows);
    gS.matched = p.matched;
    p = {};
    gS.lastPass = now;
}

// Some of the entity map per tick, within kSliceMs; a full pass every gListMs. True when a pass finished.
bool listStep(double now) {
    Pass& p = gS.pass;
    if (!p.active) {
        if (now - gS.lastPass < gListMs) return false;
        startPass();
    }
    if (gS.search || p.filter == -2) {
        uintptr_t rec = gS.search && p.filter != -2 ? ecs::record(gS.world, gS.search) : 0;
        if (rec && (p.filter < 0 || ecs::inRecord(rec, p.filter))) {
            p.rows.push_back(makeRow(gS.search, rec, p));
            p.matched = 1;
        }
        finishPass(now);
        return true;
    }
    auto add = [&](uint32_t id, uintptr_t rec) {
        if (p.filter >= 0 && !ecs::inRecord(rec, p.filter)) return;
        p.matched++;
        p.rows.push_back(makeRow(id, rec, p));
    };
    while (nowMs() - now < kSliceMs) {
        uint64_t cap = ecs::forSlots(gS.world, p.cursor, p.cursor + kSlotsPerStep, add);
        if (!cap || (p.cap && cap != p.cap)) {  // the map grew or isn't there: start over at the next pass
            p = {};
            return false;
        }
        p.cap = cap;
        p.cursor += kSlotsPerStep;
        if (p.cursor >= cap) {
            finishPass(now);
            return true;
        }
    }
    return false;
}

// The selected entity's components, copied whole when expanded, within gBudget; then the pins' values.
void refreshDetail() {
    std::map<uint32_t, std::vector<uint8_t>> last;  // a component past the budget keeps its last copy
    for (insp::Comp& c : gS.comps) last[c.hash] = std::move(c.data);
    gS.comps.clear();
    uintptr_t rec = gS.sel && !gS.selGone && gOpen ? ecs::record(gS.world, gS.sel) : 0;
    std::vector<int> idx = rec ? ecs::recordComps(rec) : std::vector<int>();
    size_t used = 0, n = idx.size();
    for (size_t k = 0; k < n; k++) {
        int i = idx[(k + gS.rot) % n];
        insp::Comp c{size_t(i) < gS.hashes.size() ? gS.hashes[size_t(i)] : 0, {}, false, false, {}};
        c.type = refl::byHash(c.hash);
        c.open = c.hash && gS.open.count(c.hash);
        size_t size = c.type ? c.type.size() : 0;
        if (c.open && size && used + size <= gBudget) {
            const uint8_t* p = ecs::inRecord(rec, i);
            c.data.assign(p, p + size);
            used += size;
        } else if (c.open && size) {
            c.truncated = true;
            c.data = std::move(last[c.hash]);
        }
        gS.comps.push_back(std::move(c));
    }
    gS.rot++;
    std::sort(gS.comps.begin(), gS.comps.end(), [](const insp::Comp& a, const insp::Comp& b) {
        if (bool(a.type) != bool(b.type)) return bool(a.type);
        return a.type ? a.type.qname() < b.type.qname() : a.hash < b.hash;
    });
    for (insp::Pin& p : gS.pins) {
        int i = p.gone ? -1 : ecs::compIndex(gS.world, p.comp);
        const uint8_t* c = i >= 0 ? ecs::component(gS.world, p.id, i) : nullptr;
        p.have = c != nullptr;
        if (c) memcpy(p.value, c + p.off, p.leaf.size());
    }
}

void publish(bool inWorld) {
    auto v = std::make_shared<insp::View>();
    v->inWorld = inWorld;
    v->off = gOff;
    v->rows = gS.rows;
    v->matched = gS.matched;
    v->sel = gS.sel;
    v->selSerial = gS.serial;
    v->selGone = gS.shownGone = gS.selGone;
    v->comps = gS.comps;
    v->pins = gS.pins;
    v->note = gS.note;
    std::lock_guard lock(gMx);
    gView = std::move(v);
}

}  // namespace

std::shared_ptr<const insp::View> insp::view() {
    std::lock_guard lock(gMx);
    return gView;
}

void insp::setOpen(bool open) { gOpen = open; }

void insp::select(uint32_t id) {
    std::lock_guard lock(gMx);
    gWants.selNew = true;
    gWants.sel = id;
}

void insp::setFilter(const std::string& comp, uint32_t id) {
    std::lock_guard lock(gMx);
    gWants.filterNew = true;
    gWants.filter = comp;
    gWants.search = id;
}

void insp::expand(uint32_t comp, bool open) {
    std::lock_guard lock(gMx);
    if (open) gWants.open.insert(comp);
    else gWants.open.erase(comp);
}

void insp::pin(uint32_t id, uint32_t comp, uint32_t off, refl::Type leaf, std::string label) {
    if (!scalar(leaf)) return;
    std::lock_guard lock(gMx);
    gWants.add.push_back({id, comp, off, leaf, std::move(label), false, false, {}});
}

void insp::unpin(uint32_t id, uint32_t comp, uint32_t off) {
    std::lock_guard lock(gMx);
    gWants.drop.push_back({id, comp, off, {}, {}, false, false, {}});
}

void insp::edit(uint64_t serial, uint32_t comp, uint32_t off, refl::Type leaf, const void* old, const void* value,
                std::string label) {
    if (!scalar(leaf)) return;
    Edit e{serial, comp, off, leaf, {}, {}, std::move(label)};
    memcpy(e.old, old, leaf.size());
    memcpy(e.value, value, leaf.size());
    std::lock_guard lock(gMx);
    gWants.edits.push_back(std::move(e));
}

void insp::tick(uintptr_t world, uint64_t session) {
    if (gOff) return;
    if (!world) {
        if (gS.world) {
            reset(0, 0);
            publish(false);
        }
        return;
    }
    if (world != gS.world || session != gS.session) reset(world, session);
    std::vector<Edit> edits = takeWants();
    presence();
    bool changed = !edits.empty();
    for (const Edit& e : edits) gS.note = applyEdit(e);
    double now = nowMs();
    if (gOpen) changed |= listStep(now);
    if ((gOpen || !gS.pins.empty()) && now - gS.lastDetail >= gDetailMs) {
        refreshDetail();
        gS.lastDetail = now;
        changed = true;
    }
    if (changed || gS.selGone != gS.shownGone) publish(true);
}

void insp::turnOff() {
    gOff = true;
    auto v = std::make_shared<insp::View>();
    v->off = true;
    std::lock_guard lock(gMx);
    gView = std::move(v);
}

void insp::tune(uint32_t listMs, uint32_t detailMs, size_t budget) {
    gListMs = listMs;
    gDetailMs = detailMs;
    gBudget = budget;
}

void insp::testView(View v) {
    auto p = std::make_shared<View>(std::move(v));
    std::lock_guard lock(gMx);
    gView = std::move(p);
}
