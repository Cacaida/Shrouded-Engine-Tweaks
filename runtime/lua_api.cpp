#include "lua_api.h"

#include "contracts.h"
#include "game.h"
#include "hooks.h"
#include "level.h"
#include "modsettings.h"
#include "version.h"
#include "writelog.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <iterator>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Lua is compiled as C++ (runtime/CMakeLists.txt), so its headers are included without extern "C".
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

namespace {

std::string gModsDir;
rt::Sink gSink = [](const char*) {};
uintptr_t gWorld;  // server world, only set while Lua runs
lua_State* gL;
std::atomic<bool> gReload, gLoaded;  // gLoaded: a Lua state with the mods exists (the toolbar's Reload Mods)
std::mutex gQueueMx;
std::vector<std::string> gQueue;
std::atomic<bool> gOverlayOpen, gHasDraw;
std::atomic<uint32_t> gBuild;  // the game build (rt::setGameBuild), for GetGameBuild() and mod.json

// Lua runs on the server thread (tick) or the main thread (frame), and on the render thread (Draw). gLuaMx keeps them
// apart; gInDraw marks the render-thread side, where game memory is off limits because the game mutates it
// concurrently.
std::mutex gLuaMx;
bool gInDraw;
bool gInConsole, gHookShutdown;
// Hooks.observe and Hooks.rule handles. A rule has rule >= 0, no Lua function (sub -1, ref LUA_NOREF), and the names
// of its params in index order.
struct HookHandler {
    int sub, ref;
    std::string mod, name;
    std::vector<hk::Arg> args;
    int rule = -1;
    std::vector<std::string> params;
    bool hasList = false;
};
std::map<lua_Integer, HookHandler> gHooks;
lua_Integer gNextHook = 0;
std::map<std::string, uint32_t> gModOrder;  // the mods' load order (loadMods): rules on one target run by it
std::atomic<bool> gBroken;  // an access violation stopped Lua work: the state is unusable, so no Lua runs again
// The mod whose code runs right now (call() sets it from the function it calls), "" for the console and anything else
// outside a mod. Its writes to resources, options and systems go into the write log (wlog). Under gLuaMx.
std::string gMod;

constexpr const char* kRefMt = "rt.ref";
constexpr const char* kTicks = "rt.ticks";
constexpr const char* kDraws = "rt.draws";
constexpr const char* kKeys = "rt.keys";
constexpr const char* kObservers = "rt.observers";  // event type hash -> list of handlers
constexpr const char* kMods = "rt.mods";            // mod name -> what its init.lua returned (GetMod)
constexpr const char* kBinds = "rt.binds";          // "<mod>.<id>" -> Keys.hotkey or Keys.input callback
constexpr const char* kRequire = "rt.require";      // makes a mod's require (the kTextOnly prelude returns it)
constexpr const char* kIo = "rt.io";                // the real io library, only for the mods' wrappers
constexpr const char* kOnInit = "rt.onInit";        // registerForEvent lists; onUpdate and onDraw use kTicks and kDraws
constexpr const char* kOnOpen = "rt.onOverlayOpen";
constexpr const char* kOnClose = "rt.onOverlayClose";
constexpr const char* kOnShutdown = "rt.onShutdown";
constexpr const char* kOnEnter = "rt.onWorldEnter";
constexpr const char* kOnLeave = "rt.onWorldLeave";
constexpr const char* kFnMod = "rt.fnmod";          // registered function -> the mod running when it was registered
constexpr const char* kOnChange = "rt.onSettingsChange";  // mod -> its Settings.define onChange
uintptr_t gReady;  // server world whose pre-tick Lua work ran this tick; afterTick only dispatches for it
// The read point's check: gReadGen is odd while rt::tick or rt::afterTick may read gReadWorld. A system job of that
// world that saw it odd, or saw it change while it ran, overlapped Lua (rt::jobEnd counts it).
std::atomic<uint32_t> gReadGen;
std::atomic<uintptr_t> gReadWorld;
std::atomic<uint64_t> gOverlaps;
struct ReadWindow {
    explicit ReadWindow(uintptr_t w) {
        gReadWorld = w;
        ++gReadGen;
    }
    ~ReadWindow() { ++gReadGen; }
};
uint64_t gSession;  // counts sessions (the new-session test in worldReady), for the inspector
bool gNewWorld;  // a session started in another world (not a respawn): onWorldEnter at the next ready tick
bool gLeaving;     // onWorldLeave is running: the world is going away
bool gSeenOpen;    // the overlay state the current Lua state last saw, for onOverlayOpen and onOverlayClose
double gDt;        // seconds since the previous tick (or frame, outside a world), for Tick.add and onUpdate
// In a world you host, from its first ready tick until the leave: the server tick runs the mods. Otherwise the main
// thread does (rt::frame).
std::atomic<bool> gInWorld;
std::atomic<bool> gClosing;  // the game is closing (rt::gameClosing): no game memory, and no mods after onShutdown

// A struct or array seen from Lua. Component refs hold no pointer: the address is re-resolved on every access, so an
// entity that moved chunk or lost the component can't leave a dangling pointer. Event refs point at the event in its
// stream and are only valid during the dispatch that created them (gen). Resource refs point at the resource, which
// the game keeps from startup until it exits (gen 0, needAlive); so do game option refs. Owned refs (Types.new,
// Types.copy) point at bytes stored right after the Ref in its own userdata.
struct Ref {
    uint32_t entity;
    int comp;             // component index in the world
    uint32_t off;         // byte offset of this value inside the component, event or resource
    uint32_t limit;       // component, event or resource size, for bounds checks
    const uint8_t* type;  // descriptor of this value
    uint8_t* raw;         // event data, a post scratch buffer, a resource or game options instead of a component
    uint32_t gen;         // raw refs: valid while gGen still equals this; 0 for resources and options, always valid
    bool writable;        // raw refs: the scratch buffers of Events.post and Admin.send, resources and options
    bool plain;           // game options and owned values: ordinary memory, written directly (resources use res::write)
    bool owned;           // Lua owns the bytes: views into them keep the owner alive (refIndex)
};

uint32_t gGen = 1;  // bumped after every event dispatch

std::string qname(refl::Type t) { return std::string(t.qname()); }

// The game frees its options and resources as it exits, so onShutdown then gets files only.
void needAlive(lua_State* L) {
    if (gClosing) luaL_error(L, "the game is closing: onShutdown can't reach it");
}

void needGame(lua_State* L) {
    if (gInDraw) luaL_error(L, "the game is not available in Draw callbacks: read it in Tick and keep it in a variable");
    needAlive(L);
    if (gLeaving) luaL_error(L, "the world is closing: onWorldLeave can't reach the game");
}

// Commands and events go to the world you host, so there has to be one.
void needWorld(lua_State* L) {
    if (!gInWorld) luaL_error(L, "not in a world you host");
}

// A registration remembers the mod running when it was made, so a function built with load() still belongs to its mod.
// ponytail: one mod per function; a function two mods register stays the first one's.
void tagFn(lua_State* L, int idx) {
    if (gMod.empty()) return;
    idx = lua_absindex(L, idx);
    lua_getfield(L, LUA_REGISTRYINDEX, kFnMod);
    lua_pushvalue(L, idx);
    if (lua_rawget(L, -2) == LUA_TNIL) {
        lua_pushvalue(L, idx);
        lua_pushstring(L, gMod.c_str());
        lua_rawset(L, -4);
    }
    lua_pop(L, 2);
}

std::string modOf(lua_State* L, int idx, bool* isMod = nullptr);

// Address of [off, off + size) inside r's value, or a Lua error.
uint8_t* addr(lua_State* L, const Ref& r, uint32_t off, uint32_t size, bool write = false) {
    if (uint64_t(off) + size > r.limit) luaL_error(L, "offset %d is outside the value", int(off));
    if (r.raw) {  // not a component: scratch, an event (stale after its dispatch: gGen), a resource, options or owned
        if (!r.owned) needAlive(L);
        if (r.gen && r.gen != gGen) luaL_error(L, "event no longer valid: use it inside its handler only");
        if (write && !r.writable) luaL_error(L, "events are read-only");
        return r.raw + off;
    }
    needGame(L);
    uint8_t* b = gWorld ? ecs::component(gWorld, r.entity, r.comp) : nullptr;
    if (!b) luaL_error(L, "entity %d no longer has this component", int(r.entity));
    return b + off;
}

bool isInt(uint8_t k) { return (k >= refl::U8 && k <= refl::S64) || (k >= refl::Enum && k <= refl::Bitmask64); }
bool isSigned(uint8_t k) { return k == refl::S8 || k == refl::S16 || k == refl::S32 || k == refl::S64; }

lua_Integer readInt(const uint8_t* p, uint32_t size, bool sign) {
    uint64_t v = 0;
    memcpy(&v, p, size);
    if (sign && size < 8 && (v >> (size * 8 - 1) & 1)) v |= ~0ull << (size * 8);  // sign-extend
    return static_cast<lua_Integer>(v);
}

bool fits(lua_Integer v, uint32_t size, bool sign) {
    if (size >= 8) return true;
    int bits = int(size * 8);
    return sign ? v >= -(1ll << (bits - 1)) && v < (1ll << (bits - 1)) : v >= 0 && v < (1ll << bits);
}

using refl::enumHas;

// Single-field wrapper structs like EntityId {id} or Time {value} read and write as their scalar, so e.deadEntityId
// is a number.
using refl::unwrap;

// The user value anchors a view into an owned value to that value's userdata.
void newRef(lua_State* L, const Ref& r) {
    *static_cast<Ref*>(lua_newuserdatauv(L, sizeof(Ref), 1)) = r;
    luaL_setmetatable(L, kRefMt);
}

// A value Lua owns, of type t: its bytes follow the Ref in the userdata, which Lua never moves. Not initialized.
uint8_t* newOwned(lua_State* L, refl::Type t) {
    auto r = static_cast<Ref*>(lua_newuserdatauv(L, sizeof(Ref) + t.size(), 1));
    auto bytes = reinterpret_cast<uint8_t*>(r + 1);
    *r = {0, -1, 0, t.size(), t.p, bytes, 0, true, true, true};
    luaL_setmetatable(L, kRefMt);
    return bytes;
}

void push(lua_State* L, const Ref& r, uint32_t off, refl::Type t);

// Offset of a blob value's data inside r, checked against r's size.
uint32_t blobAt(lua_State* L, const Ref& r, uint32_t off, const res::Blob& b) {
    uint64_t at = uint64_t(off) + b.rel;
    if (at > r.limit) luaL_error(L, "blob data at offset %d + %d is outside the value", int(off), int(b.rel));
    return static_cast<uint32_t>(at);
}

// Blobs (resources only): strings as Lua strings, arrays as proxies, optionals and variants as their value or nil.
void pushBlob(lua_State* L, const Ref& r, uint32_t off, refl::Type u) {
    uint8_t k = u.prim();
    res::Blob b = res::blob(addr(L, r, off, u.size()), k);
    if (k == refl::BlobArray) {  // the proxy reads the header again on every access
        Ref child = r;
        child.off = off;
        child.type = u.p;
        newRef(L, child);
    } else if (k == refl::BlobString) {
        uint32_t at = b.count ? blobAt(L, r, off, b) : off;
        lua_pushlstring(L, reinterpret_cast<const char*>(addr(L, r, at, b.count)), b.count);
    } else if (!b.count) {
        lua_pushnil(L);
    } else {
        refl::Type v = k == refl::BlobVariant ? refl::byHash(b.hash) : u.inner();
        if (!v) luaL_error(L, "%s holds an unknown type (hash 0x%x)", qname(u).c_str(), b.hash);
        push(L, r, blobAt(L, r, off, b), v);
    }
}

void pushGuid(lua_State* L, const uint8_t* p) {
    static constexpr uint8_t kNone[16] = {};
    if (memcmp(p, kNone, 16) == 0) lua_pushnil(L);
    else lua_pushstring(L, res::guidStr(p).c_str());
}

// Pushes the value of type t at offset off of r: scalars as Lua values, references as GUID strings, structs and
// arrays as proxies.
void push(lua_State* L, const Ref& r, uint32_t off, refl::Type t) {
    refl::Type u = unwrap(t.resolved());
    uint8_t k = u.prim();
    if (k >= refl::BlobArray && k <= refl::BlobVariant) return pushBlob(L, r, off, u);
    const uint8_t* p = addr(L, r, off, u.size());
    if (k == refl::Bool) lua_pushboolean(L, *p != 0);
    else if (k == refl::F32) lua_pushnumber(L, *reinterpret_cast<const float*>(p));
    else if (k == refl::F64) lua_pushnumber(L, *reinterpret_cast<const double*>(p));
    else if (isInt(k) && u.size() <= 8) lua_pushinteger(L, readInt(p, u.size(), isSigned(k)));
    else if ((k == refl::ObjectReference || k == refl::Guid) && u.size() == 16) pushGuid(L, p);
    else if (k == refl::Struct || k == refl::StaticArray) {
        Ref child = r;
        child.off = off;
        child.type = u.p;
        newRef(L, child);
    } else {
        // Ds types (the options' strings and lists, the hosting passwords among them): the type name, never the data
        lua_pushfstring(L, "<%s>", qname(u).c_str());
    }
}

// Copies n bytes into r at off. Resources go through res::write, because the game keeps their pages read-only.
// A mod's writes to resources and options go into the write log first; a full log refuses the write.
void put(lua_State* L, const Ref& r, uint32_t off, const void* src, uint32_t n) {
    uint8_t* p = addr(L, r, off, n, true);
    bool logged = !gMod.empty() && r.raw && !r.gen && !r.owned;
    if (logged && !wlog::reserve(r.plain ? wlog::Opt : wlog::Res, r.raw, p, n, gMod))
        luaL_error(L, "write log full: this change couldn't be made undoable");
    if (!r.raw || r.gen || r.plain) {
        memcpy(p, src, n);
    } else {
        static bool told;
        uint32_t prot = 0;
        if (!res::write(p, src, n, prot)) luaL_error(L, "resource memory couldn't be made writable");
        if (!(prot & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !std::exchange(told, true)) {
            char line[96];
            snprintf(line, sizeof line, LV_OK "Resources: The game keeps them read-only (protect 0x%x), each edit lifts that", prot);
            gSink(line);
        }
    }
    if (logged) wlog::wrote(p, src, n);
}

lua_Integer enumArg(lua_State* L, int arg, refl::Type t);
void fill(lua_State* L, const Ref& r, uint32_t off, refl::Type t, int idx, uint8_t* mask = nullptr);

// Copies the view at stack index idx over the value of type u at off of r. Only between values of one type, and only
// fixed-size data (refl::plain): blob and Ds headers point elsewhere.
void copyValue(lua_State* L, const Ref& r, uint32_t off, refl::Type u, int idx) {
    const Ref& s = *static_cast<const Ref*>(lua_touserdata(L, idx));
    if (s.type != u.p) luaL_error(L, "can't assign a %s to a %s", qname(refl::Type{s.type}).c_str(), qname(u).c_str());
    if (!refl::plain(u)) luaL_error(L, "can't copy %s: it holds strings or arrays of variable size", qname(u).c_str());
    std::vector<uint8_t> tmp(u.size());  // the two may overlap
    memcpy(tmp.data(), addr(L, s, s.off, u.size()), u.size());
    put(L, r, off, tmp.data(), u.size());
}

// Writes the Lua value at stack index idx into the value of type t at offset off. Type- and range-checked. Enums also
// take their value names; structs take a table of fields, and structs and arrays a view of the same type.
void store(lua_State* L, const Ref& r, uint32_t off, refl::Type t, int idx) {
    refl::Type u = unwrap(t.resolved());
    uint8_t k = u.prim();
    if (k == refl::Bool) {
        luaL_checktype(L, idx, LUA_TBOOLEAN);
        uint8_t b = lua_toboolean(L, idx) ? 1 : 0;
        put(L, r, off, &b, 1);
    } else if (k == refl::F32) {
        auto v = static_cast<float>(luaL_checknumber(L, idx));
        put(L, r, off, &v, 4);
    } else if (k == refl::F64) {
        double v = luaL_checknumber(L, idx);
        put(L, r, off, &v, 8);
    } else if (isInt(k) && u.size() <= 8) {
        bool named = k == refl::Enum && lua_type(L, idx) == LUA_TSTRING;
        lua_Integer v = named ? enumArg(L, idx, u) : luaL_checkinteger(L, idx);
        if (!fits(v, u.size(), isSigned(k))) luaL_error(L, "%I doesn't fit %s", v, qname(u).c_str());
        if (k == refl::Enum && !enumHas(u, v)) luaL_error(L, "%I is not a value of %s", v, qname(u).c_str());
        put(L, r, off, &v, u.size());
    } else if ((k == refl::ObjectReference || k == refl::Guid) && u.size() == 16) {
        uint8_t g[16] = {};  // nil clears the reference
        size_t n = 0;
        const char* s = lua_isnil(L, idx) ? nullptr : luaL_checklstring(L, idx, &n);
        if (s && !res::parseGuid({s, n}, g)) luaL_error(L, "'%s' is not a GUID (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)", s);
        put(L, r, off, g, 16);
    } else if (k >= refl::BlobArray && k <= refl::BlobVariant) {
        luaL_error(L, "can't assign to %s: its size is fixed; set its elements instead", qname(u).c_str());
    } else if (k == refl::Struct && lua_istable(L, idx)) {
        fill(L, r, off, u, idx);
    } else if ((k == refl::Struct || k == refl::StaticArray) && luaL_testudata(L, idx, kRefMt)) {
        copyValue(L, r, off, u, idx);
    } else {
        luaL_error(L, "can't assign to %s; set its fields instead", qname(u).c_str());
    }
}

// Copies the Lua table at stack index idx into the struct of type t at offset off of r, field by field. A nested
// table fills a nested struct: {transform = {position = {x = 1}}}. A mask, if given, gets 1 for every byte written.
void fill(lua_State* L, const Ref& r, uint32_t off, refl::Type t, int idx, uint8_t* mask) {
    idx = lua_absindex(L, idx);
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "?";
        refl::Field f;
        if (!refl::field(t, key, f)) luaL_error(L, "%s has no field '%s'", qname(t).c_str(), key);
        refl::Type ft = f.type.resolved();
        if (lua_istable(L, -1) && ft.prim() == refl::Struct) {
            fill(L, r, off + f.offset, ft, -1, mask);
        } else {
            store(L, r, off + f.offset, f.type, lua_gettop(L));
            if (mask) memset(mask + off + f.offset, 1, ft.size());
        }
        lua_pop(L, 1);
    }
}

uint32_t arrayLen(refl::Type a) {
    uint32_t e = a.inner() ? a.inner().size() : 0;
    return e ? a.size() / e : 0;
}

bool isArray(refl::Type t) { return t.prim() == refl::StaticArray || t.prim() == refl::BlobArray; }

// Where r's array elements start and how many there are. A blob array's elements are checked to lie inside r.
std::pair<uint32_t, uint32_t> elements(lua_State* L, const Ref& r) {
    refl::Type t{r.type};
    if (t.prim() == refl::StaticArray) return {r.off, arrayLen(t)};
    res::Blob b = res::blob(addr(L, r, r.off, 8), refl::BlobArray);
    uint32_t at = b.count ? blobAt(L, r, r.off, b) : r.off;
    if (at + uint64_t(b.count) * t.inner().size() > r.limit) luaL_error(L, "array elements are outside the value");
    return {at, b.count};
}

// Offset and type of the key at stack index 2 inside r: an array index or a struct field name.
std::pair<uint32_t, refl::Type> member(lua_State* L, const Ref& r) {
    refl::Type t{r.type};
    if (isArray(t)) {
        lua_Integer i = luaL_checkinteger(L, 2);
        auto [start, n] = elements(L, r);
        luaL_argcheck(L, i >= 0 && i < lua_Integer(n), 2, "index out of range");
        return {start + uint32_t(i) * t.inner().size(), t.inner()};
    }
    const char* key = luaL_checkstring(L, 2);
    refl::Field f;
    if (!refl::field(t, key, f)) luaL_error(L, "%s has no field '%s'", qname(t).c_str(), key);
    return {r.off + f.offset, f.type};
}

const Ref& checkRef(lua_State* L) { return *static_cast<Ref*>(luaL_checkudata(L, 1, kRefMt)); }

// A view into an owned value keeps the owner alive: its user value is the owner (1 itself, or 1's own anchor).
int refIndex(lua_State* L) {
    Ref r = checkRef(L);
    auto [off, t] = member(L, r);
    push(L, r, off, t);
    if (r.owned && luaL_testudata(L, -1, kRefMt)) {
        if (lua_getiuservalue(L, 1, 1) == LUA_TNIL) {
            lua_pop(L, 1);
            lua_pushvalue(L, 1);
        }
        lua_setiuservalue(L, -2, 1);
    }
    return 1;
}

int refNewIndex(lua_State* L) {
    Ref r = checkRef(L);
    auto [off, t] = member(L, r);
    store(L, r, off, t, 3);
    return 0;
}

int refLen(lua_State* L) {
    const Ref& r = checkRef(L);
    if (!isArray(refl::Type{r.type})) return luaL_error(L, "# only works on arrays");
    lua_pushinteger(L, elements(L, r).second);
    return 1;
}

// Arrays print their elements, e.g. {60, 0, 100, ...}, the first 50 of them; structs print their type name.
int refToString(lua_State* L) {
    Ref r = checkRef(L);
    refl::Type t{r.type};
    if (!isArray(t)) {
        lua_pushfstring(L, "<%s>", qname(t).c_str());
        return 1;
    }
    auto [start, n] = elements(L, r);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    luaL_addchar(&b, '{');
    for (uint32_t i = 0; i < n && i < 50; i++) {
        if (i) luaL_addstring(&b, ", ");
        push(L, r, start + i * t.inner().size(), t.inner());
        luaL_tolstring(L, -1, nullptr);
        lua_remove(L, -2);
        luaL_addvalue(&b);
    }
    luaL_addstring(&b, n > 50 ? ", ...}" : "}");
    luaL_pushresult(&b);
    return 1;
}

int ecsGet(lua_State* L) {
    needGame(L);
    auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    refl::Type t = refl::byName(name);
    if (!t || t.prim() != refl::Struct) return luaL_error(L, "unknown component type '%s'", name);
    int idx = gWorld ? ecs::compIndex(gWorld, t.hash()) : -1;
    if (idx < 0 || !ecs::component(gWorld, id, idx)) {
        lua_pushnil(L);
        return 1;
    }
    newRef(L, {id, idx, 0, t.size(), t.p, nullptr, 0, false});
    return 1;
}

// Limit: walks the whole entity map per call; mods should keep the id instead of calling this every tick.
int ecsPlayer(lua_State* L) {
    needGame(L);
    refl::Type pi = refl::byName("PlayerInput");
    int idx = gWorld && pi ? ecs::compIndex(gWorld, pi.hash()) : -1;
    uint32_t id = ecs::firstWith(gWorld, idx);
    if (id) lua_pushinteger(L, id);
    else lua_pushnil(L);
    return 1;
}

// The iterator Ecs.each returns. Upvalues: the ids, the position, then per component its descriptor and index.
int eachNext(lua_State* L) {
    lua_Integer pos = lua_tointeger(L, lua_upvalueindex(2)) + 1;
    lua_pushinteger(L, pos);
    lua_replace(L, lua_upvalueindex(2));
    if (lua_rawgeti(L, lua_upvalueindex(1), pos) == LUA_TNIL) return 1;  // nil ends the loop
    auto id = static_cast<uint32_t>(lua_tointeger(L, -1));
    int n = 0;
    for (int u = 3; lua_type(L, lua_upvalueindex(u)) != LUA_TNONE; u += 2, n++) {
        refl::Type t{static_cast<const uint8_t*>(lua_touserdata(L, lua_upvalueindex(u)))};
        newRef(L, {id, int(lua_tointeger(L, lua_upvalueindex(u + 1))), 0, t.size(), t.p, nullptr, 0, false});
    }
    return 1 + n;
}

// Ecs.each(name, ...): for id, comp, ... in Ecs.each("Health", "Faction"), over the entities that have all of them.
// Limit: one walk of the whole entity map per call; a cached archetype walk if mods need it every tick.
int ecsEach(lua_State* L) {
    needGame(L);
    int n = lua_gettop(L);
    luaL_argcheck(L, n >= 1 && n <= 8, 1, "give 1 to 8 component names");
    std::vector<int> idx;
    for (int i = 1; i <= n; i++) {
        const char* name = luaL_checkstring(L, i);
        refl::Type t = refl::byName(name);
        if (!t || t.prim() != refl::Struct) return luaL_error(L, "unknown component type '%s'", name);
        idx.push_back(gWorld ? ecs::compIndex(gWorld, t.hash()) : -1);
        lua_pushlightuserdata(L, const_cast<uint8_t*>(t.p));
        lua_replace(L, i);
    }
    std::vector<uint32_t> ids = gWorld ? ecs::allWith(gWorld, idx.data(), idx.size()) : std::vector<uint32_t>();
    lua_createtable(L, int(ids.size()), 0);
    for (size_t i = 0; i < ids.size(); i++) {
        lua_pushinteger(L, ids[i]);
        lua_rawseti(L, -2, lua_Integer(i) + 1);
    }
    lua_pushinteger(L, 0);
    for (int i = 1; i <= n; i++) {
        lua_pushvalue(L, i);
        lua_pushinteger(L, idx[i - 1]);
    }
    lua_pushcclosure(L, eachNext, 2 + 2 * n);
    return 1;
}

int ecsFields(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    refl::Type t = refl::lookup(name);
    if (!t) return luaL_error(L, "unknown type '%s'", name);
    refl::forFields(t, [](const refl::Field& f) {
        char line[512];
        snprintf(line, sizeof line, "  +0x%-4x %.*s: %.*s", f.offset, int(f.name.size()), f.name.data(),
                 int(f.type.qname().size()), f.type.qname().data());
        gSink(line);
    });
    return 0;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(tolower(static_cast<unsigned char>(c)));
    return out;
}

// Prints reflected type names containing the text (case-insensitive), at most 100.
int ecsTypes(lua_State* L) {
    std::string sub = lower(luaL_checkstring(L, 1));
    size_t found = 0;
    for (size_t i = 0; i < refl::count(); i++) {
        std::string q = qname(refl::at(i));
        if (lower(q).find(sub) != std::string::npos && ++found <= 100) gSink(q.c_str());
    }
    if (found > 100) gSink(("... and " + std::to_string(found - 100) + " more").c_str());
    return 0;
}

// ---- Res: the game's resources (game.h, namespace res). Static memory, so Draw callbacks may read them too ----

void needRes(lua_State* L) {
    needAlive(L);
    if (!res::ready()) luaL_error(L, "resources are not available (yet): see set.log");
}

refl::Type resType(lua_State* L, int arg) {
    const char* name = luaL_checkstring(L, arg);
    refl::Type t = res::typeByName(name);
    if (!t) luaL_error(L, "unknown resource type '%s' (Res.types lists them)", name);
    return t;
}

// Writable: an edit lasts until the game closes.
void pushRes(lua_State* L, const res::Entry& e) {
    newRef(L, {0, -1, 0, e.size, refl::byHash(e.type).p, res::data(e), 0, true});
}

// Res.get(guid [, type [, part]]): without a type, the first resource with that GUID.
int resGet(lua_State* L) {
    needRes(L);
    size_t n;
    const char* s = luaL_checklstring(L, 1, &n);
    uint8_t g[16];
    if (!res::parseGuid({s, n}, g)) return luaL_argerror(L, 1, "not a GUID (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)");
    uint32_t type = lua_isnoneornil(L, 2) ? 0 : resType(L, 2).hash();
    const res::Entry* e = res::find(g, type, static_cast<uint32_t>(luaL_optinteger(L, 3, 0)));
    if (e && refl::byHash(e->type)) pushRes(L, *e);
    else lua_pushnil(L);
    return 1;
}

// The iterator Res.each returns. Upvalues: the type hash and the position in res::ofType.
int resEachNext(lua_State* L) {
    const auto& list = res::ofType(static_cast<uint32_t>(lua_tointeger(L, lua_upvalueindex(1))));
    lua_Integer pos = lua_tointeger(L, lua_upvalueindex(2));
    if (pos >= lua_Integer(list.size())) return 0;
    lua_pushinteger(L, pos + 1);
    lua_replace(L, lua_upvalueindex(2));
    const res::Entry& e = *list[size_t(pos)];
    lua_pushstring(L, res::guidStr(e.guid).c_str());
    pushRes(L, e);
    lua_pushinteger(L, e.part);
    return 3;
}

// Res.each(type): for guid, res, part in Res.each("ItemInfo") do ... end
int resEach(lua_State* L) {
    needRes(L);
    lua_pushinteger(L, resType(L, 1).hash());
    lua_pushinteger(L, 0);
    lua_pushcclosure(L, resEachNext, 2);
    return 1;
}

// The first resource of the type whose debugName, name or English name is want (lowercase), or null.
const res::Entry* findNamed(uint32_t type, const std::string& want) {
    for (const res::Entry* e : res::ofType(type))
        if (lower(res::label(*e)) == want || lower(res::nameText(*e)) == want) return e;
    return nullptr;
}

// Res.text(guid): the English text a LocaTag reference (item.name, item.description, ...) names; nil if none.
int resText(lua_State* L) {
    needRes(L);
    size_t n;
    const char* s = luaL_checklstring(L, 1, &n);
    uint8_t g[16];
    if (!res::parseGuid({s, n}, g)) return luaL_argerror(L, 1, "not a GUID (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)");
    std::string_view t = res::text(g);
    if (t.empty()) lua_pushnil(L);
    else lua_pushlstring(L, t.data(), t.size());
    return 1;
}

// Res.find(type, name): guid and resource of the first whose debugName, name or English name matches, ignoring case;
// nil if none.
int resFind(lua_State* L) {
    needRes(L);
    const res::Entry* e = findNamed(resType(L, 1).hash(), lower(luaL_checkstring(L, 2)));
    if (!e) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, res::guidStr(e->guid).c_str());
    pushRes(L, *e);
    return 2;
}

