// Game-side access for the runtime: reflection, code patterns, ECS, events, systems, options and resources. Works on
// game memory directly; the only game function it calls is the camera settings push (opts). Layouts:
// docs/reflection-format.md (reflection) and docs/architecture.md (the rest).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refl {

enum Prim : uint8_t {
    Bool = 1, U8, S8, U16, S16, U32, S32, U64, S64, F32, F64,
    Enum, Bitmask8, Bitmask16, Bitmask32, Bitmask64, Typedef, Struct, StaticArray,
    DsArray, DsString, DsOptional, DsVariant, BlobArray, BlobString, BlobOptional, BlobVariant, ObjectReference, Guid,
};

// View over one TypeMetadata descriptor (0x90 bytes) in game memory.
struct Type {
    const uint8_t* p = nullptr;
    explicit operator bool() const { return p != nullptr; }
    std::string_view name() const { return str(0x00); }
    std::string_view qname() const { return str(0x20); }
    Type inner() const { return {*reinterpret_cast<const uint8_t* const*>(p + 0x38)}; }  // struct: base class
    uint32_t size() const { return u32(0x40); }
    uint32_t fieldCount() const { return u32(0x48); }
    uint8_t prim() const { return p[0x4c]; }
    uint32_t hash() const { return u32(0x50); }  // FNV-1a of qname
    Type resolved() const;                       // follows typedefs to the underlying type
    const uint8_t* defaults() const;             // the game's default value, size() bytes; null if it has none

private:
    std::string_view str(size_t off) const;
    uint32_t u32(size_t off) const { return *reinterpret_cast<const uint32_t*>(p + off); }
};

struct Field {
    std::string_view name;
    Type type;
    uint32_t offset;
};

bool init(uintptr_t imageBase);  // finds the table in the loaded image
size_t count();
Type at(size_t i);
Type byHash(uint32_t qualifiedHash);
Type byName(std::string_view name);  // "Health" means keen::ecs::Health; names with "::" are used as-is
// byName, else the only type with that short name outside keen::ds, else the only one at all ("ItemInfo" is
// keen::ItemInfo). None if several share it.
Type lookup(std::string_view name);
// Fixed-size data only: scalars, GUIDs, references, and structs or static arrays of them. Blob and Ds values point
// elsewhere, so a byte copy would break them or share the game's heap memory.
bool plain(Type t);
bool field(Type t, std::string_view name, Field& out);  // includes base-class fields
void forFields(Type t, const std::function<void(const Field&)>& fn);  // base classes first
void forEnum(Type t, const std::function<void(std::string_view name, int64_t value)>& fn);  // an enum's values
// A wrapper struct (one field named value, id or index: EntityId {id}, Time {value}) as its field's type; else t.
Type unwrap(Type t);
// A value inside t by path ("dataStorage[0]", "transform.position.x"): its offset and its type, a wrapper unwrapped.
// Static arrays only; false if any step doesn't exist.
bool path(Type t, std::string_view path, uint32_t& off, Type& leaf);
uint32_t fnv(std::string_view s);
bool enumHas(Type t, int64_t v);              // one of the enum's values; an enum that lists none takes any
std::string text(const uint8_t* p, Type u);  // a scalar as text: numbers, bools, bitmasks in hex, enums by name
// The first field of the struct at p that a real one can't hold: a bool other than 0 or 1, or a value its enum doesn't
// list. Nested structs included; the offset is from p. Empty name if there is none.
Field badField(Type t, const uint8_t* p);

}  // namespace refl

// Prologue of the ECS world tick (rcx = world, dl = flag). Unique in .text.
inline constexpr uint8_t kWorldTickPat[] = {0x40, 0x55, 0x53, 0x56, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0xF8, 0xFF};

// Prologue of the admin-command consumer system (rcx = job). Unique in .text.
inline constexpr uint8_t kAdminConsumerPat[] = {0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x41, 0x54, 0x48, 0x8D, 0xAC,
                                                0x24, 0x18, 0xE9, 0xFF, 0xFF, 0xB8, 0xE8, 0x17, 0x00, 0x00};

// Prologue of the game options' per-frame update (the GameApplicationOptions step, on the main thread; rcx = the game
// application). It reads the options at app + 0x128 first. Unique in .text.
inline constexpr uint8_t kOptionsUpdatePat[] = {0x48, 0x83, 0xEC, 0x38, 0x48, 0x83, 0xB9, 0x28, 0x01,
                                                0x00, 0x00, 0x00, 0x48, 0x89, 0x5C, 0x24, 0x48, 0x48,
                                                0x89, 0x7C, 0x24, 0x28, 0x48, 0x8B, 0xF9};

