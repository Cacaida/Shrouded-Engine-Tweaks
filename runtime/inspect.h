// The Entities window's data (docs/architecture.md, Entity inspector). The server thread reads the game at the read
// point (tick, from the world-tick hook) and publishes copies; the render thread only ever sees those copies. The
// selected and pinned entities are checked at every tick, and one that is missing once is gone for good: ids come
// back as new objects when an area streams in again.
#pragma once
#include "game.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace insp {

inline constexpr size_t kMaxRows = 200, kMaxPins = 64;

struct Row {
    uint32_t id;
    float dist;       // metres from the player; -1 without a position (or no player)
    double pos[3];    // metres
    uint16_t comps;   // component count
    bool hasHp;
    int32_t hp;       // Health.dataStorage[0]
    int16_t faction;  // Faction.faction, -1 without one
};

struct Comp {
    uint32_t hash;  // the component type's hash
    refl::Type type;
    bool open;       // expanded in the window: data holds the whole component, unless truncated
    bool truncated;  // open, but past this refresh's byte budget: copied at a later refresh
    std::vector<uint8_t> data;
};

struct Pin {
    uint32_t id, comp, off;  // entity, component type hash, the field's offset in the component
    refl::Type leaf;         // a number, bool or enum, at most 8 bytes
    std::string label;       // "Health.dataStorage[0]"
    bool gone;               // the entity was missing at a tick: never read again
    bool have;               // value holds a read (the component was there at the last refresh)
    uint8_t value[8];
};

struct View {
    bool inWorld;       // a world you host is ticking and the inspector can read it
    bool off;           // turned off after an access violation, until the game restarts
    std::vector<Row> rows;  // nearest first, at most kMaxRows
    uint32_t matched;       // entities that passed the filter in the last list pass
    uint32_t sel;           // selected entity, 0 if none
    uint64_t selSerial;     // changes with every selection; edits carry it
    bool selGone;
    std::vector<Comp> comps;  // the selected entity's components, by type name
    std::vector<Pin> pins;
    std::string note;  // the last edit's result
};

std::shared_ptr<const View> view();  // any thread; never null

// From the window (render thread). Applied at the next tick.
void setOpen(bool open);  // the window is open: the list and the selected entity refresh
void select(uint32_t id);
void setFilter(const std::string& comp, uint32_t id);  // comp "" for any component, id 0 for no id search
void expand(uint32_t comp, bool open);
void pin(uint32_t id, uint32_t comp, uint32_t off, refl::Type leaf, std::string label);
void unpin(uint32_t id, uint32_t comp, uint32_t off);
// Changes the selected entity's field at off in component comp from old (the value the window showed) to value;
// leaf.size() bytes each. Applied only if the selection is still serial and present, and the field still holds old.
void edit(uint64_t serial, uint32_t comp, uint32_t off, refl::Type leaf, const void* old, const void* value,
          std::string label);

// The server thread, right after rt::tick: world is the world you host when Lua had it this tick, else 0. session
// changes with every new session (a load, another world).
void tick(uintptr_t world, uint64_t session);
void turnOff();  // after an access violation in tick

// Self-tests: refresh rates in ms and the byte budget per refresh; a view for overlay_selftest to draw.
void tune(uint32_t listMs, uint32_t detailMs, size_t budget);
void testView(View v);

}  // namespace insp