// Res.types([text]): prints the resource types whose name contains the text (case-insensitive), with their counts.
int resTypes(lua_State* L) {
    needRes(L);
    std::string sub = lower(luaL_optstring(L, 1, ""));
    for (const auto& [t, n] : res::types()) {
        std::string q = qname(t);
        if (lower(q).find(sub) != std::string::npos) gSink((q + "  " + std::to_string(n)).c_str());
    }
    return 0;
}

// ---- GameOptions: the game's settings, live (game.h, namespace opts). They last as long as the game, so Draw
// callbacks may use them too ----

int pushOptions(lua_State* L, uint8_t* p, refl::Type t, const char* what) {
    needAlive(L);
    if (!p || !t) return luaL_error(L, "%s not found yet: see set.log", what);
    newRef(L, {0, -1, 0, t.size(), t.p, p, 0, true, true});
    return 1;
}

int optsMachine(lua_State* L) { return pushOptions(L, opts::machine(), opts::machineType(), "game options"); }
int optsUser(lua_State* L) { return pushOptions(L, opts::user(), opts::userType(), "user settings"); }

// GameOptions.dump([prefix]): prints "machine.graphics.vsync = false" lines, those whose path starts with the prefix;
// returns how many. Hosting passwords and banned accounts are never printed.
int optsDump(lua_State* L) {
    std::string_view prefix = luaL_optstring(L, 1, "");
    needAlive(L);
    if (!opts::ready()) return luaL_error(L, "game options not found yet: see set.log");
    lua_pushinteger(L, lua_Integer(opts::dump(prefix, [](const std::string& s) { gSink(s.c_str()); })));
    return 1;
}

// ---- Systems: the game's ECS systems, switched off by name (game.h, namespace sys). Flags only, no game memory, so
// Draw callbacks may use them too ----

std::atomic<bool> gSysOk;  // the program runner is hooked, after sys::scan: the overlay reads sys:: only once it is

// Systems.list([search]): the names holding search (case-insensitive), sorted.
int systemsList(lua_State* L) {
    std::string sub = lower(luaL_optstring(L, 1, ""));
    lua_newtable(L);
    lua_Integer i = 0;
    for (const sys::System& s : sys::all()) {
        if (s.name.find(sub) == std::string_view::npos) continue;
        lua_pushlstring(L, s.name.data(), s.name.size());
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

const sys::System& systemArg(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (!gSysOk) luaL_error(L, "Systems is unavailable in this game build: see set.log");
    const sys::System* s = sys::find(name);
    if (!s) luaL_error(L, "unknown system '%s' (Systems.list searches)", name);
    return *s;
}

// Systems.enable(name, on): false makes the game skip that system in every world until it's enabled again.
int systemsEnable(lua_State* L) {
    const sys::System& s = systemArg(L);
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    uint8_t on = lua_toboolean(L, 2) ? 1 : 0;
    auto p = reinterpret_cast<const uint8_t*>(&s);
    bool logged = !gMod.empty();
    if (logged && !wlog::reserve(wlog::Sys, &s, p, 1, gMod))
        return luaL_error(L, "write log full: this change couldn't be made undoable");
    sys::setOn(s, on);
    if (logged) wlog::wrote(p, &on, 1);
    return 0;
}

int systemsEnabled(lua_State* L) {
    lua_pushboolean(L, sys::isOn(systemArg(L)));
    return 1;
}

// Systems.profile(on): true starts the profiler over from zero, false stops it.
int systemsProfile(lua_State* L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    if (!gSysOk) return luaL_error(L, "Systems is unavailable in this game build: see set.log");
    sys::setProfiling(lua_toboolean(L, 1));
    return 0;
}

// Systems.stats(): {{name, ms, calls, world}, ...} since the profiler started, most ms first, and the seconds measured.
int systemsStats(lua_State* L) {
    lua_newtable(L);
    lua_Integer i = 0;
    for (const sys::Stat& s : sys::stats()) {
        lua_createtable(L, 0, 4);
        lua_pushlstring(L, s.sys->name.data(), s.sys->name.size());
        lua_setfield(L, -2, "name");
        lua_pushnumber(L, s.ms);
        lua_setfield(L, -2, "ms");
        lua_pushinteger(L, lua_Integer(s.calls));
        lua_setfield(L, -2, "calls");
        lua_pushstring(L, sys::worldName(s.kinds));
        lua_setfield(L, -2, "world");
        lua_rawseti(L, -2, ++i);
    }
    lua_pushnumber(L, sys::profiledSecs());
    return 2;
}

// ---- Types: the reflected types, and values Lua owns. No game memory, so Draw callbacks may use them too ----

refl::Type typeArg(lua_State* L, int arg) {
    const char* name = luaL_checkstring(L, arg);
    refl::Type t = refl::lookup(name).resolved();
    if (!t) luaL_error(L, "unknown type '%s' (Ecs.types searches; a name several types share needs the full one)", name);
    return t;
}

// Types.fields(name): {{name = "maxStackSize", type = "keen::uint16", offset = 20}, ...}, base classes first.
int typesFields(lua_State* L) {
    refl::Type t = typeArg(L, 1);
    lua_newtable(L);
    lua_Integer i = 0;
    refl::forFields(t, [&](const refl::Field& f) {
        lua_createtable(L, 0, 3);
        lua_pushlstring(L, f.name.data(), f.name.size());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, qname(f.type).c_str());
        lua_setfield(L, -2, "type");
        lua_pushinteger(L, f.offset);
        lua_setfield(L, -2, "offset");
        lua_rawseti(L, -2, ++i);
    });
    return 1;
}

// Types.enum(name): {None = 0, Fxaa = 1, ...}
int typesEnum(lua_State* L) {
    refl::Type t = typeArg(L, 1);
    if (t.prim() != refl::Enum) return luaL_error(L, "%s is not an enum", qname(t).c_str());
    lua_newtable(L);
    refl::forEnum(t, [&](std::string_view name, int64_t v) {
        lua_pushinteger(L, v);
        lua_setfield(L, -2, std::string(name).c_str());
    });
    return 1;
}

void needPlain(lua_State* L, refl::Type t) {
    if (t.prim() != refl::Struct && t.prim() != refl::StaticArray)
        luaL_error(L, "%s is not a struct or array", qname(t).c_str());
    if (!refl::plain(t))
        luaL_error(L, "%s holds strings or arrays of variable size: only fixed-size values can be made", qname(t).c_str());
}

// Types.new(name [, values]): a struct or array Lua owns, holding the game's default value (zeros if the type has
// none), then the fields in values.
int typesNew(lua_State* L) {
    refl::Type t = typeArg(L, 1);
    needPlain(L, t);
    if (!lua_isnoneornil(L, 2)) luaL_checktype(L, 2, LUA_TTABLE);
    uint8_t* b = newOwned(L, t);
    if (const uint8_t* d = t.defaults()) memcpy(b, d, t.size());
    else memset(b, 0, t.size());
    if (lua_istable(L, 2)) fill(L, *static_cast<const Ref*>(lua_touserdata(L, -1)), 0, t, 2);
    return 1;
}

// Types.copy(view): a copy Lua owns of a struct or array, e.g. to put back later.
int typesCopy(lua_State* L) {
    const Ref& s = checkRef(L);
    refl::Type t{s.type};
    needPlain(L, t);
    const uint8_t* src = addr(L, s, s.off, t.size());
    memcpy(newOwned(L, t), src, t.size());
    return 1;
}

// Appends fn (argument 1) to a callback list in the registry: Tick.add and Draw.add.
void addCallback(lua_State* L, const char* reg) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    tagFn(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, reg);
    lua_pushvalue(L, 1);
    lua_rawseti(L, -2, lua_Integer(lua_rawlen(L, -2)) + 1);
}

int tickAdd(lua_State* L) {
    addCallback(L, kTicks);
    return 0;
}

int drawAdd(lua_State* L) {
    addCallback(L, kDraws);
    gHasDraw = true;
    return 0;
}

// The lifecycle event names and the callback lists behind them.
constexpr std::pair<const char*, const char*> kLifecycle[] = {
    {"onInit", kOnInit},         {"onUpdate", kTicks},          {"onDraw", kDraws},
    {"onOverlayOpen", kOnOpen}, {"onOverlayClose", kOnClose}, {"onShutdown", kOnShutdown},
    {"onWorldEnter", kOnEnter}, {"onWorldLeave", kOnLeave}};

// registerForEvent(name, fn): onUpdate is Tick.add and onDraw is Draw.add.
int registerForEvent(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    auto ev = std::find_if(std::begin(kLifecycle), std::end(kLifecycle), [&](const auto& e) { return strcmp(e.first, name) == 0; });
    if (ev == std::end(kLifecycle))
        return luaL_argerror(L, 1, "unknown event (onInit, onUpdate, onDraw, onOverlayOpen, onOverlayClose, onShutdown, "
                                   "onWorldEnter, onWorldLeave)");
    lua_remove(L, 1);
    addCallback(L, ev->second);
    if (ev->second == kDraws) gHasDraw = true;
    return 0;
}

int overlayIsOpen(lua_State* L) {
    lua_pushboolean(L, gOverlayOpen);
    return 1;
}

