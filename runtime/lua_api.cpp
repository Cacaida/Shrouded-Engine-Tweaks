#include "lua_api.h"

#include "game.h"
#include "hooks.h"
#include "level.h"
#include "version.h"

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

// Lua runs on the server thread (tick) or the main thread (frame), and on the render thread (Draw). gLuaMx keeps them
// apart; gInDraw marks the render-thread side, where game memory is off limits because the game mutates it
// concurrently.
std::mutex gLuaMx;
bool gInDraw;
bool gInConsole, gHookShutdown;
struct HookHandler {
    int sub, ref;
    std::string mod, name;
    std::vector<hk::Arg> args;
};
std::map<lua_Integer, HookHandler> gHooks;
lua_Integer gNextHook = 0;
std::atomic<bool> gBroken;  // an access violation stopped Lua work: the state is unusable, so no Lua runs again

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
uintptr_t gReady;  // server world whose pre-tick Lua work ran this tick; afterTick only dispatches for it
bool gNewWorld;    // a session started in another world (not a respawn): onWorldEnter at the next ready tick
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
void put(lua_State* L, const Ref& r, uint32_t off, const void* src, uint32_t n) {
    uint8_t* p = addr(L, r, off, n, true);
    if (!r.raw || r.gen || r.plain) {
        memcpy(p, src, n);
        return;
    }
    static bool told;
    uint32_t prot = 0;
    if (!res::write(p, src, n, prot)) luaL_error(L, "resource memory couldn't be made writable");
    if (!(prot & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !std::exchange(told, true)) {
        char line[96];
        snprintf(line, sizeof line, LV_OK "Resources: the game keeps them read-only (protect 0x%x), each edit lifts that", prot);
        gSink(line);
    }
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
    sys::setOn(s, lua_toboolean(L, 2));
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
    lua_pushcfunction(L, traceback);
    lua_insert(L, base);
    int rc = lua_pcall(L, nargs, nres, base);
    lua_remove(L, base);
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
    local load, loadfile, searchpath, path, error, type = load, loadfile, package.searchpath, package.path, error, type
    _G.load = function(chunk, name, _, ...) return load(chunk, name, "t", ...) end
    package.searchers[2] = function(mod)
        local file, err = searchpath(mod, path)
        if not file then return err end
        local fn, msg = loadfile(file, "t")
        if not fn then error(("error loading module '%s' from file '%s':\n\t%s"):format(mod, file, msg)) end
        return fn, file
    end
    return function(env, dir, mod)
        local cache = {}
        return function(name)
            if type(name) ~= "string" then error("bad argument #1 to 'require' (string expected)", 2) end
            if cache[name] == nil then
                local path, chunk = dir .. "\\" .. name:gsub("%.", "\\"), "@" .. mod .. "/" .. name:gsub("%.", "/")
                local fn, err = loadtext(path .. ".lua", chunk .. ".lua", env)
                if not fn and err:find("^cannot open") then
                    fn, err = loadtext(path .. "\\init.lua", chunk .. "/init.lua", env)
                end
                if not fn and err:find("^cannot open") then err = "module '" .. name .. "' not found in " .. dir end
                if not fn then error(err, 2) end
                local r = fn(name)
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

// A mod's io.open and io.lines: upvalue 1 is its folder, 2 the real function, called with the checked full path.
int modIo(lua_State* L) {
    std::string path = modPath(L, lua_tostring(L, lua_upvalueindex(1)));
    lua_pushstring(L, path.c_str());
    lua_replace(L, 1);
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

// One declared argument position: a kind name ("u32", "entity*", "cstr"...), or "<Type>*" for a copy of a plain
// reflected type.
hk::Arg hookArg(lua_State* L, int idx) {
    size_t len;
    const char* p = luaL_checklstring(L, idx, &len);
    std::string name(p, len);
    static const std::pair<const char*, hk::Kind> kinds[] = {
        {"skip", hk::Skip}, {"bool", hk::Bool}, {"u8", hk::U8}, {"u16", hk::U16}, {"u32", hk::U32},
        {"u64", hk::U64}, {"ptrsize", hk::U64}, {"i8", hk::I8}, {"i16", hk::I16}, {"i32", hk::I32},
        {"i64", hk::I64}, {"f32", hk::F32}, {"f64", hk::F64}, {"entity", hk::Entity},
        {"entity*", hk::EntityPtr}, {"cstr", hk::CStr}};
    for (const auto& k : kinds) if (name == k.first) return {k.second, {}};
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

// Hooks.observe{...}: hookRegister runs in a pcall so every refusal reaches the Hooks window's diagnostics. Upvalue 2
// of hookRegister is set when hk::subscribe already recorded the refusal, so it isn't counted twice.
int hookObserve(lua_State* L) {
    bool recorded = false;
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushlightuserdata(L, &recorded);
    lua_pushcclosure(L, hookRegister, 2);
    lua_pushvalue(L, 1);
    if (lua_pcall(L, 1, 1, 0) == LUA_OK) return 1;
    const char* err = lua_tostring(L, -1);
    if (!recorded)
        hk::refuse(std::string(lua_tostring(L, lua_upvalueindex(1))) + ": " + (err ? err : "hook registration failed"));
    return lua_error(L);
}

void removeHook(lua_State* L, lua_Integer id) {
    auto it = gHooks.find(id);
    if (it == gHooks.end()) return;
    hk::unsubscribe(it->second.sub);
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
            gSink((LV_BAD + h.mod + ": hook '" + h.name + "' removed after an error").c_str());
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
        if (hk::drain(h.sub, batch)) runs.push_back({id, h, std::exchange(batch, {})});
    for (const auto& r : runs) runHook(L, r.id, r.handler, r.batch);
}

// A mod's globals: its own table, falling back to the shared one, plus what has to know the mod: Keys.hotkey and
// Keys.input, io inside its folder, and require from its folder.
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
    for (const auto& f : {luaL_Reg{"observe", hookObserve}, luaL_Reg{"remove", hookRemove}}) {
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
        lua_pushcclosure(L, modIo, 2);
        lua_setfield(L, -3, fn);
    }
    lua_pop(L, 1);
    lua_setfield(L, env, "io");
    lua_getfield(L, LUA_REGISTRYINDEX, kRequire);
    lua_pushvalue(L, env);
    lua_pushstring(L, dir.c_str());
    lua_pushstring(L, name.c_str());
    lua_call(L, 3, 1);
    lua_setfield(L, env, "require");
}

// Runs mods/<name>/init.lua for every mod folder, each in its own environment. A failing mod is reported and skipped.
// What init.lua returns is kept for GetMod (true if it returns nothing).
void loadMods(lua_State* L) {
    int loaded = 0, failed = 0;
    lua_getfield(L, LUA_REGISTRYINDEX, kMods);
    int mods = lua_gettop(L);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((gModsDir + "\\*").c_str(), &fd);
    for (bool more = h != INVALID_HANDLE_VALUE; more; more = FindNextFileA(h, &fd) != 0) {
        std::string name = fd.cFileName, init = gModsDir + "\\" + name + "\\init.lua";
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name[0] == '.') continue;
        if (GetFileAttributesA(init.c_str()) == INVALID_FILE_ATTRIBUTES) {
            gSink((LV_WARN "Mod skipped: " + name + " (no init.lua)").c_str());
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
            removeModHooks(L, name);
        }
        lua_settop(L, mods);
        gSink(((ok ? LV_OK "Mod loaded: " : LV_BAD "Mod failed: ") + name).c_str());
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

// The mod the function at idx comes from, for error lines: its chunk name up to the first / ("@hello/init.lua" is
// hello), "console" for a typed line, "load" for a string chunk.
std::string modOf(lua_State* L, int idx) {
    lua_Debug ar;
    lua_pushvalue(L, idx);
    lua_getinfo(L, ">S", &ar);
    std::string_view s = ar.source ? ar.source : "";
    if (!s.starts_with('@') && !s.starts_with('=')) return "load";
    s.remove_prefix(1);
    return std::string(s.substr(0, s.find('/')));
}

// Drops callback i of the list on top of the stack after an error, naming its mod: "hello: tick callback removed".
void dropCallback(lua_State* L, lua_Integer i, const char* what) {
    lua_rawgeti(L, -1, i);
    gSink((LV_BAD + modOf(L, -1) + ": " + what + " removed after an error").c_str());
    lua_pop(L, 1);
    lua_pushboolean(L, 0);
    lua_rawseti(L, -2, i);
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
        if (!call(L, tick ? 1 : 0, 0)) {
            if (draw) rt::imguiRecover();
            dropCallback(L, i, (std::string(what) + " callback").c_str());
        }
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
        gSink((LV_BAD "Admin command " + std::string(m.cmd->input) + ": not run by the game").c_str());
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
        gSink(LV_OK "Game settings: ready");
    } else if (!gSettingsWarned.exchange(true)) {
        gSink((LV_WARN "Game settings: not recognized yet (" + std::string(why) + "), still trying").c_str());
    }
}

// Game settings: the current settings, overlaid with only the bytes the caller set. While the settings are unknown the
// command is dropped (and logged) rather than zeroing every setting it doesn't name.
bool mergeSettings(AdminMsg& m, const AdminSlot& s) {
    refl::Field gs;
    if (!m.cmd->merge) return true;
    auto cur = reinterpret_cast<const uint8_t*>(gSettings.load());
    if (!cur || !refl::field(s.action, "gameSettings", gs)) {
        gSink((LV_BAD "Admin command " + std::string(m.cmd->input) + ": not run, game settings unknown").c_str());
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
        gNewWorld |= world != seen || sim < lastSim;
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
    if (ok) snprintf(line, sizeof line, LV_OK "server world %p: %u entities, sim time %.1f s", reinterpret_cast<void*>(world),
                     ecs::entityCount(world), double(sim) / 1e9);
    else snprintf(line, sizeof line, LV_WARN "server world %p not ready or not recognized: Lua waits", reinterpret_cast<void*>(world));
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
    lua_close(std::exchange(gL, nullptr));
    gLoaded = false;
    gHookShutdown = false;
}

// onWorldLeave, without the game: the world is going away, or already gone when its leave went unseen.
void leaveWorld() {
    hk::discardPending();
    gInWorld = false;
    gSink("Left the world");
    gLeaving = true;
    runCallbacks(gL, kOnLeave, "onWorldLeave");
    gLeaving = false;
}

// A new session's first ready tick, with the game. A world whose leave went unseen is left first.
void enterWorld() {
    if (gInWorld) leaveWorld();
    hk::discardPending();
    gInWorld = true;
    gSink("Entered a world");
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

constexpr std::pair<const char*, int> kNamedKeys[] = {
    {"`", VK_OEM_3},       {"Insert", VK_INSERT}, {"Home", VK_HOME},     {"End", VK_END},
    {"Delete", VK_DELETE}, {"PageUp", VK_PRIOR},  {"PageDown", VK_NEXT}, {"Pause", VK_PAUSE}};

}  // namespace

void rt::configure(const std::string& modsDir, Sink sink) {
    gModsDir = modsDir;
    gSink = sink;
}

lua_State* rt::open() {
    lua_State* L = luaL_newstate();
    openSafeLibs(L);
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
    for (const char* reg : {kTicks, kDraws, kKeys, kObservers, kMods, kBinds, kOnInit, kOnOpen, kOnClose, kOnShutdown,
                            kOnEnter, kOnLeave}) {
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
void tickMods(uintptr_t world) {
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    gDt = std::chrono::duration<double>(now - std::exchange(last, now)).count();
    if (world) checkSent(world);
    for (const std::string& line : takeQueue()) rt::exec(gL, line);
    overlayEvents();
    hk::flushLogs();
    if (!gReload) dispatchHooks(gL);
    runCallbacks(gL, kTicks, "tick");
    pollKeys(gL);
    if (world) armAdmin(world);
    if (gReload.exchange(false)) {
        gSink("Reloading mods");
        closeState();
        startState();
        if (gInWorld) runCallbacks(gL, kOnEnter, "onWorldEnter");  // set up as on entering, in either case
    }
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
        gSink(LV_BAD "Lua: access violation, mods off until the game restarts");
    } catch (...) {
        gSink((LV_BAD "Internal error in " + std::string(what)).c_str());
    }
}

}  // namespace

void rt::tick(uintptr_t world) {
    std::lock_guard lock(gLuaMx);
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
    if (!gL || gBroken) return;
    guarded([](uintptr_t) {
        if (gInWorld) leaveWorld();
        gSink("Game closing: unloading mods");
        closeState();
    }, 0, "onShutdown");
    gLeaving = false;
}

void rt::afterTick(uintptr_t world) {
    std::lock_guard lock(gLuaMx);
    if (!gL || gBroken || !gInWorld || world != gReady) return;
    gWorld = world;
    guarded([](uintptr_t w) { dispatchEvents(gL, w); }, world, "event dispatch");
    gWorld = 0;
}

void rt::queue(std::string line) {
    std::lock_guard lock(gQueueMx);
    gQueue.push_back(std::move(line));
}

// Draw callbacks, on the render thread inside the overlay's ImGui frame. Waits for the mods' tick or frame.
void rt::draw() {
    std::lock_guard lock(gLuaMx);
    if (!gL || gBroken) return;
    gInDraw = true;
    guarded([](uintptr_t) { runCallbacks(gL, kDraws, "draw"); }, 0, "Draw");
    gInDraw = false;
    if (gBroken) rt::imguiRecover();  // the callback the access violation stopped may have left windows open
}

bool rt::inDraw() { return gInDraw; }
bool rt::hasDraw() { return gHasDraw; }
void rt::setOverlayOpen(bool open) { gOverlayOpen = open; }
void rt::requestReload() { gReload = true; }
bool rt::modsLoaded() { return gLoaded; }
bool rt::systemsAvailable() { return gSysOk; }
void rt::setAdminAvailable(bool ok) { gAdminOk = ok; }
void rt::setGiveAvailable(bool ok) { gGiveOk = ok; }
void rt::setSystemsAvailable(bool ok) { gSysOk = ok; }

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
    for (const Binding& b : gBinds) rows.push_back({b.key, b.mod, b.label, b.vk ? comboName(b.mods, b.vk) : "", b.input});
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
    gSink((LV_OK "Binding " + key + ": " + (vk ? combo : "unbound")).c_str());
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