// Prologue of the game's camera settings push (rcx = the options object, rdx = a user slot): applying the settings
// menu runs it to copy field of view and the other camera settings into the camera. Unique in .text.
inline constexpr uint8_t kCamPushPat[] = {0x48, 0x83, 0xEC, 0x58, 0x80, 0xB9, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x0F,
                                          0x84, 0xE6, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x42, 0x58, 0x33, 0xC0};

// Prologue of the game's program runner (rcx = world, rdx = job, r8 = index, r9 = slot): every ECS system runs through
// it, on whichever thread the game's job system picks, by calling the update at *(job + 0x10) + 0x28. It has an unused
// twin, so the patterns are long enough to tell them apart. kRunnerPat is its prologue up to build 1792507660,
// kRunnerPat2 since 1793791052; each is unique in its builds.
inline constexpr uint8_t kRunnerPat[] = {0x40, 0x55, 0x53, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x8D, 0xAC,
                                         0x24, 0x70, 0xF8, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x90, 0x08, 0x00,
                                         0x00, 0x48, 0x8B, 0x5A, 0x10, 0x45, 0x33, 0xD2, 0x44, 0x8B};
inline constexpr uint8_t kRunnerPat2[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C,
                                          0x24, 0x18, 0x4C, 0x89, 0x64, 0x24, 0x20, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48,
                                          0x8D, 0xAC, 0x24, 0x70, 0xF8, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x90};

// The game's build: the exe's link time stamp. These builds were tested; on any other build every feature still checks
// itself.
inline constexpr uint32_t kTestedBuilds[] = {1792148057, 1792507660, 1793791052};
uint32_t gameBuild(uintptr_t imageBase);
inline bool testedBuild(uint32_t build) {
    for (uint32_t b : kTestedBuilds)
        if (b == build) return true;
    return false;
}

// Address of the only match in the image's .text, or 0 if there are none or several.
uintptr_t findUnique(uintptr_t imageBase, const uint8_t* pat, size_t len);
inline uintptr_t findWorldTick(uintptr_t imageBase) { return findUnique(imageBase, kWorldTickPat, sizeof kWorldTickPat); }
inline uintptr_t findAdminConsumer(uintptr_t imageBase) {
    return findUnique(imageBase, kAdminConsumerPat, sizeof kAdminConsumerPat);
}
inline uintptr_t findOptionsUpdate(uintptr_t imageBase) {
    return findUnique(imageBase, kOptionsUpdatePat, sizeof kOptionsUpdatePat);
}
inline uintptr_t findCamPush(uintptr_t imageBase) { return findUnique(imageBase, kCamPushPat, sizeof kCamPushPat); }
inline uintptr_t findRunner(uintptr_t imageBase) {
    uintptr_t fn = findUnique(imageBase, kRunnerPat, sizeof kRunnerPat);
    return fn ? fn : findUnique(imageBase, kRunnerPat2, sizeof kRunnerPat2);
}

// The admin consumer's query slot that holds the ecs::GameSettings resource.
inline constexpr uint32_t kSettingsSlot = 13;

// Where the system whose descriptor holds fn keeps the resource it writes called name: its index among the system's
// resources (the ones it reads, then the ones it writes) and their total. -1 if the descriptor isn't found exactly once
// or doesn't write that resource.
int writtenResource(uintptr_t imageBase, uintptr_t fn, std::string_view name, uint32_t& total);

// The function of the ECS system called name ("inventory_actions"), found through its descriptor. 0 if the name or
// the descriptor isn't there exactly once.
uintptr_t systemFn(uintptr_t imageBase, std::string_view name);