int keysOn(lua_State* L) {
    int vk = rt::keyFromName(luaL_checkstring(L, 1));
    luaL_argcheck(L, vk != 0, 1, "unknown key (use F1-F24, A-Z, 0-9, `, Insert, Home, End, Delete, PageUp, PageDown, Pause or 0xNN)");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    tagFn(L, 2);
    lua_getfield(L, LUA_REGISTRYINDEX, kKeys);
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, vk);
    return 0;
}

// A hotkey or input a mod registered. Its key combination comes from bindings.ini, set in the overlay's Bindings
// window, which reads and edits these from the render thread: gBindMx guards gBinds and gSaved.
struct Binding {
    std::string key, mod, label;  // key = "<mod>.<id>"
    bool input;
    int mods, vk;  // rt::KeyMod bits and the key; vk 0 = unbound
    bool held;     // the combination was held at the last poll
};
std::mutex gBindMx;
std::vector<Binding> gBinds;                // registered by the current Lua state
std::map<std::string, std::string> gSaved;  // bindings.ini: key -> combination text

constexpr const char* kModNames[] = {"Ctrl", "Shift", "Alt"};  // rt::ModCtrl, ModShift, ModAlt

Binding* findBind(const std::string& key) {
    for (Binding& b : gBinds)
        if (b.key == key) return &b;
    return nullptr;
}

std::string comboName(int mods, int vk) {
    std::string s;
    for (int i = 0; i < 3; i++)
        if (mods & (1 << i)) (s += kModNames[i]) += '+';
    return s + rt::keyName(vk);
}

// "Ctrl+Shift+F6" -> modifiers and key; false if any part is unknown.
bool parseCombo(const std::string& s, int& mods, int& vk) {
    mods = vk = 0;
    size_t i = 0;
    for (size_t plus; (plus = s.find('+', i)) != std::string::npos; i = plus + 1) {
        int bit = 0;
        for (int m = 0; m < 3; m++)
            if (_stricmp(s.substr(i, plus - i).c_str(), kModNames[m]) == 0) bit = 1 << m;
        if (!bit) return false;
        mods |= bit;
    }
    vk = rt::keyFromName(s.substr(i).c_str());
    return vk != 0;
}

// shrouded_engine_tweaks\bindings.ini, next to the mods folder. Empty without a mods folder.
std::string bindsPath() {
    size_t slash = gModsDir.find_last_of('\\');
    return slash == std::string::npos ? "" : gModsDir.substr(0, slash) + "\\bindings.ini";
}

// For every new Lua state, so reload() also picks up edits made by hand.
void loadBindings() {
    std::lock_guard lock(gBindMx);
    gBinds.clear();
    gSaved.clear();
    std::string path = bindsPath();
    if (path.empty()) return;
    std::vector<char> keys(32 * 1024);
    DWORD n = GetPrivateProfileStringA("bindings", nullptr, "", keys.data(), static_cast<DWORD>(keys.size()), path.c_str());
    for (const char* k = keys.data(); k < keys.data() + n && *k; k += strlen(k) + 1) {
        char combo[64];
        GetPrivateProfileStringA("bindings", k, "", combo, sizeof combo, path.c_str());
        gSaved[k] = combo;
    }
}

// Keys.hotkey(id, label, fn) and Keys.input(id, label, fn), one closure per mod: upvalue 1 is the mod's name, 2 whether
// it registers an input. Registering an id again replaces it.
int keysBind(lua_State* L) {
    std::string mod = lua_tostring(L, lua_upvalueindex(1)), id = luaL_checkstring(L, 1);
    bool input = lua_toboolean(L, lua_upvalueindex(2));
    const char* label = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    tagFn(L, 3);
    bool okId = !id.empty() && std::all_of(id.begin(), id.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    });
    luaL_argcheck(L, okId, 1, "use letters, digits and _ only");
    std::string key = mod + "." + id;
    lua_getfield(L, LUA_REGISTRYINDEX, kBinds);
    lua_pushvalue(L, 3);
    lua_setfield(L, -2, key.c_str());
    std::lock_guard lock(gBindMx);
    Binding* b = findBind(key);
    if (!b) b = &gBinds.emplace_back(Binding{key, mod, "", false, 0, 0, false});
    b->label = label;
    b->input = input;
    auto saved = gSaved.find(key);
    if (saved == gSaved.end() || !parseCombo(saved->second, b->mods, b->vk)) b->mods = b->vk = 0;
    return 0;
}

int luaPrint(lua_State* L) {
    int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++) {
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_tolstring(L, i, nullptr);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    gSink(lua_tostring(L, -1));
    return 0;
}

int luaReload(lua_State*) {
    gReload = true;  // applied after this tick's Lua work, outside the running state
    return 0;
}

int traceback(lua_State* L) {
    luaL_traceback(L, L, lua_tostring(L, 1), 1);
    return 1;
}

// pcall with a traceback; errors go to the sink. The function and nargs arguments must be on the stack.
bool call(lua_State* L, int nargs, int nres) {
    int base = lua_gettop(L) - nargs;
    bool isMod = false;
    std::string mod = lua_isfunction(L, base) ? modOf(L, base, &isMod) : "";
    std::string prior = std::exchange(gMod, isMod ? mod : "");
    lua_pushcfunction(L, traceback);
    lua_insert(L, base);
    int rc = lua_pcall(L, nargs, nres, base);
    lua_remove(L, base);
    gMod = prior;
    if (rc == LUA_OK) return true;
    gSink((LV_BAD + std::string(lua_tostring(L, -1))).c_str());
    lua_pop(L, 1);
    return false;
}

void clearFields(lua_State* L, const char* table, std::initializer_list<const char*> names) {
    lua_getglobal(L, table);
    for (const char* n : names) {
        lua_pushnil(L);
        lua_setfield(L, -2, n);
    }
    lua_pop(L, 1);
}

// Pushes the chunk of a mod's Lua file, or an error message: text only, named chunk ("@hello/init.lua") so error lines
// start with the mod's own path. A UTF-8 BOM and a first line starting with # are skipped, as luaL_loadfilex does.
bool loadText(lua_State* L, const std::string& path, const char* chunk) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    LARGE_INTEGER size{};
    std::string text;
    DWORD got = 0;
    bool ok = f != INVALID_HANDLE_VALUE && GetFileSizeEx(f, &size) && size.QuadPart < (64 << 20);
    if (ok) {
        text.resize(static_cast<size_t>(size.QuadPart));
        ok = ReadFile(f, text.data(), static_cast<DWORD>(text.size()), &got, nullptr) && got == text.size();
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) {
        lua_pushfstring(L, "cannot open %s", chunk + 1);
        return false;
    }
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
    if (text.starts_with('#')) text.erase(0, std::min(text.find('\n'), text.size()));  // keeps the line count
    return luaL_loadbufferx(L, text.data(), text.size(), chunk, "t") == LUA_OK;
}

// loadtext(path, chunk, env) for the mods' require (kTextOnly): loadText, returned the way loadfile returns.
int luaLoadText(lua_State* L) {
    if (!loadText(L, luaL_checkstring(L, 1), luaL_checkstring(L, 2))) {
        luaL_pushfail(L);
        lua_insert(L, -2);
        return 2;
    }
    if (!lua_isnoneornil(L, 3)) {
        lua_pushvalue(L, 3);
        lua_setupvalue(L, -2, 1);  // a main chunk's only upvalue is _ENV
    }
    return 1;
}

// Crafted bytecode can corrupt memory and escape the sandbox, so load and require accept source text only. The global
// require searches a copy of package.path: a mod that changes package.path can't point it at other files.
// Returns the factory for a mod's own require: names resolve inside its folder (dots become \, so ".." can't
// survive) and run in its environment, cached per mod. Its chunks are named like "@hello/util.lua" (luaLoadText).
const char* kTextOnly = R"lua(
    local loadtext = ...
    local load, loadfile, searchpath, path, error, type, pcall =
        load, loadfile, package.searchpath, package.path, error, type, pcall
    _G.load = function(chunk, name, _, ...) return load(chunk, name, "t", ...) end
    package.searchers[2] = function(mod)
        local file, err = searchpath(mod, path)
        if not file then return err end
        local fn, msg = loadfile(file, "t")
        if not fn then error(("error loading module '%s' from file '%s':\n\t%s"):format(mod, file, msg)) end
        return fn, file
    end
    return function(env, dir, mod)
        local cache, loading = {}, {}  -- loading: modules whose chunk is running, so a cycle fails instead of recursing
        return function(name)
            if type(name) ~= "string" then error("bad argument #1 to 'require' (string expected)", 2) end
            if cache[name] == nil then
                if loading[name] then
                    error("require cycle: '" .. name .. "' is still loading (it requires itself through other modules)", 2)
                end
                local path, chunk = dir .. "\\" .. name:gsub("%.", "\\"), "@" .. mod .. "/" .. name:gsub("%.", "/")
                local fn, err = loadtext(path .. ".lua", chunk .. ".lua", env)
                if not fn and err:find("^cannot open") then
                    fn, err = loadtext(path .. "\\init.lua", chunk .. "/init.lua", env)
                end
                if not fn and err:find("^cannot open") then err = "module '" .. name .. "' not found in " .. dir end
                if not fn then error(err, 2) end
                loading[name] = true
                local ok, r = pcall(fn, name)
                loading[name] = nil  -- a module that failed can be required again
                if not ok then error(r, 0) end
                if r == nil then r = true end
                cache[name] = r
            end
            return cache[name]
        end
    end
)lua";

// Mods are third-party code, so the state gets no file, process or native-code access:
// no debug, dofile/loadfile, os beyond time functions, and io only inside a mod's own folder (pushModEnv). The global
// require only finds Lua files in the mods folder; mods get their own (kTextOnly).
void openSafeLibs(lua_State* L) {
    const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base},        {LUA_LOADLIBNAME, luaopen_package},
                             {LUA_COLIBNAME, luaopen_coroutine}, {LUA_TABLIBNAME, luaopen_table},
                             {LUA_STRLIBNAME, luaopen_string},   {LUA_MATHLIBNAME, luaopen_math},
                             {LUA_UTF8LIBNAME, luaopen_utf8},    {LUA_OSLIBNAME, luaopen_os}};
    for (const luaL_Reg& l : libs) {
        luaL_requiref(L, l.name, l.func, 1);
        lua_pop(L, 1);
    }
    clearFields(L, "os", {"execute", "remove", "rename", "tmpname", "exit", "getenv", "setlocale"});
    clearFields(L, "package", {"loadlib"});
    lua_getglobal(L, "package");
    lua_pushstring(L, gModsDir.empty() ? "" : (gModsDir + "\\?.lua;" + gModsDir + "\\?\\init.lua").c_str());
    lua_setfield(L, -2, "path");
    lua_pushstring(L, "");
    lua_setfield(L, -2, "cpath");
    lua_getfield(L, -1, "searchers");  // keep 1 (preload) and 2 (Lua files); drop the two C-library searchers
    for (int i = 4; i >= 3; i--) {
        lua_pushnil(L);
        lua_rawseti(L, -2, i);
    }
    lua_pop(L, 2);
    luaL_loadstring(L, kTextOnly);  // after package.path is set: it keeps a copy
    lua_pushcfunction(L, luaLoadText);
    lua_call(L, 1, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, kRequire);
    lua_pushcfunction(L, luaopen_io);  // called directly: luaL_requiref would put it in package.loaded for require("io")
    lua_call(L, 0, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, kIo);
    clearFields(L, "_G", {"dofile", "loadfile"});
    clearFields(L, "string", {"dump"});
}

// <mod folder>\<path> for a mod's io, or a Lua error if the path could leave the folder: no drive, stream or wildcard
// characters, no empty parts (absolute paths, \\?\) and no parts made only of dots and spaces ("..", ".. ").
std::string modPath(lua_State* L, const char* dir) {
    size_t n;
    const char* p = luaL_checklstring(L, 1, &n);
    std::string_view path(p, n);
    bool ok = !path.empty() && path.find_first_of(":*?\"<>|") == std::string_view::npos &&
              std::none_of(path.begin(), path.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; });
    for (size_t i = 0; ok && i <= path.size();) {
        size_t end = std::min(path.find_first_of("/\\", i), path.size());
        ok = path.substr(i, end - i).find_first_not_of(". ") != std::string_view::npos;
        i = end + 1;
    }
    if (!ok) luaL_argerror(L, 1, "the path must stay inside the mod's folder");
    return std::string(dir) + "\\" + std::string(path);
}

// True for Shrouded Engine Tweaks' own files in a mod's folder (settings.json and its .tmp): the mod's io reads them,
// but doesn't write them. Windows drops trailing dots and spaces, so "settings.json." is the same file.
// ponytail: an 8.3 short name (SETTIN~1.JSO) still gets through; it only matters to a mod set on breaking its own file.
bool setOwned(std::string_view path) {
    if (path.find_first_of("/\\") != std::string_view::npos) return false;  // in a subfolder
    while (!path.empty() && (path.back() == '.' || path.back() == ' ')) path.remove_suffix(1);
    std::string l = lower(path);
    return l == "settings.json" || l == "settings.json.tmp";
}

// A mod's io.open and io.lines: upvalue 1 is its folder, 2 the real function, called with the checked full path, 3
// true for io.open. Opening settings.json to write fails the way io.open fails: nil and a message.
int modIo(lua_State* L) {
    std::string path = modPath(L, lua_tostring(L, lua_upvalueindex(1)));
    const char* mode = lua_toboolean(L, lua_upvalueindex(3)) ? luaL_optstring(L, 2, "r") : "r";
    if (strpbrk(mode, "wa+") && setOwned(lua_tostring(L, 1))) {
        lua_pushnil(L);
        lua_pushstring(L, "settings.json belongs to Shrouded Engine Tweaks: use Settings.define to save settings");
        return 2;
    }
    lua_pushstring(L, path.c_str());
    lua_replace(L, 1);
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

constexpr const char* kSettingTypes[] = {"bool", "number", "int", "choice", "text", nullptr};  // mset::Type order
constexpr const char* kSettingWant[] = {"a boolean", "a finite number", "an integer", "one of its values", "a string"};

// A setting's value at idx, checked against f's type, into num and text (a choice gives its index). False if it has
// the wrong type; the range is mset::assign's.
bool settingFrom(lua_State* L, int idx, const mset::Field& f, double& num, std::string& text) {
    int t = lua_type(L, idx);
    if (f.type == mset::Bool) {
        num = lua_toboolean(L, idx);
        return t == LUA_TBOOLEAN;
    }
    if (f.type == mset::Number || f.type == mset::Int) {
        int isInt = 1;
        num = f.type == mset::Int ? static_cast<double>(lua_tointegerx(L, idx, &isInt)) : lua_tonumber(L, idx);
        return t == LUA_TNUMBER && isInt && std::isfinite(num);
    }
    if (t != LUA_TSTRING) return false;
    size_t n;
    const char* p = lua_tolstring(L, idx, &n);
    text.assign(p, n);
    if (f.type == mset::Text) return true;
    auto it = std::find(f.values.begin(), f.values.end(), text);
    num = static_cast<double>(it - f.values.begin());
    return it != f.values.end();
}

void pushSetting(lua_State* L, const mset::Field& f) {
    switch (f.type) {
    case mset::Bool: lua_pushboolean(L, f.num != 0); break;
    case mset::Number: lua_pushnumber(L, f.num); break;
    case mset::Int: lua_pushinteger(L, static_cast<lua_Integer>(f.num)); break;
    case mset::Choice: lua_pushstring(L, f.values[static_cast<size_t>(f.num)].c_str()); break;
    default: lua_pushlstring(L, f.text.data(), f.text.size());
    }
}

// Field name of the spec table at t: a number, or def if it's missing.
double specNum(lua_State* L, int t, const char* name, double def, const std::string& key) {
    int type = lua_getfield(L, t, name);
    double v = type == LUA_TNIL ? def : lua_tonumber(L, -1);
    if (type != LUA_TNIL && (type != LUA_TNUMBER || v != v))
        luaL_error(L, "Settings.define: '%s': %s must be a number", key.c_str(), name);
    lua_pop(L, 1);
    return v;
}

// The range or the values of the spec table at t, for its type.
void settingLimits(lua_State* L, int t, mset::Field& f) {
    if (f.type == mset::Number || f.type == mset::Int) {
        constexpr double kExact = 9007199254740992.0;  // 2^53: integers a double holds exactly
        f.lo = specNum(L, t, "min", f.type == mset::Int ? -kExact : -HUGE_VAL, f.key);
        f.hi = specNum(L, t, "max", f.type == mset::Int ? kExact : HUGE_VAL, f.key);
        if (f.type == mset::Int) f.lo = std::max(std::ceil(f.lo), -kExact), f.hi = std::min(std::floor(f.hi), kExact);
        if (f.lo > f.hi) luaL_error(L, "Settings.define: '%s': min is above max", f.key.c_str());
    } else if (f.type == mset::Text) {
        double max = specNum(L, t, "max", 256, f.key);
        if (max < 1) luaL_error(L, "Settings.define: '%s': max (the length) must be 1 or more", f.key.c_str());
        f.maxLen = static_cast<size_t>(std::min(max, 65536.0));
    } else if (f.type == mset::Choice) {
        lua_getfield(L, t, "values");
        for (lua_Integer i = 1; lua_istable(L, -1) && lua_rawgeti(L, -1, i) == LUA_TSTRING; i++, lua_pop(L, 1))
            f.values.push_back(lua_tostring(L, -1));
        if (f.values.empty()) luaL_error(L, "Settings.define: '%s': values must list the choices (strings)", f.key.c_str());
        lua_pop(L, 2);
    }
}

// One entry of Settings.define: {key =, type =, default =, label =, min =, max =, values =}. Without a default: false,
// 0 (within the range), the first choice, "".
mset::Field settingSpec(lua_State* L, int t) {
    mset::Field f;
    if (lua_getfield(L, t, "key") != LUA_TSTRING) luaL_error(L, "Settings.define: every entry needs a key (a string)");
    f.key = lua_tostring(L, -1);
    bool okKey = !f.key.empty() && std::all_of(f.key.begin(), f.key.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    });
    if (!okKey) luaL_error(L, "Settings.define: key '%s': use letters, digits and _ only", f.key.c_str());
    int type = -1;
    if (lua_getfield(L, t, "type") == LUA_TSTRING)
        for (int i = 0; kSettingTypes[i]; i++)
            if (strcmp(lua_tostring(L, -1), kSettingTypes[i]) == 0) type = i;
    if (type < 0) luaL_error(L, "Settings.define: '%s': type must be bool, number, int, choice or text", f.key.c_str());
    f.type = static_cast<mset::Type>(type);
    f.label = lua_getfield(L, t, "label") == LUA_TSTRING ? lua_tostring(L, -1) : f.key;
    lua_pop(L, 3);
    settingLimits(L, t, f);
    double num = 0;
    std::string text;
    if (lua_getfield(L, t, "default") != LUA_TNIL && !settingFrom(L, -1, f, num, text))
        luaL_error(L, "Settings.define: '%s': default must be %s", f.key.c_str(), kSettingWant[f.type]);
    lua_pop(L, 1);
    mset::assign(f, num, text);
    return f;
}