// All of the game's ECS systems by name, each of which can be switched off: the
// program runner hook then skips it in every world. Off lasts until it's switched on again or the game closes.
namespace sys {

struct System {
    std::string_view name;
    uintptr_t fn;
};

size_t scan(uintptr_t imageBase);  // once, before the runner is hooked: every descriptor; returns how many
const std::vector<System>& all();  // sorted by name
const System* find(std::string_view name);
bool isOn(const System& s);
void setOn(const System& s, bool on);  // any thread
size_t offCount();
bool skip(const void* job);  // the runner hook, any thread: the job's system is off. One atomic load while none is off

// The profiler: the runner hook times each call while it's on. Times are summed over every thread that ran the system,
// so they are CPU time, not frame time.
void setProfiling(bool on);  // on starts over from zero
bool profiling();
void record(const void* job, uint8_t worldKind, int64_t qpcTicks);  // the runner hook, any thread
struct Stat {
    const System* sys;
    uint64_t calls;
    double ms;
    uint8_t kinds;  // bit k: ran in a world of kind k (0 server, 1 proxy, 2 client)
};
std::vector<Stat> stats();  // since profiling started, the systems that ran, most ms first
double profiledSecs();
// Stat.kinds as "server", "client" or "both"; kind 1 counts as server.
inline const char* worldName(uint8_t kinds) {
    return !(kinds & 4) ? "server" : (kinds & 3) ? "both" : "client";
}

}  // namespace sys

// Adds flag to the command line the game reads at startup: the exe's
// GetCommandLineA/W imports are pointed at copies with it added. Once, before the game's code runs (DllMain). False
// unless both imports were found.
bool addLaunchFlag(uintptr_t imageBase, const char* flag);

// A row of enshrouded.log with nothing in it for a modder: an empty message, or a trace of the game's state machines
// ("''::Enter #0", "triggered ''"). The Game Log window leaves these out.
bool logNoise(std::string_view row);

// The game's options: one object, made at startup and kept at app + kAppOptions.
// The machine settings (local_settings.json) sit at kMachine, the user settings (Steam cloud user_settings.json) in 16
// slots from slotsAt: {u32 user id, the settings at +8, a has-value byte at hasAt}. The game reads them every frame,
// and its settings menu applies a change by copying it in, so writes here apply the same way. The game never saves
// them: its menu keeps its own copy, and applying the menu sets that copy again.
namespace opts {

inline constexpr uintptr_t kAppOptions = 0x128, kMachine = 0x9c0;
// The user slots move when the settings grow (0xa68/0x308/0x300 up to build 1792507660, 0xa70/0x310/0x308 since), so
// findLayout reads them from the game's user-slot getter: the first slot, the slot size, the has-value byte. 0 until then.
inline uintptr_t slotsAt, slotSize, hasAt;
bool findLayout(uintptr_t imageBase);

const char* attach(uintptr_t options);  // nullptr once the machine settings check out, else why not
bool ready();
uint8_t* machine();  // null until ready
uint8_t* user();     // the signed-in user's settings; null until ready, or while no user's settings are loaded
refl::Type machineType();
refl::Type userType();
// From the options update hook, every frame: attaches until the options check out (returns why not), then keeps
// antialiasing at None while setAaOff is on, and puts the game's choice back when it goes off.
const char* frame(uintptr_t app);
void setAaOff(bool off);
// The game's camera settings push (findCamPush), or 0. With it, frame() hands a changed field of view to the camera.
void setCamPush(uintptr_t fn);
// Hands fn "machine.graphics.vsync = false" lines for every scalar whose path starts with prefix; returns how many.
// A value that differs from the game's default ends in " (default true)". Strings and arrays are left out, so the
// hosting passwords and banned accounts are never in it.
size_t dump(std::string_view prefix, const std::function<void(const std::string&)>& fn);

}  // namespace opts

namespace ecs {

uint32_t mix(uint32_t key);                                     // the game's entity-map key hash
int compIndex(uintptr_t world, uint32_t typeHash);              // -1 if the world has no such component type
uint8_t* component(uintptr_t world, uint32_t entity, int idx);  // null if the entity is missing or lacks it
uint32_t firstWith(uintptr_t world, int idx);                   // first entity with the component, 0 if none
std::vector<uint32_t> allWith(uintptr_t world, const int* idx, size_t n);  // entities with all n components
uint32_t entityCount(uintptr_t world);
// The inspector's walk: the entity map's value (record) for an id, 0 if missing; the used slots in [from, to) as
// fn(id, record), returning the map's capacity (0 if it doesn't look like one); a record's component indices and
// where one sits; and per component index its type hash (0 if none), from the schema's component map.
uintptr_t record(uintptr_t world, uint32_t entity);
uint64_t forSlots(uintptr_t world, uint64_t from, uint64_t to, const std::function<void(uint32_t, uintptr_t)>& fn);
std::vector<int> recordComps(uintptr_t rec);
uint8_t* inRecord(uintptr_t rec, int idx);
std::vector<uint32_t> compHashes(uintptr_t world);

// Events. Streams hold this tick's events from the start of the world tick until
// the session clears them after it, so forEachEvent belongs right after the tick.
// findEvents reads where the schema keeps its event map and the world its streams from the game's code: both move
// between builds. Until it succeeds, every world has no event types.
bool findEvents(uintptr_t imageBase);
int eventIndex(uintptr_t world, uint32_t typeHash);  // -1 if the world has no such event type
void forEachEvent(uintptr_t world, int idx, const std::function<void(const uint8_t* ev)>& fn);
// Zeroed payload to fill, distributed at the start of the next world tick. Null if the world has no stream for the
// type, the size doesn't match it, or the buffer is full.
uint8_t* postEvent(uintptr_t world, uint32_t typeHash, uint32_t size);
int64_t simTime(uintptr_t world);  // GameEvent.timeStamp for "now"

// A system's resource pointer in query slot `slot`, as the game's query fill lays the query out for that system's
// job, plus the size of the slot's resource group and its index there (to check against the descriptor). False if the
// slot holds no resource.
struct ResSlot {
    uintptr_t ptr;
    uint64_t group, index;
};
bool querySlot(const void* job, uint32_t slot, ResSlot& out);

}  // namespace ecs

// Game resources. The index comes from enshrouded.kfc; the data is the block the
// game loads every resource into at startup and keeps until it exits. Resources are Keen blobs: variable-size values
// sit after the struct, reached through offsets relative to the field, all inside the resource.
inline constexpr uintptr_t kResourceBase = 0x20000000000;  // the GF_Resource block in enshrouded.log, same in every log

namespace res {

struct Entry {
    uint8_t guid[16];
    uint32_t type;       // qualified hash of the resource's reflected type
    uint32_t part;       // 0, or the part of a resource split into several
    uint32_t off, size;  // in the data block
};

// A blob value's data: where it starts, relative to the value (0 if empty), and its element count (string length,
// variant size in bytes, 0 or 1 for an optional). hash is the variant's type. Callers check the data is inside().
struct Blob {
    uint32_t rel, count, hash;
};

bool load(const uint8_t* kfc, size_t n);  // the index from enshrouded.kfc; false if it isn't a KFC4 index
bool attach(uintptr_t data);              // checks the loaded data at data; true makes ready()
bool ready();
// Copies n bytes into resource memory, around the read-only protection the game puts on it after loading. prot gets
// the protection the pages had. False if it couldn't be lifted.
bool write(void* dst, const void* src, size_t n, uint32_t& prot);
size_t count();
uint32_t protection();  // page protection of the data, as VirtualQuery reports it
uint8_t* data(const Entry& e);
const Entry* find(const uint8_t guid[16], uint32_t type, uint32_t part = 0);  // type 0: any type
const std::vector<const Entry*>& ofType(uint32_t type);
std::vector<std::pair<refl::Type, size_t>> types();  // the index's types and their counts, by qualified name
refl::Type typeByName(std::string_view name);         // "ItemInfo" or "keen::ItemInfo", among the index's types
std::string_view label(const Entry& e);               // its debugName or name, "" if it has neither
Blob blob(const uint8_t* p, uint8_t prim);

// An enum field of a resource type, at the top level or one struct down ("equipment.slot"): the game's own
// classification, which the Resources window groups by.
struct EnumField {
    std::string path;
    refl::Type type;  // the enum
    uint32_t offset;
};
std::vector<EnumField> enumFields(refl::Type t);
bool enumValue(const Entry& e, const EnumField& f, uint64_t& v);  // the raw value; false if the field isn't inside e
bool inside(const Entry& e, const uint8_t* p, uint64_t n);
bool parseGuid(std::string_view s, uint8_t out[16]);  // xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
std::string guidStr(const uint8_t g[16]);

// English text. The game's En_Us table sits in enshrouded_<container>.dat, which
// main.cpp reads; the index's content map and the LocaTagCollectionResource say where.
bool textSource(uint32_t& container, uint32_t& offset, uint32_t& size);  // false if it can't be found; needs ready()
bool setText(std::vector<uint8_t> table);                                // false if it isn't a sorted text table
size_t textCount();
bool isText(refl::Type t);                       // a LocaTag reference, like ItemInfo.name
std::string_view text(const uint8_t guid[16]);  // the text a LocaTag reference names, "" if none
std::string_view nameText(const Entry& e);      // the text of e's name field when that is a LocaTag reference

}  // namespace res