// mods\<mod>\settings.json into page, read with the mods' json.decode. A file that isn't a JSON object is renamed to
// settings.json.bad; a value of the wrong type keeps its default; keys the mod doesn't define are kept, as JSON text.
void readSettings(lua_State* L, mset::Page& page) {
    FILE* f = nullptr;
    if (fopen_s(&f, page.path.c_str(), "rb") != 0 || !f) return;
    std::string text;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);  // Notepad's UTF-8 BOM
    int top = lua_gettop(L);
    std::string why = "not a JSON object";
    size_t at = text.find_first_not_of(" \t\r\n");
    if (at != std::string::npos && text[at] == '{') {
        lua_getglobal(L, "json");
        lua_getfield(L, -1, "decode");
        lua_pushlstring(L, text.data(), text.size());
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) why = lua_tostring(L, -1);
        else why.clear();
    }
    if (!why.empty()) {
        lua_settop(L, top);
        MoveFileExA(page.path.c_str(), (page.path + ".bad").c_str(), MOVEFILE_REPLACE_EXISTING);
        gSink((LV_WARN "Settings: " + page.mod + "\\settings.json couldn't be read (" + why +
               "): Renamed to settings.json.bad, using the defaults").c_str());
        return;
    }
    for (lua_pushnil(L); lua_next(L, -2); lua_pop(L, 1)) {
        std::string key = lua_tostring(L, -2);  // an object's keys are strings
        auto it = std::find_if(page.fields.begin(), page.fields.end(), [&](const mset::Field& x) { return x.key == key; });
        if (it == page.fields.end()) {
            lua_getglobal(L, "json");
            lua_getfield(L, -1, "encode");
            lua_pushvalue(L, -3);
            if (lua_pcall(L, 1, 1, 0) == LUA_OK) page.unknown[key] = lua_tostring(L, -1);
            lua_pop(L, 2);
            continue;
        }
        double num = it->num;
        std::string s;
        if (!settingFrom(L, -1, *it, num, s) || !mset::assign(*it, num, s))
            gSink((LV_WARN "Settings: " + page.mod + ": '" + key + "' in settings.json isn't " +
                   kSettingWant[it->type] + ", using the default").c_str());
    }
    lua_settop(L, top);
}

int settingGet(lua_State* L) {
    mset::Field f;
    const char* key = luaL_checkstring(L, 2);
    if (!mset::get(lua_tostring(L, lua_upvalueindex(1)), key, f)) return luaL_error(L, "Settings: no setting '%s'", key);
    pushSetting(L, f);
    return 1;
}

// s.key = value: checked and clamped, saved within a second. No onChange: that's for edits in the overlay.
int settingSet(lua_State* L) {
    std::string mod = lua_tostring(L, lua_upvalueindex(1)), key = luaL_checkstring(L, 2), text;
    mset::Field f;
    if (!mset::get(mod, key, f)) return luaL_error(L, "Settings: no setting '%s'", key.c_str());
    double num = f.num;
    if (!settingFrom(L, 3, f, num, text) || !mset::set(mod, key, num, text, false))
        return luaL_error(L, "Settings: '%s' takes %s", key.c_str(), kSettingWant[f.type]);
    return 0;
}

// Settings.define{entries..., onChange = fn}, one closure per mod (upvalue 1: its name): the mod's typed settings,
// with the values saved in its settings.json, once per mod. Returns the table they're read and written through.
int settingsDefine(lua_State* L) {
    std::string mod = lua_tostring(L, lua_upvalueindex(1));
    luaL_checktype(L, 1, LUA_TTABLE);
    if (mset::has(mod)) return luaL_error(L, "Settings.define: once per mod");
    mset::Page page{mod, gModsDir + "\\" + mod + "\\settings.json"};
    for (lua_Integer i = 1; lua_rawgeti(L, 1, i) != LUA_TNIL; i++, lua_pop(L, 1)) {
        if (!lua_istable(L, -1)) luaL_error(L, "Settings.define: entry %d isn't a table", static_cast<int>(i));
        mset::Field f = settingSpec(L, lua_gettop(L));
        for (const mset::Field& g : page.fields)
            if (g.key == f.key) luaL_error(L, "Settings.define: '%s' is defined twice", f.key.c_str());
        page.fields.push_back(std::move(f));
    }
    lua_pop(L, 1);
    if (page.fields.empty()) return luaL_error(L, "Settings.define: no settings given");
    if (lua_getfield(L, 1, "onChange") != LUA_TNIL) {
        luaL_checktype(L, -1, LUA_TFUNCTION);
        tagFn(L, -1);
        lua_getfield(L, LUA_REGISTRYINDEX, kOnChange);
        lua_pushvalue(L, -2);
        lua_setfield(L, -2, mod.c_str());
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    readSettings(L, page);
    mset::define(std::move(page));
    lua_newtable(L);  // stays empty, so every read and write goes through the metatable
    lua_newtable(L);
    for (const auto& f : {luaL_Reg{"__index", settingGet}, luaL_Reg{"__newindex", settingSet}}) {
        lua_pushstring(L, mod.c_str());
        lua_pushcclosure(L, f.func, 1);
        lua_setfield(L, -2, f.name);
    }
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    return 1;
}

// onChange(key, value) for each setting edited in the overlay since the last tick, run as the mod that defined it.
void settingsEdits(lua_State* L) {
    auto edits = mset::takeEdits();
    if (edits.empty()) return;
    lua_getfield(L, LUA_REGISTRYINDEX, kOnChange);
    for (const auto& [mod, key] : edits) {
        mset::Field f;
        if (lua_getfield(L, -1, mod.c_str()) != LUA_TFUNCTION || !mset::get(mod, key, f)) {
            lua_pop(L, 1);
            continue;
        }
        lua_pushstring(L, key.c_str());
        pushSetting(L, f);
        call(L, 2, 0);
    }
    lua_pop(L, 1);
}

// One declared argument position: a kind name ("u32", "entity*", "cstr"...), or "<Type>*" for a copy of a plain
// reflected type.
hk::Arg hookArg(lua_State* L, int idx) {
    size_t len;
    const char* p = luaL_checklstring(L, idx, &len);
    std::string name(p, len);
    hk::Kind kind;
    if (hk::kindOf(name, kind)) return {kind, {}};
    if (!name.empty() && name.back() == '*') {
        name.pop_back();
        auto type = refl::lookup(name).resolved();
        if (type) { needPlain(L, type); return {hk::Snapshot, type}; }
    }
    luaL_error(L, "unknown hook argument type '%s'", name.c_str());
    return {};
}

std::vector<hk::Arg> hookArgs(lua_State* L) {
    lua_getfield(L, 1, "args");
    luaL_checktype(L, -1, LUA_TTABLE);
    size_t n = lua_rawlen(L, -1);
    if (n > hk::kMaxArgs) luaL_error(L, "hooks accept at most eight argument positions");
    std::vector<hk::Arg> args;
    for (size_t i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, lua_Integer(i));
        args.push_back(hookArg(L, -1));
        lua_pop(L, 1);
    }
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 || lua_tointeger(L, -2) > lua_Integer(n))
            luaL_error(L, "hook args must be a contiguous array");
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return args;
}

std::string hookText(lua_State* L, const char* field, size_t limit) {
    lua_getfield(L, 1, field);
    size_t len;
    const char* p = luaL_checklstring(L, -1, &len);
    if (!len || len > limit || memchr(p, 0, len)) luaL_error(L, "invalid hook %s", field);
    std::string text(p, len);
    lua_pop(L, 1);
    return text;
}

int hookRegister(lua_State* L) {
    if (gInDraw || gInConsole || gHookShutdown || gClosing || gBroken)
        return luaL_error(L, "Hooks.observe is unavailable in Draw, the console, or shutdown");
    luaL_checktype(L, 1, LUA_TTABLE);
    std::string mod = lua_tostring(L, lua_upvalueindex(1)), name = hookText(L, "name", 128);
    if (name.find_first_of("/\\") != std::string::npos) return luaL_error(L, "hook names cannot contain slashes");
    std::string pattern = hookText(L, "pattern", 4096), err;
    auto args = hookArgs(L);
    lua_getfield(L, 1, "queue");
    lua_Integer cap = lua_isnil(L, -1) ? 256 : luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    if (cap < 1 || cap > hk::kMaxQueue) return luaL_error(L, "hook queue must be between 1 and 4096");
    lua_getfield(L, 1, "fn");
    luaL_checktype(L, -1, LUA_TFUNCTION);
    tagFn(L, -1);
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t fn = hk::findPattern(base, pattern, err);
    if (!fn) return luaL_error(L, "%s", err.c_str());
    if (gNextHook == LUA_MAXINTEGER) return luaL_error(L, "hook handle limit reached; reload mods");
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_Integer handle = ++gNextHook;
    auto [it, inserted] = gHooks.emplace(handle, HookHandler{-1, ref, mod, name, std::move(args)});
    (void)inserted;
    int id = hk::subscribe(fn, mod + '/' + name, it->second.args, uint32_t(cap), err);
    if (id < 0) {
        *static_cast<bool*>(lua_touserdata(L, lua_upvalueindex(2))) = true;
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        gHooks.erase(it);
        return luaL_error(L, "%s", err.c_str());
    }
    it->second.sub = id;
    lua_pushinteger(L, handle);
    return 1;
}

// Hooks.observe{...} and Hooks.rule{...}: fn runs in a pcall so every refusal reaches the Hooks window's diagnostics.
// Upvalue 2 of fn is set when the hooks layer already recorded the refusal, so it isn't counted twice.
int registerGuarded(lua_State* L, lua_CFunction fn) {
    bool recorded = false;
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushlightuserdata(L, &recorded);
    lua_pushcclosure(L, fn, 2);
    lua_pushvalue(L, 1);
    if (lua_pcall(L, 1, 1, 0) == LUA_OK) return 1;
    const char* err = lua_tostring(L, -1);
    if (!recorded)
        hk::refuse(std::string(lua_tostring(L, lua_upvalueindex(1))) + ": " + (err ? err : "hook registration failed"));
    return lua_error(L);
}

int hookObserve(lua_State* L) { return registerGuarded(L, hookRegister); }

// Up to 64 entity ids from the table at idx.
std::vector<uint32_t> ruleIds(lua_State* L, int idx) {
    luaL_checktype(L, idx, LUA_TTABLE);
    std::vector<uint32_t> ids;
    lua_Integer n = lua_Integer(lua_rawlen(L, idx));
    if (n > rules::kMaxList) luaL_error(L, "a rule's id list holds at most 64 ids");
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, idx, i);
        lua_Integer v = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : -1;
        if (v < 0 || v > 0xffffffffLL) luaL_error(L, "rule ids are entity ids (whole numbers)");
        ids.push_back(uint32_t(v));
        lua_pop(L, 1);
    }
    return ids;
}

struct RuleParse {
    hk::RuleDecl d;
    std::vector<std::string> params;  // names, in index order
    bool hasList = false;
    const contracts::Entry* entry = nullptr;  // target = "<catalog name>"
};

// params = {name = number}: a "$name" value reads the param's live value (Hooks.param). Indexes follow the sorted
// names, so the same rule after a reload has the same shape and reuses its slot.
void ruleParams(lua_State* L, RuleParse& p) {
    int type = lua_getfield(L, 1, "params");
    if (type != LUA_TNIL && type != LUA_TTABLE) luaL_error(L, "rule params must be a table of names to numbers");
    std::map<std::string, double> named;
    if (type == LUA_TTABLE) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TNUMBER)
                luaL_error(L, "rule params must be a table of names to numbers");
            named[lua_tostring(L, -2)] = lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    if (named.size() > rules::kMaxParams) luaL_error(L, "a rule takes at most 8 params");
    for (const auto& [name, v] : named) {
        p.params.push_back(name);
        p.d.params.push_back(v);
    }
}

// A number, true/false, "player" (SET's player entity) or "$name" (a param).
rules::Operand ruleOperand(lua_State* L, int idx, const RuleParse& p) {
    rules::Operand o;
    if (lua_type(L, idx) == LUA_TNUMBER) { o.value = lua_tonumber(L, idx); return o; }
    if (lua_type(L, idx) == LUA_TBOOLEAN) { o.value = lua_toboolean(L, idx); return o; }
    const char* s = lua_type(L, idx) == LUA_TSTRING ? lua_tostring(L, idx) : "";
    if (!strcmp(s, "player")) { o.src = rules::Player; return o; }
    for (size_t i = 0; *s == '$' && i < p.params.size(); ++i)
        if (p.params[i] == s + 1) { o.src = rules::Param; o.param = uint8_t(i); return o; }
    luaL_error(L, *s == '$' ? "rule param '%s' isn't in params" : "rule values are numbers, booleans, \"player\" or \"$param\"", s + 1);
    return o;
}

// A field the catalog lists for argument a (1-based) of its entry.
rules::Loc catalogField(lua_State* L, const contracts::Entry& e, lua_Integer a, const char* name) {
    for (const auto& f : e.fields)
        if (f.arg == a - 1 && !strcmp(f.name, name)) return contracts::loc(f);
    luaL_error(L, "catalog target '%s' has no field '%s' on argument %d", e.name, name, int(a));
    return {};
}

// {arg = n [, field = "path"]} at t: the value it names. An entity* argument without a field is the id it points at;
// on a catalog target, a field is one the catalog lists.
rules::Loc ruleLoc(lua_State* L, int t, const RuleParse& p) {
    const auto& args = p.d.args;
    lua_getfield(L, t, "arg");
    int ok = 0;
    lua_Integer a = lua_tointegerx(L, -1, &ok);
    if (!ok || a < 1 || a > lua_Integer(args.size())) luaL_error(L, "rule entries need arg = a declared position (1..%d)", int(args.size()));
    lua_getfield(L, t, "field");
    if (p.entry && !lua_isnil(L, -1)) {
        rules::Loc l = catalogField(L, *p.entry, a, luaL_checkstring(L, -1));
        lua_pop(L, 2);
        return l;
    }
    const hk::Arg& arg = args[size_t(a - 1)];
    rules::Loc l;
    std::string err;
    bool found = lua_isnil(L, -1) && arg.kind != hk::EntityPtr
                     ? hk::argLoc(arg, uint8_t(a - 1), l)
                     : hk::fieldLoc(arg, uint8_t(a - 1), lua_isnil(L, -1) ? "" : luaL_checkstring(L, -1), l, err);
    if (!found) luaL_error(L, "%s", err.empty() ? ("argument " + std::to_string(a) + " is a pointer: name a field").c_str() : err.c_str());
    lua_pop(L, 2);
    return l;
}

// The one key of names set in the entry at t, its value pushed; its index in names.
int ruleKey(lua_State* L, int t, std::initializer_list<const char*> names, const char* what) {
    int found = -1, i = 0;
    for (const char* n : names) {
        if (lua_getfield(L, t, n) != LUA_TNIL) {
            if (found >= 0) luaL_error(L, "each %s entry takes exactly one operation", what);
            found = i;
        }
        lua_pop(L, 1);
        ++i;
    }
    if (found < 0) luaL_error(L, "a %s entry has no operation", what);
    lua_getfield(L, t, names.begin()[found]);
    return found;
}

// when or set: a list of entry tables. Returns the list's stack index, or 0 if it's absent.
int ruleList(lua_State* L, const char* field) {
    int type = lua_getfield(L, 1, field);
    if (type == LUA_TNIL) { lua_pop(L, 1); return 0; }
    if (type != LUA_TTABLE) luaL_error(L, "rule %s must be a list of tables", field);
    size_t n = 0;
    for (lua_pushnil(L); lua_next(L, -2); lua_pop(L, 1)) ++n;
    if (n != lua_rawlen(L, -1)) luaL_error(L, "rule %s is a list: %s = {{arg = 1, ...}}", field, field);
    return lua_gettop(L);
}

void ruleConds(lua_State* L, RuleParse& p) {
    int list = ruleList(L, "when");
    for (lua_Integer i = 1; list && lua_rawgeti(L, list, i) != LUA_TNIL; ++i) {
        int e = lua_gettop(L);
        luaL_checktype(L, e, LUA_TTABLE);
        if (p.d.shape.conds == rules::kMaxConds) luaL_error(L, "a rule takes at most 8 conditions");
        rules::Cond c;
        c.at = ruleLoc(L, e, p);
        c.cmp = rules::Cmp(ruleKey(L, e, {"eq", "ne", "lt", "le", "gt", "ge", "in"}, "when"));
        if (c.cmp == rules::In) {
            if (p.hasList) luaL_error(L, "a rule takes one in list");
            p.hasList = true;
            p.d.list = ruleIds(L, -1);
        } else {
            c.v = ruleOperand(L, -1, p);
        }
        p.d.shape.cond[p.d.shape.conds++] = c;
        lua_settop(L, e - 1);
    }
    if (list) lua_settop(L, list - 1);
}

// block = value: a matching call doesn't run and returns value (true on a void function).
void ruleBlock(lua_State* L, RuleParse& p) {
    if (lua_getfield(L, 1, "block") != LUA_TNIL) {
        p.d.shape.block = true;
        p.d.shape.ret = ruleOperand(L, -1, p);
    }
    lua_pop(L, 1);
}

void ruleChanges(lua_State* L, RuleParse& p) {
    int list = ruleList(L, "set");
    for (lua_Integer i = 1; list && lua_rawgeti(L, list, i) != LUA_TNIL; ++i) {
        int e = lua_gettop(L);
        luaL_checktype(L, e, LUA_TTABLE);
        if (p.d.shape.changes == rules::kMaxChanges) luaL_error(L, "a rule takes at most 8 changes");
        rules::Change c;
        c.at = ruleLoc(L, e, p);
        c.op = rules::Op(ruleKey(L, e, {"set", "add", "mul", "min", "max"}, "set"));
        c.v = ruleOperand(L, -1, p);
        p.d.shape.change[p.d.shape.changes++] = c;
        lua_settop(L, e - 1);
    }
    if (list) lua_settop(L, list - 1);
}

// after = {{<set|add|mul|min|max> = value}, ...}: changes to the return value, once the original returned.
void ruleAfter(lua_State* L, RuleParse& p) {
    auto& s = p.d.shape;
    int list = ruleList(L, "after");
    for (lua_Integer i = 1; list && lua_rawgeti(L, list, i) != LUA_TNIL; ++i) {
        int e = lua_gettop(L);
        luaL_checktype(L, e, LUA_TTABLE);
        if (s.afters == rules::kMaxChanges) luaL_error(L, "a rule takes at most 8 after changes");
        rules::Change c;
        c.at = s.retAt;
        c.op = rules::Op(ruleKey(L, e, {"set", "add", "mul", "min", "max"}, "after"));
        c.v = ruleOperand(L, -1, p);
        s.after[s.afters++] = c;
        lua_settop(L, e - 1);
    }
    if (list) lua_settop(L, list - 1);
    if (s.block && (s.changes || s.afters)) luaL_error(L, "a rule that blocks changes nothing: the original doesn't run");
    if (!s.changes && !s.block && !s.afters)
        luaL_error(L, "a rule needs set = {{arg = n, <set|add|mul|min|max> = value}, ...}, after = {...} or block = value");
}

// builds = {...}: the game builds the pattern and argument declarations were verified on. Required.
bool ruleBuildListed(lua_State* L) {
    if (lua_getfield(L, 1, "builds") != LUA_TTABLE || !lua_rawlen(L, -1))
        luaL_error(L, "a rule on a pattern needs builds = {...}: the game builds it was verified on");
    bool listed = false;
    for (lua_Integer i = 1; lua_rawgeti(L, -1, i) != LUA_TNIL; ++i, lua_pop(L, 1))
        listed |= lua_isinteger(L, -1) && lua_tointeger(L, -1) == lua_Integer(gBuild.load());
    lua_pop(L, 2);
    return listed;
}

// target = "<catalog name>": the catalog declares the arguments and the fields a rule may read or change.
void ruleTarget(lua_State* L, RuleParse& p, contracts::Status& st) {
    std::string target = hookText(L, "target", 128);
    for (const char* k : {"pattern", "args", "builds"}) {
        bool set = lua_getfield(L, 1, k) != LUA_TNIL;
        lua_pop(L, 1);
        if (set) luaL_error(L, "a rule on a catalog target takes no %s: the catalog has it", k);
    }
    if (!contracts::find(target, st) || !st.entry) luaL_error(L, "no catalog target '%s'", target.c_str());
    p.entry = st.entry;
    p.d.block = st.entry->caps & contracts::Block;
    p.d.after = st.entry->caps & contracts::After;
    p.d.shape.retAt = contracts::retLoc(*st.entry);
    p.d.args = contracts::args(*st.entry);
    for (const auto& f : st.entry->fields) (f.readOnly ? p.d.reads : p.d.fields).push_back(contracts::loc(f));
}

// The catalog entry's capabilities: changing an argument needs Args, changing a field Fields, blocking Block, changing
// the return value After.
void ruleCaps(lua_State* L, const RuleParse& p) {
    if (p.d.shape.block && !(p.entry->caps & contracts::Block))
        luaL_error(L, "catalog target '%s' doesn't let rules block it", p.entry->name);
    if (p.d.shape.afters && !(p.entry->caps & contracts::After))
        luaL_error(L, "catalog target '%s' doesn't let rules change its return value", p.entry->name);
    for (int i = 0; i < p.d.shape.changes; ++i) {
        bool field = p.d.shape.change[i].at.field;
        if (!(p.entry->caps & (field ? contracts::Fields : contracts::Args)))
            luaL_error(L, "catalog target '%s' doesn't let rules change %s", p.entry->name, field ? "fields" : "arguments");
    }
}

// Where the rule goes: the catalog target's function, or the pattern's on a listed build. 0 if the rule stays off on
// this game (a yellow line says why).
uintptr_t ruleFn(lua_State* L, const RuleParse& p, const contracts::Status& st, const std::string& mod,
                 const std::string& name) {
    std::string off, err;
    if (p.entry) {
        ruleCaps(L, p);
        if (st.fn) return st.fn;
        off = "Catalog target '" + st.name + "' is off (" + st.why + ")";
    } else if (ruleBuildListed(L)) {
        uintptr_t fn = hk::findPattern(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), hookText(L, "pattern", 4096), err);
        if (!fn) luaL_error(L, "%s", err.c_str());
        return fn;
    } else {
        off = "Not verified on game build " + std::to_string(gBuild.load());
    }
    gSink((LV_WARN + mod + ": Rule '" + name + "' off: " + off).c_str());
    return 0;
}

int hookRuleRegister(lua_State* L) {
    if (gInDraw || gInConsole || gHookShutdown || gClosing || gBroken)
        return luaL_error(L, "Hooks.rule is unavailable in Draw, the console, or shutdown");
    luaL_checktype(L, 1, LUA_TTABLE);
    std::string mod = lua_tostring(L, lua_upvalueindex(1)), name = hookText(L, "name", 128), err, conflict;
    if (name.find_first_of("/\\") != std::string::npos) return luaL_error(L, "hook names cannot contain slashes");
    RuleParse p;
    contracts::Status st;
    p.d.key = mod + '/' + name;
    bool catalog = lua_getfield(L, 1, "target") != LUA_TNIL;
    lua_pop(L, 1);
    if (catalog) ruleTarget(L, p, st);
    else { hookText(L, "pattern", 4096); p.d.args = hookArgs(L); }
    auto order = gModOrder.find(mod);
    p.d.order = order == gModOrder.end() ? UINT32_MAX : order->second;
    ruleParams(L, p);
    ruleConds(L, p);
    ruleBlock(L, p);
    ruleChanges(L, p);
    ruleAfter(L, p);
    bool* recorded = static_cast<bool*>(lua_touserdata(L, lua_upvalueindex(2)));
    if (!hk::validRule(p.d, err)) {
        *recorded = true;  // validRule recorded it
        return luaL_error(L, "%s", err.c_str());
    }
    uintptr_t fn = ruleFn(L, p, st, mod, name);
    if (!fn) { lua_pushnil(L); return 1; }
    if (gNextHook == LUA_MAXINTEGER) return luaL_error(L, "hook handle limit reached; reload mods");
    int id = hk::addRule(fn, p.d, err, conflict);
    if (id < 0) {
        *recorded = true;
        return luaL_error(L, "%s", err.c_str());
    }
    if (!conflict.empty())
        gSink((LV_WARN + mod + ": Rule '" + name +
               (p.d.shape.block ? "' and " + conflict + " both block this call; the first in mod order wins"
                                : "' changes a value " + conflict + " changes too; both apply, in mod order")).c_str());
    lua_Integer handle = ++gNextHook;
    gHooks.emplace(handle, HookHandler{-1, LUA_NOREF, mod, name, p.d.args, id, p.params, p.hasList});
    lua_pushinteger(L, handle);
    return 1;
}

int hookRule(lua_State* L) { return registerGuarded(L, hookRuleRegister); }

// The calling mod's active rule with the handle at 1.
HookHandler& ownRule(lua_State* L) {
    auto it = gHooks.find(luaL_checkinteger(L, 1));
    if (it == gHooks.end() || it->second.rule < 0) luaL_error(L, "not an active rule");
    if (it->second.mod != lua_tostring(L, lua_upvalueindex(1))) luaL_error(L, "cannot change another mod's rule");
    return it->second;
}

// Hooks.param(id, name, value): the next calls use the new value.
int hookParam(lua_State* L) {
    HookHandler& h = ownRule(L);
    const char* name = luaL_checkstring(L, 2);
    double v = luaL_checknumber(L, 3);
    auto at = std::find(h.params.begin(), h.params.end(), name);
    if (at == h.params.end()) return luaL_error(L, "rule has no param '%s'", name);
    hk::setParam(h.rule, int(at - h.params.begin()), v);
    return 0;
}

// Hooks.list(id, {ids}): replaces the ids of the rule's in condition.
int hookList(lua_State* L) {
    HookHandler& h = ownRule(L);
    if (!h.hasList) return luaL_error(L, "rule has no in condition");
    hk::setList(h.rule, ruleIds(L, 2));
    return 0;
}

void removeHook(lua_State* L, lua_Integer id) {
    auto it = gHooks.find(id);
    if (it == gHooks.end()) return;
    if (it->second.rule >= 0) hk::removeRule(it->second.rule);
    else hk::unsubscribe(it->second.sub);
    luaL_unref(L, LUA_REGISTRYINDEX, it->second.ref);
    gHooks.erase(it);
}

int hookRemove(lua_State* L) {
    lua_Integer id = luaL_checkinteger(L, 1);
    auto it = gHooks.find(id);
    if (it != gHooks.end() && it->second.mod != lua_tostring(L, lua_upvalueindex(1)))
        return luaL_error(L, "cannot remove another mod's hook");
    removeHook(L, id);
    return 0;
}

void removeModHooks(lua_State* L, const std::string& mod) {
    for (auto it = gHooks.begin(); it != gHooks.end();) {
        auto cur = it++;
        if (cur->second.mod == mod) removeHook(L, cur->first);
    }
}

void stopHooks(lua_State* L) {
    hk::unsubscribeAll();
    if (L) for (const auto& [id, h] : gHooks) { (void)id; luaL_unref(L, LUA_REGISTRYINDEX, h.ref); }
    gHooks.clear();
}

void pushHookValue(lua_State* L, const hk::Arg& arg, const hk::Value& v, const hk::Batch& batch) {
    switch (arg.kind) {
    case hk::Skip: lua_pushnil(L); break;
    case hk::Bool: lua_pushboolean(L, v.bits != 0); break;
    case hk::F32: lua_pushnumber(L, std::bit_cast<float>(uint32_t(v.bits))); break;
    case hk::F64: lua_pushnumber(L, std::bit_cast<double>(v.bits)); break;
    case hk::Snapshot:
        if (!v.bits) lua_pushnil(L);
        else memcpy(newOwned(L, arg.type), batch.payload.data() + v.off, v.len);
        break;
    case hk::CStr:
        if (!v.bits) lua_pushnil(L);
        else lua_pushlstring(L, v.len ? reinterpret_cast<const char*>(batch.payload.data() + v.off) : "", v.len);
        break;
    case hk::EntityPtr:
        if (!v.len) { lua_pushnil(L); break; }
        [[fallthrough]];
    default: lua_pushinteger(L, std::bit_cast<int64_t>(v.bits)); break;
    }
}

void pushHookEvent(lua_State* L, const HookHandler& h, const hk::Batch& batch, const hk::Event& e) {
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, lua_Integer(e.seq)); lua_setfield(L, -2, "seq");
    lua_pushnumber(L, hk::seconds(e.qpc)); lua_setfield(L, -2, "time");
    lua_pushinteger(L, lua_Integer(e.dropped)); lua_setfield(L, -2, "dropped");
    lua_createtable(L, int(h.args.size()), 0);
    for (size_t i = 0; i < h.args.size(); ++i) {
        pushHookValue(L, h.args[i], e.v[i], batch);
        lua_rawseti(L, -2, lua_Integer(i + 1));
    }
    lua_setfield(L, -2, "args");
}

void runHook(lua_State* L, lua_Integer id, const HookHandler& h, const hk::Batch& batch) {
    for (const auto& e : batch.events) {
        if (!gHooks.contains(id) || gReload) break;
        int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, h.ref);
        pushHookEvent(L, h, batch, e);
        // Allocating the event can run Lua finalizers that remove a subscription.
        if (!gHooks.contains(id) || !hk::delivered(h.sub, batch.generation)) { lua_settop(L, top); break; }
        if (!call(L, 1, 0)) {
            gSink((LV_BAD + h.mod + ": Hook '" + h.name + "' removed after an error").c_str());
            removeHook(L, id);
        }
        lua_settop(L, top);
    }
}

void dispatchHooks(lua_State* L) {
    struct Run { lua_Integer id; HookHandler handler; hk::Batch batch; };
    std::vector<Run> runs;
    hk::Batch batch;
    // Seal every batch before any handler runs, including calls caused by another handler.
    for (const auto& [id, h] : gHooks)
        if (h.rule < 0 && hk::drain(h.sub, batch)) runs.push_back({id, h, std::exchange(batch, {})});
    for (const auto& r : runs) runHook(L, r.id, r.handler, r.batch);
}

// A mod's globals: its own table, falling back to the shared one, plus what has to know the mod: Keys.hotkey and
// Keys.input, Hooks, Settings.define, io inside its folder, and require from its folder.
void pushModEnv(lua_State* L, const std::string& name) {
    std::string dir = gModsDir + "\\" + name;
    lua_newtable(L);
    int env = lua_gettop(L);
    lua_newtable(L);
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, env);
    lua_newtable(L);
    lua_pushcfunction(L, keysOn);
    lua_setfield(L, -2, "on");
    for (bool input : {false, true}) {
        lua_pushstring(L, name.c_str());
        lua_pushboolean(L, input);
        lua_pushcclosure(L, keysBind, 2);
        lua_pushvalue(L, -1);
        lua_setfield(L, env, input ? "registerInput" : "registerHotkey");
        lua_setfield(L, -2, input ? "input" : "hotkey");
    }
    lua_setfield(L, env, "Keys");
    lua_newtable(L);
    for (const auto& f : {luaL_Reg{"observe", hookObserve}, luaL_Reg{"rule", hookRule}, luaL_Reg{"param", hookParam},
                          luaL_Reg{"list", hookList}, luaL_Reg{"remove", hookRemove}}) {
        lua_pushstring(L, name.c_str());
        lua_pushcclosure(L, f.func, 1);
        lua_setfield(L, -2, f.name);
    }
    lua_setfield(L, env, "Hooks");
    lua_newtable(L);
    lua_getfield(L, LUA_REGISTRYINDEX, kIo);
    for (const char* fn : {"open", "lines"}) {
        lua_pushstring(L, dir.c_str());
        lua_getfield(L, -2, fn);
        lua_pushboolean(L, fn[0] == 'o');
        lua_pushcclosure(L, modIo, 3);
        lua_setfield(L, -3, fn);
    }
    lua_pop(L, 1);
    lua_setfield(L, env, "io");
    lua_newtable(L);
    lua_pushstring(L, name.c_str());
    lua_pushcclosure(L, settingsDefine, 1);
    lua_setfield(L, -2, "define");
    lua_setfield(L, env, "Settings");
    lua_getfield(L, LUA_REGISTRYINDEX, kRequire);
    lua_pushvalue(L, env);
    lua_pushstring(L, dir.c_str());
    lua_pushstring(L, name.c_str());
    lua_call(L, 3, 1);
    lua_setfield(L, env, "require");
}

// In the table at idx, every function of mod becomes false (a list, like a dropped callback) or is removed (a map).
void dropModFns(lua_State* L, int idx, const std::string& mod, bool map) {
    idx = lua_absindex(L, idx);
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        bool mine = lua_isfunction(L, -1) && modOf(L, -1) == mod;
        lua_pop(L, 1);
        if (!mine) continue;
        lua_pushvalue(L, -1);  // the key, kept for lua_next
        if (map) lua_pushnil(L);
        else lua_pushboolean(L, 0);
        lua_rawset(L, idx);  // assigning to a field that exists is allowed while traversing
    }
}

// A mod whose init.lua failed is gone completely: what it registered before the error stops too (callbacks, events,
// keys, hotkeys, native hooks), so it can't half-run.
void unloadMod(lua_State* L, const std::string& mod) {
    for (const char* reg : {kTicks, kDraws, kOnInit, kOnOpen, kOnClose, kOnShutdown, kOnEnter, kOnLeave, kKeys, kBinds}) {
        lua_getfield(L, LUA_REGISTRYINDEX, reg);
        dropModFns(L, -1, mod, reg == kKeys || reg == kBinds);
        lua_pop(L, 1);
    }
    lua_getfield(L, LUA_REGISTRYINDEX, kObservers);  // event type hash -> handler list
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        if (lua_istable(L, -1)) dropModFns(L, -1, mod, false);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    {
        std::lock_guard lock(gBindMx);
        std::erase_if(gBinds, [&](const Binding& b) { return b.mod == mod; });
    }
    removeModHooks(L, mod);
    wlog::leave(mod, gSink);
    mset::drop(mod, gSink);
}

// mods/<name>/mod.json, optional: {"requires": "0.1.1", "builds": [1793791052]}: the oldest Shrouded Engine Tweaks the
// mod needs, and the game builds it was made for. Read with the mods' own json.decode.
struct ModInfo {
    std::string needs;  // "requires" in mod.json
    std::vector<uint32_t> builds;
};

ModInfo readModJson(lua_State* L, const std::string& name) {
    ModInfo info;
    FILE* f = nullptr;
    if (fopen_s(&f, (gModsDir + "\\" + name + "\\mod.json").c_str(), "rb") != 0 || !f) return info;
    std::string text;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);  // Notepad's UTF-8 BOM
    int top = lua_gettop(L);
    lua_getglobal(L, "json");
    lua_getfield(L, -1, "decode");
    lua_pushlstring(L, text.data(), text.size());
    if (lua_pcall(L, 1, 1, 0) != LUA_OK || !lua_istable(L, -1)) {
        std::string why = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "not a JSON object";
        gSink((LV_WARN + name + ": mod.json not read (" + why + "), loading without it").c_str());
        lua_settop(L, top);
        return info;
    }
    if (lua_getfield(L, -1, "requires") == LUA_TSTRING) info.needs = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (lua_getfield(L, -1, "builds") == LUA_TTABLE) {
        for (lua_Integer i = 1; lua_rawgeti(L, -1, i) == LUA_TNUMBER; i++, lua_pop(L, 1))
            info.builds.push_back(static_cast<uint32_t>(lua_tointeger(L, -1)));
    }
    lua_settop(L, top);
    return info;
}

// True if version want ("0.2.0") is newer than this Shrouded Engine Tweaks. Missing parts count as 0.
bool newerThanSet(const std::string& want) {
    int a[3] = {}, b[3] = {};
    sscanf_s(want.c_str(), "%d.%d.%d", &a[0], &a[1], &a[2]);
    sscanf_s(RUNTIME_VERSION, "%d.%d.%d", &b[0], &b[1], &b[2]);
    return std::lexicographical_compare(b, b + 3, a, a + 3);
}

// "Mod Loaded: x", or failed, with the builds a mod.json named when this game build isn't one of them.
std::string modLine(const std::string& name, bool ok, const ModInfo& info) {
    bool other = !info.builds.empty() && std::find(info.builds.begin(), info.builds.end(), gBuild) == info.builds.end();
    std::string line = (ok ? (other ? LV_WARN "Mod Loaded: " : LV_OK "Mod Loaded: ") : LV_BAD "Mod Failed: ") + name;
    if (!other) return line;
    std::string made;
    for (uint32_t b : info.builds) made += (made.empty() ? "" : ", ") + std::to_string(b);
    line += " (made for game build" + std::string(info.builds.size() > 1 ? "s " : " ") + made + ", this is " +
            std::to_string(gBuild) + (ok ? ")" : ": Wrong game version?)");
    return line;
}

// Runs mods/<name>/init.lua for every mod folder, each in its own environment. A failing mod is reported and unloaded.
// What init.lua returns is kept for GetMod (true if it returns nothing).
void loadMods(lua_State* L) {
    int loaded = 0, failed = 0;
    gModOrder.clear();
    lua_getfield(L, LUA_REGISTRYINDEX, kMods);
    int mods = lua_gettop(L);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((gModsDir + "\\*").c_str(), &fd);
    for (bool more = h != INVALID_HANDLE_VALUE; more; more = FindNextFileA(h, &fd) != 0) {
        std::string name = fd.cFileName, init = gModsDir + "\\" + name + "\\init.lua";
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name[0] == '.') continue;
        if (GetFileAttributesA(init.c_str()) == INVALID_FILE_ATTRIBUTES) {
            gSink((LV_WARN "Mod Skipped: " + name + " (no init.lua)").c_str());
            continue;
        }
        gModOrder.emplace(name, uint32_t(gModOrder.size()));
        ModInfo info = readModJson(L, name);
        if (!info.needs.empty() && newerThanSet(info.needs)) {
            failed++;
            gSink((LV_BAD "Mod Skipped: " + name + " (needs Shrouded Engine Tweaks " + info.needs +
                   ", this is " RUNTIME_VERSION ")").c_str());
            continue;
        }
        bool ok = loadText(L, init, ("@" + name + "/init.lua").c_str());
        if (!ok) {
            gSink((LV_BAD + std::string(lua_tostring(L, -1))).c_str());  // call() prints its own errors
        } else {
            pushModEnv(L, name);
            lua_setupvalue(L, -2, 1);  // a main chunk's only upvalue is _ENV
            ok = call(L, 0, 1);
        }
        if (ok) {
            if (lua_isnil(L, -1)) lua_pushboolean(L, 1);
            lua_setfield(L, mods, name.c_str());
            loaded++;
        } else {
            failed++;
            unloadMod(L, name);
        }
        lua_settop(L, mods);
        gSink(modLine(name, ok, info).c_str());
    }
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    lua_pop(L, 1);
    gSink(((failed ? LV_WARN "Mods: " : LV_OK "Mods: ") + std::to_string(loaded) + " loaded, " + std::to_string(failed) + " failed").c_str());
}

// GetMod(name): what that mod's init.lua returned, true if it returned nothing, nil if it isn't loaded.
int getMod(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kMods);
    lua_getfield(L, -1, luaL_checkstring(L, 1));
    return 1;
}

// The mod the function at idx belongs to: the one running when it was registered (tagFn), else its chunk name up to the
// first / ("@hello/init.lua" is hello). Otherwise "console" for a typed line and "load" for a string chunk, and isMod
// is false.
std::string modOf(lua_State* L, int idx, bool* isMod) {
    idx = lua_absindex(L, idx);
    if (isMod) *isMod = true;
    lua_getfield(L, LUA_REGISTRYINDEX, kFnMod);
    lua_pushvalue(L, idx);
    if (lua_rawget(L, -2) == LUA_TSTRING) {
        std::string m = lua_tostring(L, -1);
        lua_pop(L, 2);
        return m;
    }
    lua_pop(L, 2);
    lua_Debug ar;
    lua_pushvalue(L, idx);
    lua_getinfo(L, ">S", &ar);
    std::string_view s = ar.source ? ar.source : "";
    if (isMod) *isMod = s.starts_with('@');
    if (!s.starts_with('@') && !s.starts_with('=')) return "load";
    s.remove_prefix(1);
    return std::string(s.substr(0, s.find('/')));
}

// Drops callback i of the list on top of the stack after an error, naming its mod: "hello: tick callback removed".
void dropCallback(lua_State* L, lua_Integer i, const char* what) {
    lua_rawgeti(L, -1, i);
    gSink((LV_BAD + modOf(L, -1) + ": " + char(toupper(static_cast<unsigned char>(*what))) + (what + 1) + " removed after an error").c_str());
    lua_pop(L, 1);
    lua_pushboolean(L, 0);
    lua_rawseti(L, -2, i);
}

// A Tick or Draw callback slower than this holds up the game (the server tick or frame, or the mods' Draw).
constexpr double kSlowMs = 50;

// One yellow line per mod and kind every 10 s, naming the mod whose callback i (in the list on top) was slow.
void warnSlow(lua_State* L, lua_Integer i, const char* what, double ms) {
    static std::map<std::string, std::chrono::steady_clock::time_point> told;  // under gLuaMx, like all Lua work
    lua_rawgeti(L, -1, i);
    std::string mod = modOf(L, -1);
    lua_pop(L, 1);
    auto now = std::chrono::steady_clock::now();
    auto [it, fresh] = told.try_emplace(mod + "." + what, now);
    if (!fresh && now - it->second < std::chrono::seconds(10)) return;
    it->second = now;
    char line[256];
    snprintf(line, sizeof line, LV_WARN "%s: A %s callback took %.0f ms (the game waits for it)", mod.c_str(), what, ms);
    gSink(line);
}

// Runs every callback in a list: Tick.add, Draw.add or a registerForEvent event (what names it in the error line).
// A callback that errors is dropped so it can't repeat the error every tick or frame. Tick callbacks get dt, and Draw
// callbacks get ImGui's stacks restored, so a Begin without End can't break the frame.
void runCallbacks(lua_State* L, const char* reg, const char* what) {
    bool draw = reg == kDraws, tick = reg == kTicks;
    lua_getfield(L, LUA_REGISTRYINDEX, reg);
    for (lua_Integer i = 1; i <= lua_Integer(lua_rawlen(L, -1)); i++) {
        lua_rawgeti(L, -1, i);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        if (draw) rt::imguiMark();
        if (tick) lua_pushnumber(L, gDt);
        auto start = std::chrono::steady_clock::now();
        if (!call(L, tick ? 1 : 0, 0)) {
            if (draw) rt::imguiRecover();
            dropCallback(L, i, (std::string(what) + " callback").c_str());
            continue;
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if ((tick || draw) && ms > kSlowMs) warnSlow(L, i, what, ms);
    }
    lua_pop(L, 1);
}

bool isEvent(refl::Type t) {
    for (int i = 0; t && t.prim() == refl::Struct && i < 32; i++, t = t.inner()) {
        if (t.qname() == "keen::ecs::Event") return true;
    }
    return false;
}

refl::Type eventType(lua_State* L, int arg) {
    const char* name = luaL_checkstring(L, arg);
    refl::Type t = refl::byName(name);
    if (!t || !isEvent(t)) luaL_error(L, "'%s' is not an event type", name);
    return t;
}

// Observe(name, fn): fn(event) for every event of that type, after each server tick.
int observe(lua_State* L) {
    refl::Type t = eventType(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    tagFn(L, 2);
    lua_getfield(L, LUA_REGISTRYINDEX, kObservers);
    if (lua_rawgeti(L, -1, t.hash()) == LUA_TNIL) {  // handler list for this type
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_rawseti(L, -3, t.hash());
    }
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, lua_Integer(lua_rawlen(L, -2)) + 1);
    return 0;
}

// Events.post(name, fields): fields are checked in a scratch copy first, so a bad table posts nothing.
// GameEvent.timeStamp is set to now. Returns false if the game's event buffer is full.
int eventsPost(lua_State* L) {
    refl::Type t = eventType(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (t.size() >= 0x100) return luaL_error(L, "%s is too big to post", qname(t).c_str());
    needGame(L);
    needWorld(L);
    if (!gWorld || ecs::eventIndex(gWorld, t.hash()) < 0) return luaL_error(L, "this world has no %s stream", qname(t).c_str());
    uint8_t scratch[0x100] = {};
    Ref r{0, -1, 0, t.size(), t.p, scratch, gGen, true};
    refl::Field ts;
    if (refl::field(t, "timeStamp", ts) && ts.type.size() == 8) {
        int64_t now = ecs::simTime(gWorld);
        memcpy(scratch + ts.offset, &now, 8);
    }
    fill(L, r, 0, t, 2);
    uint8_t* p = ecs::postEvent(gWorld, t.hash(), t.size());
    if (p) memcpy(p, scratch, t.size());
    lua_pushboolean(L, p != nullptr);
    return 1;
}

// ---- Admin commands ----
// The game's consumer system (admin_player) runs a command when the player's ServerConsumedPlayerInput counter
// differs from the command's version in PlayerInput.fromAdminClient, then sets the counter to the version. The game
// rebuilds PlayerInput inside the world tick, so a hook right before the consumer writes the commands
// (rt::applyAdmin). It moves the counter, not the version: the next rebuild then leaves the pair equal, and the
// command fires exactly once.

struct AdminCmd {
    const char* input;     // field of AdminClientPlayerInputData
    const char* consumed;  // its counter in ServerConsumedPlayerInput
    bool merge = false;    // unset fields keep their current value instead of 0 (game settings)
    int sys = 0;           // the system that runs it: 0 admin_player, 1 inventory_actions
};

// The commands admin_player handles, then the ones inventory_actions does (create item last, for kGiveCmd). Unset
// fields are 0, except in game settings, which merge into the current ones. Left out: kill faction (release builds
// don't register debug_kill_faction, the system that reads its event), and destroy item and perk, which need inventory
// slot ids.
constexpr AdminCmd kAdmin[] = {
    {"adminSetDaytime", "consumedAdminSetDaytime"},
    {"adminTeleport", "consumedAdminTeleport"},
    {"adminSetFlameLevel", "consumedAdminSetFlameLevel"},
    {"adminSetCharacterLevel", "consumedAdminSetCharacterLevel"},
    {"adminSpawnCommand", "consumedAdminSpawnAction"},
    {"adminChangeGameSettingsAction", "consumedAdminChangeGameSettingsAction", true},
    {"adminApplyInventory", "consumedAdminApplyInventory", false, 1},
    {"adminRepairAllItemsAction", "consumedAdminRepairAllItemsAction", false, 1},
    {"adminRefillAllFuelAction", "consumedAdminRefillAllFuelAction", false, 1},
    {"adminInventoryCreateAction", "consumedAdminInventoryCreateAction", false, 1},
};
constexpr const AdminCmd* kGiveCmd = &kAdmin[std::size(kAdmin) - 1];

struct AdminMsg {
    const AdminCmd* cmd;
    std::vector<uint8_t> data;  // the whole action struct; its version is replaced by the live one
    std::vector<uint8_t> mask;  // merge commands: 1 for every byte the caller set
    bool give = false;          // made by Admin.give's queue, not Admin.send
};

// Admin.give's queue, under gLuaMx. The game makes one item, or one full stack, per create command, so a give takes
// one command per tick until its count is reached.
// Limit: one command per tick; batch them if giving hundreds of unstackable items ever matters.
struct Give {
    uint32_t item, left;  // ItemInfo.itemId, and how many are still to give
    uint16_t stack;       // its maxStackSize
    uint8_t level;        // 0: the game picks
    int rarity;           // -1: the game picks
};
std::deque<Give> gGives;
bool gGiveBusy;  // a give command is queued, armed or sent and not checked yet
bool gGiveOk;    // inventory_actions is hooked

bool gAdminOk;                      // the consumer is hooked
std::vector<AdminMsg> gAdminQueue;  // from Lua, under gLuaMx
// Armed in the pre-tick step, written by the consumer hook (maybe on a job thread), checked the next tick. Never held
// while touching game memory: an access violation there would skip the unlock.
std::mutex gAdminMx;
std::vector<AdminMsg> gArmed, gSent;
uintptr_t gAdminWorld;
std::atomic<bool> gHasArmed;
// ecs::GameSettings: its place among the consumer's resources (-1: not found), and the resource itself, taken
// from inside the consumer once it checks out (0: not yet). The first failed check of a session is logged.
int gSettingsIdx = -1;
uint32_t gSettingsCount;
std::atomic<uintptr_t> gSettings;
std::atomic<bool> gSettingsWarned;

// Where a command lives: its action inside PlayerInput, the action's version, and the counter. False if the
// reflection lacks one of them (a game update).
struct AdminSlot {
    refl::Type action;
    uint32_t off, version, consumed;
};

bool adminSlot(const AdminCmd& c, AdminSlot& s) {
    refl::Type pi = refl::byName("PlayerInput"), sc = refl::byName("ServerConsumedPlayerInput");
    refl::Field from, act, ver, con;
    if (!pi || !sc || !refl::field(pi, "fromAdminClient", from) || !refl::field(from.type.resolved(), c.input, act) ||
        !refl::field(act.type.resolved(), "versionData", ver) || !refl::field(sc, c.consumed, con))
        return false;
    s = {act.type.resolved(), from.offset + act.offset, ver.offset, con.offset};
    return true;
}

// The player's entity id (the same player as Ecs.player()) with its PlayerInput and ServerConsumedPlayerInput, or 0.
uint32_t playerInputs(uintptr_t world, uint8_t*& in, uint8_t*& con) {
    refl::Type pi = refl::byName("PlayerInput"), sc = refl::byName("ServerConsumedPlayerInput");
    int ii = pi ? ecs::compIndex(world, pi.hash()) : -1, ci = sc ? ecs::compIndex(world, sc.hash()) : -1;
    uint32_t p = ecs::firstWith(world, ii);
    in = p ? ecs::component(world, p, ii) : nullptr;
    con = p ? ecs::component(world, p, ci) : nullptr;
    return in && con ? p : 0;
}

// Admin.send(command, fields): fills a zeroed copy of the action, checked like Events.post, and queues it. A newer
// call for the same command replaces the queued one; for game settings the fields of both go through. Touches no
// game memory, so Draw callbacks may call it.
int adminSend(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    auto c = std::find_if(std::begin(kAdmin), std::end(kAdmin), [&](const AdminCmd& a) { return strcmp(a.input, name) == 0; });
    if (c == std::end(kAdmin)) return luaL_error(L, "'%s' is not an admin command (see docs/api-reference.md)", name);
    needWorld(L);
    if (!c->sys && !gAdminOk) return luaL_error(L, "admin commands are unavailable in this game build");
    if (c->sys && !gGiveOk) return luaL_error(L, "%s is unavailable in this game build", name);
    AdminSlot s;
    if (!adminSlot(*c, s)) return luaL_error(L, "%s is missing from this game build", name);
    if (lua_getfield(L, 2, "versionData") != LUA_TNIL) return luaL_error(L, "versionData is set by the runtime");
    lua_pop(L, 1);
    std::vector<uint8_t> data(s.action.size()), mask(c->merge ? s.action.size() : 0);
    fill(L, {0, -1, 0, s.action.size(), s.action.p, data.data(), gGen, true}, 0, s.action, 2, c->merge ? mask.data() : nullptr);
    auto same = std::find_if(gAdminQueue.begin(), gAdminQueue.end(), [&](const AdminMsg& m) { return m.cmd == c; });
    if (same == gAdminQueue.end()) {
        gAdminQueue.push_back({c, std::move(data), std::move(mask)});
    } else if (!c->merge) {
        same->data = std::move(data);
    } else {
        for (size_t i = 0; i < mask.size(); i++)
            if (mask[i]) same->data[i] = data[i], same->mask[i] = 1;
    }
    return 0;
}

// Admin.setTime(hours): time of day, 0 <= hours < 24 (12.5 = 12:30). syncDaytime is in nanoseconds.
int adminSetTime(lua_State* L) {
    double h = luaL_checknumber(L, 1);
    luaL_argcheck(L, h >= 0 && h < 24, 1, "hours must be at least 0 and below 24");
    lua_settop(L, 0);
    lua_pushliteral(L, "adminSetDaytime");
    lua_createtable(L, 0, 1);
    lua_pushinteger(L, llround(h * 3600e9));
    lua_setfield(L, -2, "syncDaytime");
    return adminSend(L);
}

// Admin.teleport(x, y, z [, exact]): meters. The game copies the orientation as given, and all zeros is no rotation,
// so it gets the identity. Unless exact, the game looks for a safe spot near the target.
int adminTeleport(lua_State* L) {
    const lua_Number pos[] = {luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3)};
    bool exact = lua_toboolean(L, 4);
    lua_settop(L, 0);
    lua_pushliteral(L, "adminTeleport");
    lua_createtable(L, 0, 2);
    lua_createtable(L, 0, 2);  // transform
    lua_createtable(L, 0, 3);
    for (int i = 0; i < 3; i++) {
        lua_pushnumber(L, pos[i]);
        lua_setfield(L, -2, i == 0 ? "x" : i == 1 ? "y" : "z");
    }
    lua_setfield(L, -2, "position");
    lua_createtable(L, 0, 1);
    lua_pushnumber(L, 1);
    lua_setfield(L, -2, "w");
    lua_setfield(L, -2, "orientation");
    lua_setfield(L, -2, "transform");
    lua_pushboolean(L, !exact);
    lua_setfield(L, -2, "searchBestSpawnPosition");
    return adminSend(L);
}

// Sends cmd with field = argument 1, an integer from lo to hi.
int sendInt(lua_State* L, const char* cmd, const char* field, lua_Integer lo, lua_Integer hi) {
    lua_Integer v = luaL_checkinteger(L, 1);
    if (v < lo || v > hi) return luaL_argerror(L, 1, lua_pushfstring(L, "must be from %I to %I", lo, hi));
    lua_settop(L, 0);
    lua_pushstring(L, cmd);
    lua_createtable(L, 0, 1);
    lua_pushinteger(L, v);
    lua_setfield(L, -2, field);
    return adminSend(L);
}

// Admin.setLevel(n): your character level. targetEntityId stays 0, which applyAdmin turns into the player. The game
// resets the XP toward the next level.
int adminSetLevel(lua_State* L) { return sendInt(L, "adminSetCharacterLevel", "level", 1, 255); }

// Admin.setFlameLevel(n): the world's flame level, saved with the world.
int adminSetFlameLevel(lua_State* L) { return sendInt(L, "adminSetFlameLevel", "flameLevel", 1, 255); }

// The resource of the given type that argument 1 names: its GUID, or a name as Res.find takes it.
const res::Entry* namedRes(lua_State* L, const char* type, const char* what) {
    needRes(L);
    refl::Type t = res::typeByName(type);
    if (!t) luaL_error(L, "this game build has no %s", type);
    size_t n;
    const char* s = luaL_checklstring(L, 1, &n);
    uint8_t g[16];
    const res::Entry* e = res::parseGuid({s, n}, g) ? res::find(g, t.hash()) : findNamed(t.hash(), lower({s, n}));
    if (!e) luaL_argerror(L, 1, lua_pushfstring(L, "no %s has this GUID or name (Res.each(\"%s\") lists them)", what, type));
    return e;
}

// The TemplateResource GUID that argument 1 names: the GUID itself, or a template's name (ignoring case).
std::string templateGuid(lua_State* L) { return res::guidStr(namedRes(L, "TemplateResource", "template")->guid); }

// The host player's CurrentTransform position (WorldPosition: meters * 2^32), or a Lua error.
void playerPos(lua_State* L, int64_t pos[3]) {
    needGame(L);
    refl::Type pi = refl::byName("PlayerInput"), ct = refl::byName("CurrentTransform");
    int ii = gWorld && pi ? ecs::compIndex(gWorld, pi.hash()) : -1;
    int ci = gWorld && ct ? ecs::compIndex(gWorld, ct.hash()) : -1;
    uint32_t id = gWorld ? ecs::firstWith(gWorld, ii) : 0;
    uint8_t* c = id ? ecs::component(gWorld, id, ci) : nullptr;
    refl::Field tf, p;
    if (!c || !refl::field(ct, "transform", tf) || !refl::field(tf.type.resolved(), "position", p) ||
        p.type.resolved().size() != 24)
        luaL_error(L, "no player position to spawn at");
    memcpy(pos, c + tf.offset + p.offset, 24);
}

// Admin.spawn(template [, count [, level]]): a TemplateResource, by GUID or name, 5 m from you along x.
int adminSpawn(lua_State* L) {
    needWorld(L);
    std::string guid = templateGuid(L);
    lua_Integer count = luaL_optinteger(L, 2, 1), level = luaL_optinteger(L, 3, 1);
    luaL_argcheck(L, count >= 1 && count <= 255, 2, "must be from 1 to 255");
    luaL_argcheck(L, level >= 1 && level <= 65535, 3, "must be from 1 to 65535");
    int64_t pos[3];
    playerPos(L, pos);
    pos[0] += 5ll << 32;
    lua_settop(L, 0);
    lua_pushliteral(L, "adminSpawnCommand");
    lua_createtable(L, 0, 4);
    lua_pushstring(L, guid.c_str());
    lua_setfield(L, -2, "templateGuid");
    lua_pushinteger(L, count);
    lua_setfield(L, -2, "count");
    lua_pushinteger(L, level);
    lua_setfield(L, -2, "level");
    lua_createtable(L, 0, 3);
    for (int i = 0; i < 3; i++) {
        lua_pushinteger(L, pos[i]);
        lua_setfield(L, -2, i == 0 ? "x" : i == 1 ? "y" : "z");
    }
    lua_setfield(L, -2, "position");
    return adminSend(L);
}

// Value of enum t from argument arg: one of its names (ignoring case) or numbers.
lua_Integer enumArg(lua_State* L, int arg, refl::Type t) {
    if (lua_type(L, arg) == LUA_TNUMBER) {
        lua_Integer v = luaL_checkinteger(L, arg);
        luaL_argcheck(L, t.fieldCount() && enumHas(t, v), arg, lua_pushfstring(L, "not a value of %s", qname(t).c_str()));
        return v;
    }
    std::string want = lower(luaL_checkstring(L, arg));
    lua_Integer v = -1;
    refl::forEnum(t, [&](std::string_view n, int64_t e) {
        if (lower(n) == want) v = e;
    });
    if (v < 0) luaL_argerror(L, arg, lua_pushfstring(L, "not a value of %s", qname(t).c_str()));
    return v;
}

// Admin.give(item [, count [, level [, rarity]]]): an ItemInfo by GUID, debugName or English name, into your
// inventory. Queued: the runtime sends one create command per tick until count is reached.
int adminGive(lua_State* L) {
    needWorld(L);
    if (!gGiveOk) return luaL_error(L, "Admin.give is unavailable in this game build");
    const res::Entry* e = namedRes(L, "ItemInfo", "item");
    lua_Integer count = luaL_optinteger(L, 2, 1), level = luaL_optinteger(L, 3, 0);
    luaL_argcheck(L, count >= 1 && count <= 999, 2, "must be from 1 to 999");
    luaL_argcheck(L, lua_isnoneornil(L, 3) || (level >= 1 && level <= 255), 3, "must be from 1 to 255");
    AdminSlot s;
    refl::Field id, stack, rarity;
    refl::Type it = refl::byHash(e->type);
    if (!adminSlot(*kGiveCmd, s) || !refl::field(s.action, "rarity", rarity) || !refl::field(it, "itemId", id) ||
        !refl::field(it, "maxStackSize", stack))
        return luaL_error(L, "Admin.give doesn't match this game build");
    int r = lua_isnoneornil(L, 4) ? -1 : int(enumArg(L, 4, rarity.type.resolved()));
    Give g{0, uint32_t(count), 0, uint8_t(level), r};
    memcpy(&g.item, res::data(*e) + id.offset, 4);
    memcpy(&g.stack, res::data(*e) + stack.offset, 2);
    g.stack = std::max<uint16_t>(g.stack, 1);
    gGives.push_back(g);
    return 0;
}

// Admin.settings(): the current game settings as a plain table, or nil until the consumer ran this session.
int adminSettings(lua_State* L) {
    needGame(L);
    needWorld(L);
    refl::Type t = refl::byName("GameSettings");
    auto p = reinterpret_cast<const uint8_t*>(gSettings.load());
    if (!p || !t) {
        lua_pushnil(L);
        return 1;
    }
    std::vector<uint8_t> copy(p, p + t.size());
    Ref r{0, -1, 0, t.size(), t.p, copy.data(), gGen, false};
    lua_newtable(L);
    refl::forFields(t, [&](const refl::Field& f) {
        uint8_t k = unwrap(f.type.resolved()).prim();
        if (k == refl::Struct || k == refl::StaticArray) return;  // a proxy would outlive the copy; GameSettings has none
        push(L, r, f.offset, f.type);
        lua_setfield(L, -2, std::string(f.name).c_str());
    });
    return 1;
}

// Admin.changeSettings(fields): Admin.send of the settings action; only the given settings change.
int adminChangeSettings(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);
    lua_createtable(L, 0, 1);
    lua_insert(L, 1);                    // {}, fields
    lua_setfield(L, 1, "gameSettings");  // {gameSettings = fields}
    lua_pushliteral(L, "adminChangeGameSettingsAction");
    lua_insert(L, 1);
    return adminSend(L);
}

// The front give as one create command, unless one is in flight or Admin.send queued the command this tick: a full
// stack while at least a stack is left, else one item.
void queueGive() {
    AdminSlot s;
    if (gGiveBusy || gGives.empty() || !adminSlot(*kGiveCmd, s)) return;
    if (std::any_of(gAdminQueue.begin(), gAdminQueue.end(), [](const AdminMsg& m) { return m.cmd == kGiveCmd; })) return;
    const Give& g = gGives.front();
    std::vector<uint8_t> d(s.action.size());
    auto set = [&](const char* name, uint32_t v) {
        refl::Field f;
        if (refl::field(s.action, name, f)) memcpy(d.data() + f.offset, &v, std::min<uint32_t>(f.type.size(), 4));
    };
    set("itemId", g.item);
    set("createFullStack", g.left >= g.stack);
    set("level", g.level);
    if (g.rarity >= 0) set("hasItemRarity", 1), set("rarity", uint32_t(g.rarity));
    gAdminQueue.push_back({kGiveCmd, std::move(d), {}, true});
    gGiveBusy = true;
}

// checkSent's verdict on the give command sent last tick: count it, or drop the give the game didn't run.
void gaveOne(bool ran) {
    gGiveBusy = false;
    if (gGives.empty()) return;
    Give& g = gGives.front();
    if (ran) g.left -= g.left >= g.stack ? g.stack : 1;
    if (!ran || !g.left) gGives.pop_front();
}

// End of the pre-tick step: hands this tick's commands to the consumer hooks.
void armAdmin(uintptr_t world) {
    queueGive();
    if (gAdminQueue.empty()) return;
    std::lock_guard lock(gAdminMx);
    for (AdminMsg& m : gAdminQueue) gArmed.push_back(std::move(m));  // applied in order: a later one overwrites
    gAdminQueue.clear();
    gAdminWorld = world;
    gHasArmed = true;
}

// Start of the pre-tick step: a command sent last tick whose counter still differs from its version wasn't run (the
// consumer skipped the player). Its counter is set back, so it can't fire later with other data.
void checkSent(uintptr_t world) {
    std::vector<AdminMsg> sent;
    {
        std::lock_guard lock(gAdminMx);
        sent = std::exchange(gSent, {});
    }
    uint8_t *in, *con;
    if (sent.empty() || !playerInputs(world, in, con)) {
        for (const AdminMsg& m : sent)
            if (m.give) gGiveBusy = false;  // can't tell if it ran: the front give goes again
        return;
    }
    for (const AdminMsg& m : sent) {
        AdminSlot s;
        bool ran = !adminSlot(*m.cmd, s) || memcmp(in + s.off + s.version, con + s.consumed, 4) == 0;
        if (m.give) gaveOne(ran);
        if (ran) continue;
        memcpy(con + s.consumed, in + s.off + s.version, 4);
        gSink((LV_BAD "Admin Command " + std::string(m.cmd->input) + ": Not run by the game").c_str());
    }
}

// A new session: nothing queued for the old world may reach the new one, and its settings are found again.
void resetAdmin() {
    gAdminQueue.clear();
    gGives.clear();
    gGiveBusy = false;
    gSettings = 0;
    gSettingsWarned = false;
    std::lock_guard lock(gAdminMx);
    gArmed.clear();
    gSent.clear();
    gHasArmed = false;
}

using refl::badField;

// Inside the consumer, until it checks out (a load may still be filling the resource): the ecs::GameSettings resource
// from the consumer's own query slot, if that slot's group matches the descriptor's resources and the content looks
// right. The first failure of a session is logged with its reason.
void captureSettings(void* job) {
    refl::Type t = refl::byName("GameSettings");
    ecs::ResSlot q{};
    char why[160] = "";
    if (!t || !ecs::querySlot(job, kSettingsSlot, q)) {
        snprintf(why, sizeof why, "query slot %u holds no resource", kSettingsSlot);
    } else if (q.group != gSettingsCount || q.index != uint64_t(gSettingsIdx)) {
        snprintf(why, sizeof why, "slot %u is %llu of %llu, expected %d of %u", kSettingsSlot,
                 static_cast<unsigned long long>(q.index), static_cast<unsigned long long>(q.group), gSettingsIdx, gSettingsCount);
    } else if (refl::Field f = badField(t, reinterpret_cast<const uint8_t*>(q.ptr)); !f.name.empty()) {
        snprintf(why, sizeof why, "field %.*s = %lld", int(f.name.size()), f.name.data(),
                 static_cast<long long>(readInt(reinterpret_cast<const uint8_t*>(q.ptr) + f.offset, f.type.resolved().size(), false)));
    }
    if (!why[0]) {
        gSettings = q.ptr;
        gSink(LV_OK "Game Settings: Ready");
    } else if (!gSettingsWarned.exchange(true)) {
        gSink((LV_WARN "Game Settings: Not recognized yet (" + std::string(why) + "), still trying").c_str());
    }
}

// Game settings: the current settings, overlaid with only the bytes the caller set. While the settings are unknown the
// command is dropped (and logged) rather than zeroing every setting it doesn't name.
bool mergeSettings(AdminMsg& m, const AdminSlot& s) {
    refl::Field gs;
    if (!m.cmd->merge) return true;
    auto cur = reinterpret_cast<const uint8_t*>(gSettings.load());
    if (!cur || !refl::field(s.action, "gameSettings", gs)) {
        gSink((LV_BAD "Admin Command " + std::string(m.cmd->input) + ": Not run, game settings unknown").c_str());
        return false;
    }
    for (uint32_t i = 0; i < gs.type.size(); i++)
        if (!m.mask[gs.offset + i]) m.data[gs.offset + i] = cur[i];
    return true;
}

// Fields the runtime fills in: set daytime's sync tick (the sim time the new time of day starts from, as the save
// loader passes it), and a character-level target of 0, which means the player.
void patchAction(uintptr_t world, refl::Type action, uint8_t* act, uint32_t player) {
    refl::Field f;
    if (refl::field(action, "syncDaytimeTick", f)) {
        int64_t now = ecs::simTime(world);
        memcpy(act + f.offset, &now, 8);
    }
    uint32_t target = 1;
    if (refl::field(action, "targetEntityId", f)) memcpy(&target, act + f.offset, 4);
    if (!target) memcpy(act + f.offset, &player, 4);
}

// Calls the handler list on top of the stack with a read-only proxy of ev. A handler that errors is dropped.
void callHandlers(lua_State* L, refl::Type t, const uint8_t* ev) {
    for (lua_Integer i = 1; i <= lua_Integer(lua_rawlen(L, -1)); i++) {
        lua_rawgeti(L, -1, i);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        newRef(L, {0, -1, 0, t.size(), t.p, const_cast<uint8_t*>(ev), gGen, false});
        if (!call(L, 1, 0)) dropCallback(L, i, "event handler");
    }
}

// Runs Observe handlers for this tick's events. Types are collected first, so a handler may call Observe.
void dispatchEvents(lua_State* L, uintptr_t world) {
    std::vector<uint32_t> types;
    lua_getfield(L, LUA_REGISTRYINDEX, kObservers);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        types.push_back(static_cast<uint32_t>(lua_tointeger(L, -2)));
        lua_pop(L, 1);
    }
    for (uint32_t h : types) {
        refl::Type t = refl::byHash(h);
        int idx = t ? ecs::eventIndex(world, h) : -1;
        if (idx < 0) continue;
        lua_rawgeti(L, -1, h);
        ecs::forEachEvent(world, idx, [&](const uint8_t* ev) { callHandlers(L, t, ev); });
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    gGen++;  // proxies handed out during this dispatch are invalid from now on
}

// The game's own window, not the runtime console: there F-keys are line editing (F6 types ^Z).
bool gameFocused() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() && fg != GetConsoleWindow();
}

bool keyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// A binding is active while its key and exactly its modifiers are held. A hotkey fires when it becomes active; an
// input gets true then and false when it stops, which losing focus also does. The callbacks run after gBindMx is
// released, since they may register bindings.
void pollBinds(lua_State* L, bool focus) {
    std::vector<std::pair<std::string, int>> fire;  // key, and -1 for a hotkey or the input's new state
    int mods = rt::heldMods();
    {
        std::lock_guard lock(gBindMx);
        for (Binding& b : gBinds) {
            bool now = focus && b.vk && keyDown(b.vk) && mods == b.mods;
            if (now == b.held) continue;
            b.held = now;
            if (b.input || now) fire.emplace_back(b.key, b.input ? int(now) : -1);
        }
    }
    if (fire.empty()) return;
    lua_getfield(L, LUA_REGISTRYINDEX, kBinds);
    for (const auto& [key, arg] : fire) {
        lua_getfield(L, -1, key.c_str());
        if (arg >= 0) lua_pushboolean(L, arg);
        call(L, arg >= 0 ? 1 : 0, 0);
    }
    lua_pop(L, 1);
}

// Calls Keys.on handlers once per key press (down edge), and the bound hotkeys and inputs, only while the game window
// has focus.
void pollKeys(lua_State* L) {
    static bool down[256];
    bool focus = gameFocused() && !gOverlayOpen;
    lua_getfield(L, LUA_REGISTRYINDEX, kKeys);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        int vk = int(lua_tointeger(L, -2)) & 0xff;
        bool now = keyDown(vk);
        bool press = now && !down[vk] && focus;
        down[vk] = now;
        if (press) {
            lua_pushvalue(L, -1);
            call(L, 0, 0);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    pollBinds(L, focus);
}

// True once a server world looks like the layout we know: Health is registered and entities exist. Checked every
// tick until it passes, because the first ticks of a world can come before it is populated.
bool worldReady(uintptr_t world) {
    static uintptr_t seen;
    static bool ok, warned;
    static ULONGLONG last;
    static int64_t lastSim;
    ULONGLONG now = GetTickCount64();
    int64_t sim = ecs::simTime(world);
    // A new session: another world, or ticks resuming after a pause (main menu, then a load). A reloaded world
    // often gets the same address, so the pause matters. A death and respawn pauses too, but there the session's
    // sim time goes on; a load starts it again.
    if (world != seen || now - last > 2000) {
        bool fresh = world != seen || sim < lastSim;
        gNewWorld |= fresh;
        gSession += fresh;
        seen = world;
        ok = warned = false;
        resetAdmin();
    }
    last = now;
    lastSim = sim;
    if (ok) return true;
    refl::Type hp = refl::byName("Health");
    ok = hp && ecs::compIndex(world, hp.hash()) >= 0 && ecs::entityCount(world) > 0;
    char line[160];
    if (ok) snprintf(line, sizeof line, LV_OK "Server World: %p: %u Entities, Sim Time: %.1fs.", reinterpret_cast<void*>(world),
                     ecs::entityCount(world), double(sim) / 1e9);
    else snprintf(line, sizeof line, LV_WARN "Server World: %p Not recognized or ready yet: Waiting", reinterpret_cast<void*>(world));
    if (ok || !std::exchange(warned, true)) gSink(line);
    return ok;
}

std::vector<std::string> takeQueue() {
    std::lock_guard lock(gQueueMx);
    return std::exchange(gQueue, {});
}

void startState() {
    gL = rt::open();
    gSeenOpen = false;  // an overlay that is already open gets onOverlayOpen at the next tick
    loadBindings();
    loadMods(gL);
    gLoaded = true;
    runCallbacks(gL, kOnInit, "onInit");
}

// onShutdown runs first, so mods can still save their files.
void closeState() {
    gHookShutdown = true;
    stopHooks(gL);
    runCallbacks(gL, kOnShutdown, "onShutdown");
    mset::dropAll(gSink);  // after onShutdown, so what a mod sets there is saved too
    lua_close(std::exchange(gL, nullptr));
    gLoaded = false;
    gHookShutdown = false;
}

// onWorldLeave, without the game: the world is going away, or already gone when its leave went unseen.
void leaveWorld() {
    hk::discardPending();
    rules::setPlayer(0);
    gInWorld = false;
    gSink(LV_LOG "Left the world");
    gLeaving = true;
    runCallbacks(gL, kOnLeave, "onWorldLeave");
    gLeaving = false;
}

// A new session's first ready tick, with the game. A world whose leave went unseen is left first.
void enterWorld() {
    if (gInWorld) leaveWorld();
    hk::discardPending();
    gInWorld = true;
    gSink(LV_LOG "Entered a world");
    runCallbacks(gL, kOnEnter, "onWorldEnter");
}

// onOverlayOpen or onOverlayClose when the overlay changed since the last tick. On the tick, not the render thread, so
// handlers can reach the game in a world.
void overlayEvents() {
    bool open = gOverlayOpen;
    if (open != std::exchange(gSeenOpen, open))
        runCallbacks(gL, open ? kOnOpen : kOnClose, open ? "onOverlayOpen" : "onOverlayClose");
}

void global(lua_State* L, const char* name, const luaL_Reg* fns) {
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setglobal(L, name);
}

int frozenWrite(lua_State* L) {
    return luaL_error(L, "%s is shared by every mod and can't be changed", lua_tostring(L, lua_upvalueindex(1)));
}

// pairs() on a frozen table lists the real one. Upvalues: next, the real table.
int frozenPairs(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_pushnil(L);
    return 3;
}

// Puts the global table `name` behind a read-only proxy. Every mod shares it, so one mod's assignment (Ecs.player =
// nil) would change it for all of them. rawset still gets past it.
void freeze(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    int real = lua_gettop(L);
    lua_newtable(L);
    lua_createtable(L, 0, 4);
    lua_pushvalue(L, real);
    lua_setfield(L, -2, "__index");
    lua_pushstring(L, name);
    lua_pushcclosure(L, frozenWrite, 1);
    lua_setfield(L, -2, "__newindex");
    lua_getglobal(L, "next");
    lua_pushvalue(L, real);
    lua_pushcclosure(L, frozenPairs, 2);
    lua_setfield(L, -2, "__pairs");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");  // getmetatable gives false, setmetatable fails
    lua_setmetatable(L, -2);
    lua_setglobal(L, name);
    lua_pop(L, 1);
}

int getVersion(lua_State* L) {
    lua_pushliteral(L, RUNTIME_VERSION);
    return 1;
}

// GetGameBuild(): the running exe's build (its link time stamp), as Status shows it, so a mod can pick per build.
int getGameBuild(lua_State* L) {
    lua_pushinteger(L, gBuild);
    return 1;
}

// Features(): {id = {compatible, available, reason, label}}, a new table on every call.
int luaFeatures(lua_State* L) {
    auto all = rt::features();
    lua_createtable(L, 0, static_cast<int>(all.size()));
    for (const rt::Feature& f : all) {
        lua_createtable(L, 0, 4);
        lua_pushboolean(L, f.compatible);
        lua_setfield(L, -2, "compatible");
        lua_pushboolean(L, f.available);
        lua_setfield(L, -2, "available");
        lua_pushstring(L, f.reason.c_str());
        lua_setfield(L, -2, "reason");
        lua_pushstring(L, f.label.c_str());
        lua_setfield(L, -2, "label");
        lua_setfield(L, -2, f.id.c_str());
    }
    return 1;
}

// The features main.cpp checks, in the Compatibility window's order. An id is never renamed or dropped once shipped:
// a removed feature stays here, incompatible, with the reason "removed in x.y". world: usable only in a world you host.
struct FeatDef {
    const char* id;
    const char* label;
    bool world;
};
constexpr FeatDef kFeats[] = {
    {"resources", "Resources (Res)", false},
    {"english_names", "English Names (Res.text)", false},
    {"game_options", "Game Options (GameOptions)", false},
    {"field_of_view", "Live Field of View", false},
    {"systems", "Systems", false},
    {"events", "Events (Observe, Events.post)", true},
    {"admin", "Admin Commands (Admin.*)", true},
    {"admin_give", "Admin.give", true},
    {"admin_settings", "Game Settings (Admin.settings)", true},
    {"overlay", "Overlay (Draw, ImGui)", false},
};
struct FeatState {
    bool set = false, compatible = false, ready = false;
    std::string reason;
};
std::mutex gFeatMx;
FeatState gFeats[std::size(kFeats)];

// Key names, both ways. Punctuation is named after the US layout, in words: bindings.ini values lose surrounding
// quotes, and a name may not contain '+', which splits combos.
constexpr std::pair<const char*, int> kNamedKeys[] = {
    {"`", VK_OEM_3},         {"Insert", VK_INSERT},    {"Home", VK_HOME},          {"End", VK_END},
    {"Delete", VK_DELETE},   {"PageUp", VK_PRIOR},     {"PageDown", VK_NEXT},      {"Pause", VK_PAUSE},
    {"Space", VK_SPACE},     {"Tab", VK_TAB},          {"Enter", VK_RETURN},       {"Backspace", VK_BACK},
    {"CapsLock", VK_CAPITAL}, {"Left", VK_LEFT},       {"Up", VK_UP},              {"Right", VK_RIGHT},
    {"Down", VK_DOWN},       {"PrintScreen", VK_SNAPSHOT}, {"ScrollLock", VK_SCROLL}, {"NumLock", VK_NUMLOCK},
    {"Num0", VK_NUMPAD0},    {"Num1", VK_NUMPAD1},     {"Num2", VK_NUMPAD2},       {"Num3", VK_NUMPAD3},
    {"Num4", VK_NUMPAD4},    {"Num5", VK_NUMPAD5},     {"Num6", VK_NUMPAD6},       {"Num7", VK_NUMPAD7},
    {"Num8", VK_NUMPAD8},    {"Num9", VK_NUMPAD9},     {"NumMultiply", VK_MULTIPLY}, {"NumAdd", VK_ADD},
    {"NumSubtract", VK_SUBTRACT}, {"NumDecimal", VK_DECIMAL}, {"NumDivide", VK_DIVIDE}, {"Semicolon", VK_OEM_1},
    {"Equals", VK_OEM_PLUS}, {"Comma", VK_OEM_COMMA},  {"Minus", VK_OEM_MINUS},    {"Period", VK_OEM_PERIOD},
    {"Slash", VK_OEM_2},     {"LeftBracket", VK_OEM_4}, {"Backslash", VK_OEM_5},   {"RightBracket", VK_OEM_6},
    {"Quote", VK_OEM_7},     {"<>", VK_OEM_102}};

}  // namespace

void rt::configure(const std::string& modsDir, Sink sink) {
    gModsDir = modsDir;
    gSink = sink;
}

// kFnMod: function -> mod, with weak keys, so a dropped function can still be collected.
void makeFnModTable(lua_State* L) {
    lua_newtable(L);
    lua_newtable(L);
    lua_pushliteral(L, "k");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, kFnMod);
}

lua_State* rt::open() {
    lua_State* L = luaL_newstate();
    openSafeLibs(L);
    makeFnModTable(L);
    luaL_newmetatable(L, kRefMt);
    const luaL_Reg meta[] = {{"__index", refIndex}, {"__newindex", refNewIndex}, {"__len", refLen},
                             {"__tostring", refToString}, {nullptr, nullptr}};
    luaL_setfuncs(L, meta, 0);
    lua_pop(L, 1);
    const luaL_Reg ecsFns[] = {{"get", ecsGet},       {"player", ecsPlayer}, {"each", ecsEach},
                               {"fields", ecsFields}, {"types", ecsTypes},   {nullptr, nullptr}};
    global(L, "Ecs", ecsFns);
    const luaL_Reg resFns[] = {{"get", resGet},     {"each", resEach}, {"find", resFind},
                               {"types", resTypes}, {"text", resText}, {nullptr, nullptr}};
    global(L, "Res", resFns);
    const luaL_Reg optsFns[] = {{"machine", optsMachine}, {"user", optsUser}, {"dump", optsDump}, {nullptr, nullptr}};
    global(L, "GameOptions", optsFns);
    const luaL_Reg sysFns[] = {{"list", systemsList},       {"enable", systemsEnable}, {"enabled", systemsEnabled},
                               {"profile", systemsProfile}, {"stats", systemsStats},   {nullptr, nullptr}};
    global(L, "Systems", sysFns);
    const luaL_Reg typesFns[] = {{"fields", typesFields}, {"enum", typesEnum}, {"new", typesNew},
                                 {"copy", typesCopy},     {nullptr, nullptr}};
    global(L, "Types", typesFns);
    const luaL_Reg tickFns[] = {{"add", tickAdd}, {nullptr, nullptr}}, keysFns[] = {{"on", keysOn}, {nullptr, nullptr}},
                   eventsFns[] = {{"post", eventsPost}, {nullptr, nullptr}},
                   drawFns[] = {{"add", drawAdd}, {nullptr, nullptr}},
                   overlayFns[] = {{"isOpen", overlayIsOpen}, {nullptr, nullptr}};
    global(L, "Tick", tickFns);
    global(L, "Keys", keysFns);
    global(L, "Events", eventsFns);
    global(L, "Draw", drawFns);
    global(L, "Overlay", overlayFns);
    const luaL_Reg adminFns[] = {{"send", adminSend},         {"setTime", adminSetTime},
                                 {"teleport", adminTeleport}, {"setLevel", adminSetLevel},
                                 {"setFlameLevel", adminSetFlameLevel}, {"settings", adminSettings},
                                 {"changeSettings", adminChangeSettings}, {"spawn", adminSpawn}, {"give", adminGive},
                                 {nullptr, nullptr}};
    global(L, "Admin", adminFns);
    rt::openImGui(L);
    rt::openJson(L);
    lua_register(L, "Observe", observe);
    lua_register(L, "print", luaPrint);
    lua_register(L, "reload", luaReload);
    lua_register(L, "GetMod", getMod);
    lua_register(L, "registerForEvent", registerForEvent);
    lua_register(L, "GetVersion", getVersion);
    lua_register(L, "GetGameBuild", getGameBuild);
    lua_register(L, "Features", luaFeatures);
    for (const char* reg : {kTicks, kDraws, kKeys, kObservers, kMods, kBinds, kOnInit, kOnOpen, kOnClose, kOnShutdown,
                            kOnEnter, kOnLeave, kOnChange}) {
        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, reg);
    }
    for (const char* name : {"Ecs", "Res", "GameOptions", "Systems", "Types", "Tick", "Keys", "Events", "Draw", "Overlay", "Admin",
                             "ImGui", "ImGuiCond", "ImGuiWindowFlags", "json"})
        freeze(L, name);
    gHasDraw = false;
    return L;
}

void rt::setWorld(uintptr_t world) { gWorld = world; }

void rt::exec(lua_State* L, const std::string& line) {
    struct ConsoleScope {
        bool prior = std::exchange(gInConsole, true);
        ~ConsoleScope() { gInConsole = prior; }
    } scope;
    int top = lua_gettop(L);
    std::string expr = "return " + line;  // expressions print their value, like the standalone lua REPL
    if (luaL_loadbuffer(L, expr.data(), expr.size(), "=console") != LUA_OK) {
        lua_pop(L, 1);
        if (luaL_loadbuffer(L, line.data(), line.size(), "=console") != LUA_OK) {
            gSink((LV_BAD + std::string(lua_tostring(L, -1))).c_str());
            lua_settop(L, top);
            return;
        }
    }
    if (call(L, 0, LUA_MULTRET) && lua_gettop(L) > top) {
        lua_getglobal(L, "print");
        lua_insert(L, top + 1);
        call(L, lua_gettop(L) - top - 1, 0);
    }
    lua_settop(L, top);
}

namespace {

// One tick of the loaded mods: queued lines, overlay events, hook handlers, Tick callbacks, keys, admin commands,
// reload(). world is 0 on the main thread, outside a world. dt is timed here, so it stays true when the other thread
// takes over.
// The host player's entity, for rules comparing with "player": the last one is kept while it still has PlayerInput, so
// the entity map is walked only when it changes. 0 outside a world.
void updatePlayer(uintptr_t world) {
    static uint32_t last;
    refl::Type pi = refl::byName("PlayerInput");
    int idx = world && pi ? ecs::compIndex(world, pi.hash()) : -1;
    if (idx < 0) last = 0;
    else if (!last || !ecs::component(world, last, idx)) last = ecs::firstWith(world, idx);
    rules::setPlayer(last);
}

void tickMods(uintptr_t world) {
    static auto last = std::chrono::steady_clock::now();
    updatePlayer(world);
    auto now = std::chrono::steady_clock::now();
    gDt = std::chrono::duration<double>(now - std::exchange(last, now)).count();
    if (world) checkSent(world);
    for (const std::string& line : takeQueue()) rt::exec(gL, line);
    overlayEvents();
    settingsEdits(gL);
    hk::flushLogs();
    if (!gReload) dispatchHooks(gL);
    runCallbacks(gL, kTicks, "tick");
    pollKeys(gL);
    if (world) armAdmin(world);
    if (gReload.exchange(false)) {
        gSink("Reloading mods");
        closeState();
        wlog::leaveAll(gSink);  // after onShutdown, so a mod's own restore comes first
        startState();
        if (gInWorld) runCallbacks(gL, kOnEnter, "onWorldEnter");  // set up as on entering, in either case
    }
    mset::save(false, gSink);
}

// guarded's SEH half: __try can't share a function with C++ objects to destroy or a C++ try.
bool noAccessViolation(void (*fn)(uintptr_t), uintptr_t arg) {
    __try {
        fn(arg);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// Runs Lua work under gLuaMx, which the caller holds. An access violation (a game update can move what a mod reads)
// is caught here, inside the lock, so the caller still unlocks: caught outside it, /EHsc would skip the unlock and
// freeze the next thread that waits for Lua. The Lua state is unusable afterwards, since Lua's error handling still
// points into the skipped frames, so no Lua runs again.
void guarded(void (*fn)(uintptr_t), uintptr_t arg, const char* what) {
    try {
        if (noAccessViolation(fn, arg)) return;
        gBroken = true;
        gLoaded = gHasDraw = false;
        stopHooks(nullptr);
        gMod.clear();
        if (!gClosing) wlog::leaveAll(gSink);  // C++ only: no Lua runs again
        mset::dropAll(gSink);
        gSink(LV_BAD "Lua: Access violation, mods off until the game restarts");
    } catch (...) {
        gSink((LV_BAD "Internal error in " + std::string(what)).c_str());
    }
}

}  // namespace

void rt::tick(uintptr_t world) {
    std::lock_guard lock(gLuaMx);
    ReadWindow rw(world);
    gReady = 0;
    if (gBroken || gClosing) return;
    guarded([](uintptr_t w) {
        if (!worldReady(w)) return;
        gReady = gWorld = w;
        if (!gL) startState();  // without the main thread's frame hook, mods load in the first world
        if (std::exchange(gNewWorld, false)) enterWorld();
        if (gInWorld) tickMods(w);  // after a leave, the world ticks on while it shuts down: no mods there
    }, world, "the Lua tick");
    gWorld = 0;
    gLeaving = false;
}

// The main thread, every frame (the options update): mods load here once the game is ready, and run here while no
// world you host runs them.
void rt::frame() {
    if (gInWorld || gClosing || gBroken) return;
    std::lock_guard lock(gLuaMx);
    if (gInWorld || gClosing || gBroken) return;  // a world started, or the game began closing, while this waited
    guarded([](uintptr_t) {
        if (!gL) startState();
        tickMods(0);
    }, 0, "the Lua frame");
}

// The player left the world: onWorldLeave runs now, without the game, and the main thread runs the mods again.
void rt::worldLeft() {
    std::lock_guard lock(gLuaMx);
    resetAdmin();
    if (!gInWorld || gBroken) return;
    guarded([](uintptr_t) { leaveWorld(); }, 0, "onWorldLeave");
    gLeaving = false;
}

// The game started closing: onWorldLeave if still in a world, then onShutdown, both without the game. No mods after.
void rt::gameClosing() {
    std::lock_guard lock(gLuaMx);
    gClosing = true;
    resetAdmin();
    if (!gL || gBroken) {
        gSink("Game Closing");  // every clean exit logs a line starting so: the next start's crash check reads it
        return;
    }
    guarded([](uintptr_t) {
        if (gInWorld) leaveWorld();
        gSink("Game Closing: Unloading mods");
        closeState();
    }, 0, "onShutdown");
    gLeaving = false;
}

void rt::afterTick(uintptr_t world) {
    std::lock_guard lock(gLuaMx);
    ReadWindow rw(world);
    if (!gL || gBroken || !gInWorld || world != gReady) return;
    gWorld = world;
    guarded([](uintptr_t w) { dispatchEvents(gL, w); }, world, "event dispatch");
    gWorld = 0;
}

uintptr_t rt::readyWorld(uint64_t& session) {
    session = gSession;
    return gInWorld && !gBroken ? gReady : 0;
}

uint32_t rt::jobStart() { return gReadGen.load(); }

void rt::jobEnd(uintptr_t world, uint32_t gen) {
    if (world == gReadWorld.load(std::memory_order_relaxed) && ((gen & 1) || gReadGen.load() != gen))
        gOverlaps.fetch_add(1, std::memory_order_relaxed);
}

uint64_t rt::overlaps() { return gOverlaps.load(std::memory_order_relaxed); }

void rt::queue(std::string line) {
    std::lock_guard lock(gQueueMx);
    gQueue.push_back(std::move(line));
}

// Draw callbacks, on the render thread inside the overlay's ImGui frame. Waits for the mods' tick or frame.
void rt::draw() {
    gLuaMx.lock();
    drawLocked();
}

// The overlay's way in: takes the Lua lock only if no tick or frame holds it, so a present never waits for a mod.
bool rt::tryLockDraw() { return gLuaMx.try_lock(); }

// Draw callbacks under the lock tryLockDraw (or draw) took. Releases it.
void rt::drawLocked() {
    std::lock_guard lock(gLuaMx, std::adopt_lock);
    if (!gL || gBroken) return;
    gInDraw = true;
    guarded([](uintptr_t) { runCallbacks(gL, kDraws, "draw"); }, 0, "Draw");
    gInDraw = false;
    if (gBroken) rt::imguiRecover();  // the callback the access violation stopped may have left windows open
}

// overlay_selftest only: a helper thread holds the Lua lock, as a long tick would, and Draw counts as registered.
void rt::testHoldLua(bool on) {
    static std::thread holder;
    static std::atomic<bool> hold, held;
    if (on == holder.joinable()) return;
    if (!on) {
        hold = false;
        holder.join();
        gHasDraw = false;
        return;
    }
    gHasDraw = hold = true;
    held = false;
    holder = std::thread([] {
        std::lock_guard lock(gLuaMx);
        held = true;
        while (hold) Sleep(1);
    });
    while (!held) Sleep(1);
}

bool rt::inDraw() { return gInDraw; }
bool rt::hasDraw() { return gHasDraw; }
void rt::setOverlayOpen(bool open) { gOverlayOpen = open; }
void rt::requestReload() { gReload = true; }
void rt::setGameBuild(uint32_t build) { gBuild = build; }
bool rt::modsLoaded() { return gLoaded; }
bool rt::systemsAvailable() { return gSysOk; }
void rt::setAdminAvailable(bool ok) { gAdminOk = ok; }
void rt::setGiveAvailable(bool ok) { gGiveOk = ok; }
void rt::setSystemsAvailable(bool ok) { gSysOk = ok; }

void rt::setFeature(const char* id, bool compatible, std::string reason, bool ready) {
    std::lock_guard lock(gFeatMx);
    for (size_t i = 0; i < std::size(kFeats); i++)
        if (strcmp(kFeats[i].id, id) == 0) gFeats[i] = {true, compatible, ready, std::move(reason)};
}

std::vector<rt::Feature> rt::features() {
    bool tested = testedBuild(gBuild);
    std::vector<Feature> out{{"game_build", "Game Build " + std::to_string(gBuild),
                              tested ? "" : "Not tested with this version: Each feature checks itself", tested, tested}};
    {
        std::lock_guard lock(gFeatMx);
        for (size_t i = 0; i < std::size(kFeats); i++) {
            const FeatState& s = gFeats[i];
            Feature f{kFeats[i].id, kFeats[i].label, s.set ? s.reason : "Not checked yet", s.compatible,
                      s.compatible && s.ready};
            if (f.available && kFeats[i].world && !gInWorld) f.available = false, f.reason = "Not in a world you host";
            out.push_back(std::move(f));
        }
    }
    auto checked = contracts::checked();
    for (const contracts::Entry& e : contracts::catalog()) {
        auto s = std::find_if(checked.begin(), checked.end(), [&](const contracts::Status& c) { return c.name == e.name; });
        bool ok = s != checked.end() && s->fn;
        out.push_back({e.name, std::string("Rule Target ") + e.name, ok ? "" : s != checked.end() ? s->why : "Not checked yet",
                       ok, ok});
    }
    return out;
}

void rt::setSettingsSlot(int index, uint32_t count) {
    gSettingsIdx = index;
    gSettingsCount = count;
    gSettings = 0;
    gSettingsWarned = false;
}

// Inside the world tick, right before system sys and after the game rebuilt PlayerInput: writes each of sys's armed
// commands (keeping the live version) and moves its counter off the version. Costs two atomic loads when idle.
void rt::applyAdmin(void* job, int sys) {
    if (sys == 0 && job && gSettingsIdx >= 0 && !gSettings) captureSettings(job);
    if (!gHasArmed) return;
    std::vector<AdminMsg> armed, sent;
    uintptr_t world;
    {
        std::lock_guard lock(gAdminMx);
        auto mine = std::stable_partition(gArmed.begin(), gArmed.end(), [&](const AdminMsg& m) { return m.cmd->sys != sys; });
        armed.assign(std::make_move_iterator(mine), std::make_move_iterator(gArmed.end()));
        gArmed.erase(mine, gArmed.end());
        gHasArmed = !gArmed.empty();
        world = gAdminWorld;
    }
    uint8_t *in, *con;
    uint32_t player = playerInputs(world, in, con);
    for (AdminMsg& m : armed) {
        AdminSlot s;
        if (!player || !adminSlot(*m.cmd, s) || !mergeSettings(m, s)) continue;
        uint8_t* act = in + s.off;
        memcpy(m.data.data() + s.version, act + s.version, 4);
        memcpy(act, m.data.data(), m.data.size());
        patchAction(world, s.action, act, player);
        uint32_t v;
        memcpy(&v, act + s.version, 4);
        v++;
        memcpy(con + s.consumed, &v, 4);
        sent.push_back(std::move(m));
    }
    std::lock_guard lock(gAdminMx);
    for (AdminMsg& m : sent) gSent.push_back(std::move(m));
}

// Key names shared by Keys.on and the overlay key setting: F1-F24, A-Z, 0-9, a few named keys, or a 0xNN code.
int rt::keyFromName(const char* n) {
    for (const auto& [name, vk] : kNamedKeys)
        if (_stricmp(n, name) == 0) return vk;
    char* end = nullptr;
    if (n[0] == '0' && (n[1] == 'x' || n[1] == 'X')) {
        long v = strtol(n + 2, &end, 16);
        return n[2] && !*end && v > 0 && v < 256 ? int(v) : 0;
    }
    if ((n[0] == 'F' || n[0] == 'f') && n[1]) {
        long k = strtol(n + 1, &end, 10);
        return !*end && k >= 1 && k <= 24 ? VK_F1 + int(k) - 1 : 0;
    }
    char c = char(toupper(static_cast<unsigned char>(n[0])));
    return n[0] && !n[1] && ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : 0;
}

std::string rt::keyName(int vk) {
    for (const auto& [name, k] : kNamedKeys)
        if (k == vk) return name;
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
    char hex[8];
    snprintf(hex, sizeof hex, "0x%02X", vk & 0xff);
    return hex;
}

int rt::heldMods() {
    return (keyDown(VK_CONTROL) ? ModCtrl : 0) | (keyDown(VK_SHIFT) ? ModShift : 0) | (keyDown(VK_MENU) ? ModAlt : 0);
}

std::vector<rt::BindInfo> rt::bindings(int& dead) {
    std::lock_guard lock(gBindMx);
    std::vector<BindInfo> rows;
    for (const Binding& b : gBinds)
        rows.push_back({b.key, b.mod, b.label, b.vk ? comboName(b.mods, b.vk) : "", b.input, b.vk});
    std::stable_sort(rows.begin(), rows.end(), [](const BindInfo& a, const BindInfo& b) { return a.mod < b.mod; });
    dead = static_cast<int>(std::count_if(gSaved.begin(), gSaved.end(), [](const auto& s) { return !findBind(s.first); }));
    return rows;
}

// Applies at once and writes bindings.ini, keeping the file's other lines. Logged, so set.log shows what was bound.
void rt::setBinding(const std::string& key, int mods, int vk) {
    std::string combo = vk ? comboName(mods, vk) : "";
    {
        std::lock_guard lock(gBindMx);
        if (Binding* b = findBind(key)) {
            b->mods = vk ? mods : 0;
            b->vk = vk;
        }
        if (vk) gSaved[key] = combo;
        else gSaved.erase(key);
        std::string path = bindsPath();
        if (!path.empty()) WritePrivateProfileStringA("bindings", key.c_str(), vk ? combo.c_str() : nullptr, path.c_str());
    }
    gSink((LV_OK "Binding " + key + ": " + (vk ? combo : "Unbound")).c_str());
}

void rt::removeDeadBindings() {
    std::lock_guard lock(gBindMx);
    std::string path = bindsPath();
    for (auto it = gSaved.begin(); it != gSaved.end();) {
        if (findBind(it->first)) {
            ++it;
            continue;
        }
        if (!path.empty()) WritePrivateProfileStringA("bindings", it->first.c_str(), nullptr, path.c_str());
        it = gSaved.erase(it);
    }
}
