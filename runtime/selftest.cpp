// Offline self-test for the runtime, no game needed: reflection, patterns and the system scan against a copy of the
// exe, then ECS, events, resources, options, admin commands, native hooks and the whole Lua API against synthetic game
// memory built in this process.
// Usage: runtime_selftest <copy of enshrouded.exe>   (a copy, never the one in the game folder)
#include <process.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "game.h"
#include "hooks.h"
#include "level.h"
#include "lua_api.h"
#include "version.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "lauxlib.h"
#include "lua.h"

std::string gOut;  // everything the runtime printed

#define CHECK(c)                                                                                         \
    do {                                                                                                 \
        if (!(c)) {                                                                                      \
            printf("FAIL line %d: %s\nruntime output (tail):\n%s\n", __LINE__, #c,                       \
                   gOut.substr(gOut.size() > 800 ? gOut.size() - 800 : 0).c_str());                      \
            exit(1);                                                                                     \
        }                                                                                                \
    } while (0)

namespace {
void sink(const char* line) { (gOut += line) += '\n'; }

void checkReflection(uintptr_t base) {
    CHECK(refl::init(base));
    CHECK(refl::count() > 10000);
    refl::Type hp = refl::byName("Health");
    CHECK(hp && hp.size() == 0x28 && hp.inner().qname() == "keen::ecs::SInt32Attribute");
    refl::Field f;
    CHECK(refl::field(hp, "dataStorage", f) && f.offset == 8);
    refl::Type arr = f.type.resolved();
    CHECK(arr.prim() == refl::StaticArray && arr.size() == 32 && arr.inner().resolved().prim() == refl::S32);
    CHECK(refl::byHash(hp.hash()).p == hp.p && refl::byName("keen::ecs::Health").p == hp.p);
    for (size_t i = 0; i < refl::count(); i++) CHECK(refl::fnv(refl::at(i).qname()) == refl::at(i).hash());
    int values = 0;
    int64_t rare = -1;
    refl::forEnum(refl::byName("WeatherFrequency"), [&](std::string_view name, int64_t v) {
        values++;
        if (name == "Rare") rare = v;
    });
    CHECK(values == 4 && rare == 1);
    printf("reflection ok: %zu types\n", refl::count());
}

// keen hash map with 64 slots, inserted the way the game probes: home slot, then linear.
struct FakeMap {
    uint64_t hdr[11] = {};  // +8 bitmap, +0x10 capacity, +0x28 keys, +0x40 values, +0x54 count
    uint64_t bits = 0;
    uint32_t keys[64] = {};
    uint64_t vals[64] = {};
    uint16_t vals16[64] = {};

    explicit FakeMap(bool wide) {
        hdr[1] = reinterpret_cast<uint64_t>(&bits);
        hdr[2] = 64;
        hdr[5] = reinterpret_cast<uint64_t>(keys);
        hdr[8] = wide ? reinterpret_cast<uint64_t>(vals) : reinterpret_cast<uint64_t>(vals16);
    }
    void add(uint32_t key, uint64_t value, bool mixed) {
        uint32_t s = (mixed ? ecs::mix(key) : key) & 63;
        while (bits >> s & 1) s = (s + 1) & 63;
        bits |= 1ull << s;
        keys[s] = key;
        vals[s] = value;
        vals16[s] = static_cast<uint16_t>(value);
        reinterpret_cast<uint32_t*>(hdr)[0x54 / 4]++;
    }
};

uint8_t* bigZeroed(size_t n) {
    return static_cast<uint8_t*>(VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
}

// Synthetic server world: entity 1 has a Health (index 245) at row 2 of a chunk; another entity sits in entity 1's
// home slot (forces probing) and has no Health. The component map is keyed by Health's real reflection hash.
// Events: EntityDiedEvent (index 1) has 130 events over two pages, TestEvent1 (index 3) an empty one-page stream.
// world and schema are big because the event fields sit at world + 0xeb9e80 and schema + 0xe151b8.
struct FakeWorld {
    uint8_t* world = bigZeroed(0xec0000);
    uint8_t* schema = bigZeroed(0xe16000);
    FakeMap comps{false}, ents{true}, events{false};
    std::vector<uint8_t> arch1 = std::vector<uint8_t>(0x1484), arch2 = std::vector<uint8_t>(0x1484);
    std::vector<uint8_t> chunk = std::vector<uint8_t>(0x1800);  // Health at 0x40, then addPlayer()'s columns
    std::vector<uint8_t> streams = std::vector<uint8_t>(5 * 0x48), diedPages = std::vector<uint8_t>(2 * 0x800);
    std::vector<uint8_t> testPages = std::vector<uint8_t>(0x800), posterBuf = std::vector<uint8_t>(0x100);
    uint64_t rec1[7] = {}, rec2[7] = {}, poster[3] = {};
    uint8_t* hp() { return chunk.data() + 0x40 + 2 * 0x28; }
    uint32_t& posterUsed() { return *reinterpret_cast<uint32_t*>(&poster[2]); }
    uint8_t* streamAt(int i) { return streams.data() + i * 0x48; }

    ~FakeWorld() {
        VirtualFree(world, 0, MEM_RELEASE);
        VirtualFree(schema, 0, MEM_RELEASE);
    }

    // Stream {+0x14 size, +0x18 count, +0x30 pool, +0x40 first page, +0x42 last page}, pages linked at page + 2.
    void plantEvents() {
        events.add(refl::byName("EntityDiedEvent").hash(), 1, false);
        events.add(refl::byName("TestEvent1").hash(), 3, false);
        memcpy(schema + 0xe151b8, events.hdr, sizeof events.hdr);
        auto set = [](uint8_t* st, uint32_t size, uint32_t count, uint8_t* pool, uint16_t last) {
            memcpy(st + 0x14, &size, 4);
            memcpy(st + 0x18, &count, 4);
            memcpy(st + 0x30, &pool, 8);
            st[0x40] = 1;
            st[0x42] = static_cast<uint8_t>(last);
        };
        set(streamAt(1), 16, 130, diedPages.data(), 2);
        set(streamAt(3), 4, 0, testPages.data(), 1);
        diedPages[2] = 2;  // page 1 -> page 2
        for (uint32_t k = 0, id = 100; k < 130; k++, id++) {  // 127 events of 16 bytes fit before 0x7f8
            uint8_t* ev = diedPages.data() + (k < 127 ? 8 + k * 16 : 0x800 + 8 + (k - 127) * 16);
            memcpy(ev + 8, &id, 4);  // deadEntityId
        }
        *reinterpret_cast<uint64_t*>(world + 0xeb9e80) = reinterpret_cast<uint64_t>(streams.data());
        *reinterpret_cast<uint64_t*>(world + 0xeb9e88) = 5;
        poster[0] = reinterpret_cast<uint64_t>(posterBuf.data());
        poster[1] = posterBuf.size();
        *reinterpret_cast<uint64_t*>(world + 0xeb9ef0) = reinterpret_cast<uint64_t>(poster);
        *reinterpret_cast<int64_t*>(world + 0x4c0) = 33333333;  // tick length
        *reinterpret_cast<uint32_t*>(world + 0x4dc) = 3;        // tick index
    }

    FakeWorld() {
        comps.add(refl::byName("Health").hash(), 245, false);
        arch1[245 / 8] |= 1 << (245 % 8);
        reinterpret_cast<uint16_t*>(arch1.data() + 0x84)[245] = 0x40;
        reinterpret_cast<uint16_t*>(arch1.data() + 0xa84)[245] = 0x28;
        rec1[3] = reinterpret_cast<uint64_t>(arch1.data());
        rec1[4] = reinterpret_cast<uint64_t>(chunk.data());
        rec1[6] = 2;  // row
        rec2[3] = reinterpret_cast<uint64_t>(arch2.data());
        rec2[4] = reinterpret_cast<uint64_t>(chunk.data());
        uint32_t other = 2;
        while ((ecs::mix(other) & 63) != (ecs::mix(1) & 63)) other++;
        ents.add(other, reinterpret_cast<uint64_t>(rec2), true);
        ents.add(1, reinterpret_cast<uint64_t>(rec1), true);
        *reinterpret_cast<uint64_t*>(world) = reinterpret_cast<uint64_t>(schema);
        memcpy(world + 0x148, ents.hdr, sizeof ents.hdr);
        memcpy(schema + 0x88, comps.hdr, sizeof comps.hdr);
        *reinterpret_cast<int32_t*>(hp() + 8) = 60;  // dataStorage[0] = current HP
        plantEvents();
    }
    uintptr_t addr() { return reinterpret_cast<uintptr_t>(world); }

    // Entity 1 becomes the player: PlayerInput (index 300, column 0x200) and ServerConsumedPlayerInput (301, 0x1400).
    uint8_t* input() { return chunk.data() + 0x200 + 2 * 0x5c0; }
    uint8_t* consumed() { return chunk.data() + 0x1400 + 2 * 0xe0; }
    void addPlayer() {
        auto offs = reinterpret_cast<uint16_t*>(arch1.data() + 0x84), strides = reinterpret_cast<uint16_t*>(arch1.data() + 0xa84);
        for (auto [idx, name, off, stride] : {std::tuple{300, "PlayerInput", 0x200, 0x5c0},
                                              std::tuple{301, "ServerConsumedPlayerInput", 0x1400, 0xe0}}) {
            comps.add(refl::byName(name).hash(), idx, false);
            arch1[idx / 8] |= uint8_t(1 << (idx % 8));
            offs[idx] = uint16_t(off);
            strides[idx] = uint16_t(stride);
        }
        memcpy(schema + 0x88, comps.hdr, sizeof comps.hdr);
    }

    // Entity 1's CurrentTransform (index 302, column 0x16b0, 56 bytes a row) at x 10 m, y 2 m, z -3 m.
    void addTransform() {
        comps.add(refl::byName("CurrentTransform").hash(), 302, false);
        arch1[302 / 8] |= uint8_t(1 << (302 % 8));
        reinterpret_cast<uint16_t*>(arch1.data() + 0x84)[302] = 0x16b0;
        reinterpret_cast<uint16_t*>(arch1.data() + 0xa84)[302] = 56;
        memcpy(schema + 0x88, comps.hdr, sizeof comps.hdr);
        const int64_t pos[3] = {10ll << 32, 2ll << 32, -3ll << 32};
        memcpy(chunk.data() + 0x16b0 + 2 * 56, pos, sizeof pos);
    }
};

void checkEcs(FakeWorld& w) {
    uintptr_t world = w.addr();
    CHECK(ecs::compIndex(world, refl::byName("Health").hash()) == 245);
    CHECK(ecs::compIndex(world, refl::byName("Stamina").hash()) == -1);
    CHECK(ecs::component(world, 1, 245) == w.hp());
    CHECK(ecs::component(world, 1, 246) == nullptr);
    CHECK(ecs::component(world, 12345, 245) == nullptr);
    CHECK(ecs::firstWith(world, 245) == 1);
    CHECK(ecs::entityCount(world) == 2);
    printf("ecs lookups ok\n");
}

const char* kLuaChecks = R"(
    local hp = Ecs.get(1, "Health")
    assert(hp ~= nil, "Health missing")
    assert(hp.dataStorage[0] == 60, "read")
    hp.dataStorage[0] = 77
    assert(#hp.dataStorage == 8, "len")
    assert(tostring(hp.dataStorage):find("^{77, 0"), "tostring")
    assert(not pcall(function() hp.dataStorage[8] = 1 end), "index 8 accepted")
    assert(not pcall(function() hp.dataStorage[0] = "x" end), "string accepted")
    assert(not pcall(function() hp.dataStorage[0] = 2^40 end), "out-of-range int accepted")
    assert(not pcall(function() return hp.nope end), "unknown field accepted")
    assert(not pcall(function() hp.dataStorage = 1 end), "array assignment accepted")
    assert(Ecs.get(1, "Stamina") == nil and Ecs.get(9, "Health") == nil, "absent component")
    assert(Ecs.player() == nil, "player without PlayerInput")
    assert(io == nil and debug == nil and dofile == nil and loadfile == nil, "unsafe library open")
    assert(package.loaded.io == nil and not pcall(require, "io"), "io reachable through require")
    package.path = "C:\\Windows\\win.ini"
    assert(not select(2, pcall(require, "x")):find("win.ini", 1, true), "require follows package.path")
    assert(os.execute == nil and os.remove == nil and os.exit == nil and os.time ~= nil, "os not trimmed")
    assert(package.loadlib == nil and package.cpath == "" and package.searchers[3] == nil, "native loading open")
    assert(string.format and table.concat and math.floor and utf8.char and coroutine.wrap, "safe library missing")
    assert(string.dump == nil and select(2, load("\27Lua", nil, "b")):find("binary chunk"), "bytecode accepted")
    assert(load("return math")() == math and load("return x", "c", "t", {x = 5})() == 5, "load wrapper")
    assert(not pcall(function() Ecs.player = nil end) and type(Ecs.player) == "function", "Ecs writable")
    local names = 0
    for _ in pairs(Ecs) do names = names + 1 end
    assert(names == 5 and getmetatable(Ecs) == false, "frozen Ecs: pairs or metatable")
    assert(select(2, pcall(function() json.x = 1 end)):find("json is shared by every mod"), "json writable")
)" R"lua(
    local j = json.encode({b = {1, 2.5, "x\n\"\1"}, a = true, c = {}, d = {k = -3}})
    assert(j == '{"a":true,"b":[1,2.5,"x\\n\\"\\u0001"],"c":[],"d":{"k":-3}}', j)
    assert(json.encode(0.1) == "0.1" and json.decode(json.encode(1 / 3)) == 1 / 3, "json floats")
    local d = json.decode(' {"a": [1, 2.5e1, null, -0.5], "s": "\\u00e9\\ud83d\\ude00\\/\\t", "o": {}} ')
    assert(math.type(d.a[1]) == "integer" and d.a[2] == 25.0 and d.a[3] == nil and d.a[4] == -0.5, "json numbers")
    assert(d.s == "\u{e9}\u{1F600}/\t" and next(d.o) == nil, "json strings")
    local cyc = {}
    cyc[1] = cyc
    for _, bad in ipairs({cyc, {print}, {1, nil, 3}, {0 / 0}, {1, x = 2}, {[true] = 1}}) do
        assert(not pcall(json.encode, bad), "json encoded a bad value")
    end
    for _, bad in ipairs({"", "{", "[1,]", '{"a" 1}', "tru", '"\\x"', "[1] x", '"\\ud800"', "-"}) do
        assert(not pcall(json.decode, bad), "json decoded " .. bad)
    end
    assert(select(2, pcall(json.decode, "[1,]")) == "json: unexpected character at position 4", "json error position")
    local seen = {}
    for id, hp in Ecs.each("Health") do seen[#seen + 1] = id .. ":" .. hp.dataStorage[0] end
    assert(#seen == 1 and seen[1] == "1:77", "Ecs.each Health")
    for _ in Ecs.each("Health", "Stamina") do error("Stamina isn't in this world") end
    assert(not pcall(Ecs.each, "Nope") and not pcall(Ecs.each), "Ecs.each took a bad call")
)lua";

void checkLua(FakeWorld& w) {
    lua_State* L = rt::open();
    rt::setWorld(w.addr());
    if (luaL_dostring(L, kLuaChecks) != LUA_OK) {
        printf("FAIL lua: %s\n", lua_tostring(L, -1));
        exit(1);
    }
    CHECK(*reinterpret_cast<int32_t*>(w.hp() + 8) == 77);
    rt::exec(L, "1 + 1");
    CHECK(gOut.find("2\n") != std::string::npos);
    rt::exec(L, "print('version', GetVersion())");
    CHECK(gOut.find("version\t" RUNTIME_VERSION "\n") != std::string::npos);
    rt::setWorld(0);
    lua_close(L);
    printf("lua api ok\n");
}

const char* kTypesChecks = R"(
    local aa = Types.enum("RenderAntiAliasingSetting")
    assert(aa.None == 0 and aa.Dlss == 4, "enum")
    local at
    for _, f in ipairs(Types.fields("Health")) do if f.name == "dataStorage" then at = f.offset end end
    assert(at == 8, "fields")
    local hp = Ecs.get(1, "Health")
    local before, saved = hp.dataStorage[0], Types.copy(hp)
    hp.dataStorage[0] = before + 1
    hp.dataStorage = saved.dataStorage
    assert(hp.dataStorage[0] == before, "copied back")
    local a = Types.new("Health").dataStorage
    collectgarbage() collectgarbage()
    a[0] = 3
    assert(a[0] == 3 and a[1] == 0, "owned value collected")
    hp.dataStorage = a
    assert(hp.dataStorage[0] == 3, "owned value into the game")
    hp.dataStorage = saved.dataStorage
    assert(not pcall(function() hp.dataStorage = 1 end), "number into an array")
    assert(not pcall(Types.new, "ItemInfo") and not pcall(Types.new, "Health", {nope = 1}), "bad new")
    assert(not pcall(Types.new, "Settings") and not pcall(Types.enum, "Health"), "bad name")
    local m = Types.new("keen::g38_options::pc::machine::Settings", {sound = {muteInBackground = false}})
    assert(m.graphics.render.antiAliasing == 4 and m.graphics.renderScale == 1.0, "defaults")
    assert(m.sound.muteInBackground == false and m.graphics.vsync == true, "values over defaults")
    assert(not pcall(function() hp.dataStorage = m.graphics end), "another type")
)";

// Types: lenient names, fixed-size values, the game's defaults, and values Lua owns.
void checkTypes(FakeWorld& w) {
    CHECK(refl::byName("keen::g38_options::pc::machine::Settings").defaults());
    CHECK(!refl::byName("keen::ds::g38_options::pc::machine::Settings").defaults());
    CHECK(refl::plain(refl::byName("Health")) && !refl::plain(refl::lookup("ItemInfo")) && !refl::plain(opts::userType()));
    CHECK(refl::lookup("RenderAntiAliasingSetting").qname() == "keen::pc::RenderAntiAliasingSetting");
    CHECK(refl::lookup("Health").p == refl::byName("Health").p && !refl::lookup("Settings") && !refl::lookup("Nope"));
    CHECK(refl::unwrap(refl::lookup("EntityId")).prim() != refl::Struct);  // {id}: a wrapper
    CHECK(refl::unwrap(refl::byName("keen::ds::g38_options::common::user::GraphicsSettings")).prim() == refl::Struct);
    lua_State* L = rt::open();
    rt::setWorld(w.addr());
    if (luaL_dostring(L, kTypesChecks) != LUA_OK) {
        printf("FAIL types: %s\n", lua_tostring(L, -1));
        exit(1);
    }
    rt::exec(L, "Ecs.fields('ItemInfo')");
    CHECK(gOut.find("maxStackSize: keen::uint16") != std::string::npos);
    rt::setWorld(0);
    lua_close(L);
    printf("types ok\n");
}

std::string gTmp;  // %TEMP%\rt_selftest: the mods folder (freshMods), and bindings.ini next to it

// Writes dir\name, creating dir.
void writeFile(const std::string& dir, const char* name, const char* text) {
    CreateDirectoryA(dir.c_str(), nullptr);
    FILE* f = nullptr;
    CHECK(fopen_s(&f, (dir + "\\" + name).c_str(), "wb") == 0);
    fputs(text, f);
    fclose(f);
}

// Deletes a folder and everything in it.
void removeTree(const std::string& dir) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    for (bool more = h != INVALID_HANDLE_VALUE; more; more = FindNextFileA(h, &fd) != 0) {
        std::string name = fd.cFileName, path = dir + "\\" + name;
        if (name == "." || name == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(path);
        else DeleteFileA(path.c_str());
    }
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    RemoveDirectoryA(dir.c_str());
}

// Sets a global in its own environment, requires a bytecode file from its folder, and registers bindings.
const char* kHelloMod = R"(
    HP = Ecs.get(1, 'Health').dataStorage[0]
    print('hello mod up', HP)
    local ok, e = pcall(require, 'bin')
    print('require bin', not ok and e:find('binary chunk') ~= nil)
    Keys.hotkey('heal', 'Heal', function() end)
    Keys.input('hold', 'Hold', function(down) end)
    print('badid', pcall(Keys.hotkey, 'a b', 'x', print))
)";

// A module from its own folder, files inside its folder, and paths that must not get out of it. Errors name the mod's
// own files, and a tick callback that errors is dropped under the mod's name.
const char* kLibMod = R"(
    SHARED = 1
    local util = require('util')
    print('where', select(2, pcall(function() local x return x.y end)))
    print('broken', select(2, pcall(require, 'broken')))
    Tick.add(function() error('tick boom') end)
    local f = assert(io.open('save.txt', 'w'))
    f:write('a\nb\n')
    f:close()
    local n = 0
    for _ in io.lines('save.txt') do n = n + 1 end
    f = io.open('save.txt')
    local all = f:read('a')
    f:close()
    local bad = 0
    for _, p in ipairs({'../x', 'C:\\x', '/x', 'a\\..\\b', 'a/.. /b', 'x:y', '', 'sub/'}) do
        if not pcall(io.open, p) then bad = bad + 1 end
    end
    print('io', all == 'a\nb\n', n, bad)
    return {version = 2, util = util.ok, own = SHARED, hp = HP, again = require('util') == util}
)";

// Loads at the main menu, and enters the first world with the game (entity 1 has Health).
const char* kMenuMod = R"lua(
    registerForEvent("onInit", function() print("menu init") end)
    local upd
    registerForEvent("onUpdate", function(dt)
        if not upd then print("menu update", math.type(dt) == "float" and dt >= 0) end
        upd = true
    end)
    registerForEvent("onWorldEnter", function() print("menu enter", Ecs.get(1, "Health") ~= nil) end)
)lua";

// %TEMP%\rt_selftest\mods, emptied, and the runtime pointed at it.
std::string freshMods() {
    char tmp[MAX_PATH];
    CHECK(GetTempPathA(MAX_PATH, tmp) > 0);
    gTmp = std::string(tmp) + "rt_selftest";
    removeTree(gTmp);  // leftovers of a failed run
    std::string mods = gTmp + "\\mods";
    CreateDirectoryA(gTmp.c_str(), nullptr);
    CreateDirectoryA(mods.c_str(), nullptr);
    rt::configure(mods, sink);
    return mods;
}

// Mods load on the main thread's first frame, before any world. There is no game there: Ecs finds nothing, and admin
// commands and events are refused. Console lines run at the next frame.
void checkMenu() {
    writeFile(freshMods() + "\\menu", "init.lua", kMenuMod);
    CHECK(!rt::modsLoaded());
    rt::frame();
    CHECK(rt::modsLoaded() && gOut.find(LV_OK "Mod loaded: menu\n") != std::string::npos);
    CHECK(gOut.find("menu init\nmenu update\ttrue\n") != std::string::npos);
    rt::queue("local n = 0 for _ in Ecs.each('Health') do n = n + 1 end print('menu ecs', Ecs.player(), n)");
    rt::queue("print('menu admin', pcall(Admin.setTime, 12))");
    rt::queue("print('menu post', pcall(Events.post, 'TestEvent1', {data = 1}))");
    rt::queue("print('menu reads', select(2, pcall(Admin.settings)), select(2, pcall(Admin.spawn, 'x')))");
    rt::frame();
    CHECK(gOut.find("menu ecs\tnil\t0\n") != std::string::npos);
    CHECK(gOut.find("menu admin\tfalse\tnot in a world you host\n") != std::string::npos);
    CHECK(gOut.find("menu post\tfalse\tnot in a world you host\n") != std::string::npos);
    CHECK(gOut.find("menu reads\tnot in a world you host\tnot in a world you host\n") != std::string::npos);
    printf("main menu ok\n");
}

// Full hook path: rt::tick validates the world and enters it, then runs queued console lines: first a reload, which
// loads mods from a temp folder inside the world. The "bin" mod is a bytecode file: it must be refused both as a mod
// and through require. "lib" returns a table for GetMod, and "empty" has no init.lua. Mods keep their globals to
// themselves.
void checkTick(FakeWorld& w) {
    std::string mods = freshMods();
    writeFile(mods + "\\hello", "init.lua", kHelloMod);
    writeFile(mods + "\\hello", "bin.lua", "\x1bLua");
    writeFile(mods + "\\bin", "init.lua", "\x1bLua");
    writeFile(mods + "\\lib", "init.lua", kLibMod);
    // util.lua starts with a UTF-8 BOM, as Notepad saves it
    writeFile(mods + "\\lib", "util.lua", "\xEF\xBB\xBFreturn {ok = Keys.hotkey ~= nil and io.open ~= nil}\n");
    writeFile(mods + "\\lib", "broken.lua", "return x.y\n");
    CreateDirectoryA((mods + "\\empty").c_str(), nullptr);
    rt::queue("reload()");
    rt::tick(w.addr());
    CHECK(gOut.find("Entered a world\nmenu enter\ttrue\n") != std::string::npos);
    for (const char* line : {"print('isolated', HP, SHARED, Keys.hotkey, io)",
                             "print('getmod', GetMod('hello'), GetMod('lib').version, GetMod('bin'), GetMod('empty'))",
                             "local l = GetMod('lib') print('lib', l.util, l.own, l.hp, l.again)", "reload()"})
        rt::queue(line);
    rt::tick(w.addr());
    rt::queue("print('after reload', GetMod('lib').version)");
    rt::tick(w.addr());
    CHECK(gOut.find("server world") != std::string::npos);
    CHECK(gOut.find("hello mod up\t77") != std::string::npos && gOut.find("Mod loaded: hello") != std::string::npos);
    CHECK(gOut.find("require bin\ttrue") != std::string::npos && gOut.find("attempt to load a binary chunk") != std::string::npos);
    CHECK(gOut.find(LV_BAD "Mod failed: bin") != std::string::npos && gOut.find(LV_WARN "Mod skipped: empty (no init.lua)") != std::string::npos);
    CHECK(gOut.find("Mods: 2 loaded, 1 failed") != std::string::npos);
    CHECK(gOut.find("badid\tfalse\t") != std::string::npos && gOut.find("use letters, digits and _ only") != std::string::npos);
    CHECK(gOut.find("io\ttrue\t2\t8") != std::string::npos && gOut.find("isolated\tnil\tnil\tnil\tnil") != std::string::npos);
    CHECK(gOut.find("lib\ttrue\t1\tnil\ttrue") != std::string::npos);
    CHECK(gOut.find("where\tlib/init.lua:") != std::string::npos);
    CHECK(gOut.find("broken\tlib/broken.lua:1: attempt to index a nil value (global 'x')") != std::string::npos);
    CHECK(gOut.find(LV_BAD "lib: tick callback removed after an error\n") != std::string::npos);
    CHECK(gOut.find("getmod\ttrue\t2\tnil\tnil") != std::string::npos && gOut.find("after reload\t2") != std::string::npos);
    printf("tick + mods ok\n");
}

// The Bindings window's side: hello registered "heal" (hotkey) and "hold" (input) in checkTick. A combination
// survives reload() through bindings.ini, and a saved line no loaded mod registers is dead.
void checkBindings(FakeWorld& w) {
    std::string ini = gTmp + "\\bindings.ini";
    int dead = -1;
    std::vector<rt::BindInfo> rows = rt::bindings(dead);
    CHECK(rows.size() == 2 && dead == 0 && rows[0].key == "hello.heal" && rows[0].label == "Heal" && !rows[0].input);
    CHECK(rows[1].key == "hello.hold" && rows[1].input && rows[0].combo.empty() && rows[1].combo.empty());
    rt::setBinding("hello.heal", rt::ModCtrl, VK_F6);
    rt::setBinding("hello.hold", rt::ModCtrl | rt::ModShift | rt::ModAlt, 'A');
    WritePrivateProfileStringA("bindings", "gone.x", "F2", ini.c_str());
    rt::queue("reload()");
    rt::tick(w.addr());
    rows = rt::bindings(dead);
    CHECK(rows.size() == 2 && rows[0].combo == "Ctrl+F6" && rows[1].combo == "Ctrl+Shift+Alt+A" && dead == 1);
    rt::removeDeadBindings();
    rt::setBinding("hello.heal", 0, 0xBB);
    rt::setBinding("hello.hold", 0, 0);
    rows = rt::bindings(dead);
    CHECK(dead == 0 && rows[0].combo == "0xBB" && rows[1].combo.empty());
    CHECK(gOut.find("Binding hello.heal: Ctrl+F6\n") != std::string::npos && gOut.find("Binding hello.hold: unbound\n") != std::string::npos);
    char v[16];
    GetPrivateProfileStringA("bindings", "gone.x", "none", v, sizeof v, ini.c_str());
    CHECK(strcmp(v, "none") == 0);
    GetPrivateProfileStringA("bindings", "hello.hold", "none", v, sizeof v, ini.c_str());
    CHECK(strcmp(v, "none") == 0);
    GetPrivateProfileStringA("bindings", "hello.heal", "", v, sizeof v, ini.c_str());
    CHECK(strcmp(v, "0xBB") == 0);
    removeTree(gTmp);
    printf("bindings ok\n");
}

void checkEvents(FakeWorld& w) {
    uintptr_t world = w.addr();
    uint32_t died = refl::byName("EntityDiedEvent").hash(), test = refl::byName("TestEvent1").hash();
    uint32_t noise = refl::byName("NoiseEvent").hash();
    CHECK(ecs::eventIndex(world, died) == 1 && ecs::eventIndex(world, test) == 3 && ecs::eventIndex(world, noise) == -1);
    std::vector<uint32_t> ids;
    ecs::forEachEvent(world, 1, [&](const uint8_t* ev) { ids.push_back(*reinterpret_cast<const uint32_t*>(ev + 8)); });
    CHECK(ids.size() == 130 && ids[0] == 100 && ids[126] == 226 && ids[127] == 227 && ids[129] == 229);
    CHECK(ecs::simTime(world) == 99999999);
    uint8_t* p = ecs::postEvent(world, test, 4);
    CHECK(p == w.posterBuf.data() + 8 && w.posterUsed() == 16);
    CHECK(*reinterpret_cast<uint32_t*>(w.posterBuf.data()) == test && w.posterBuf[6] == 4 && w.posterBuf[4] == 0);
    CHECK(!ecs::postEvent(world, noise, 168) && !ecs::postEvent(world, test, 0x100));
    for (int i = 0; i < 20; i++) ecs::postEvent(world, test, 4);  // 16 records of 16 bytes fill 0x100
    CHECK(w.posterUsed() == 0x100 && !ecs::postEvent(world, test, 4));
    w.posterUsed() = 0;
    printf("events ok\n");
}

// Observe and Events.post through the real hook path: rt::tick runs queued lines, rt::afterTick dispatches.
void checkLuaEvents(FakeWorld& w) {
    uintptr_t world = w.addr();
    for (const char* line : {
             "Observe('EntityDiedEvent', function(e) N = (N or 0) + 1; LAST = e.deadEntityId; KEEP = e end)",
             "Observe('TestEvent1', function(e) print('test', e.data); e.data = 5 end)"})
        rt::queue(line);
    rt::tick(world);
    rt::afterTick(world);
    for (const char* line : {
             "print('died', N, LAST)", "print(pcall(function() return KEEP.deadEntityId end))",
             "print('post', Events.post('TestEvent1', {data = 42}), Events.post('EntityDiedEvent', {deadEntityId = 7}))",
             "print(pcall(Events.post, 'Health', {}))", "print(pcall(Events.post, 'TestEvent1', {nope = 1}))",
             "print(pcall(Events.post, 'TestEvent1', {data = -1}))", "print(pcall(Events.post, 'NoiseEvent', {}))",
             "print(pcall(Observe, 'Health', print))"})
        rt::queue(line);
    rt::tick(world);
    CHECK(gOut.find("died\t130\t229") != std::string::npos);
    CHECK(gOut.find("no longer valid") != std::string::npos);
    CHECK(gOut.find("post\ttrue\ttrue") != std::string::npos);
    CHECK(gOut.find("'Health' is not an event type") != std::string::npos);
    CHECK(gOut.find("has no field 'nope'") != std::string::npos && gOut.find("doesn't fit") != std::string::npos);
    CHECK(gOut.find("has no keen::ecs::NoiseEvent stream") != std::string::npos);
    uint8_t* buf = w.posterBuf.data();  // TestEvent1 {data} at 8, then EntityDiedEvent {timeStamp, deadEntityId} at 24
    CHECK(*reinterpret_cast<uint32_t*>(buf + 8) == 42 && w.posterUsed() == 16 + 24);
    CHECK(*reinterpret_cast<int64_t*>(buf + 24) == 99999999 && *reinterpret_cast<uint32_t*>(buf + 32) == 7);
    memcpy(w.testPages.data() + 8, buf + 8, 4);  // what the game's distributor would do with the posted TestEvent1
    w.streamAt(3)[0x18] = 1;
    rt::afterTick(world);
    CHECK(gOut.find("test\t42") != std::string::npos && gOut.find("events are read-only") != std::string::npos);
    CHECK(gOut.find("event handler removed") != std::string::npos);
    printf("observe + post ok\n");
}

}  // namespace

void checkKeys() {
    CHECK(rt::keyFromName("`") == VK_OEM_3 && rt::keyFromName("F8") == VK_F8 && rt::keyFromName("f24") == VK_F24);
    CHECK(rt::keyFromName("pageup") == VK_PRIOR && rt::keyFromName("0xC0") == 0xC0 && rt::keyFromName("A") == 'A');
    for (const char* bad : {"F25", "F1x", "0x", "0x1FF", "AB", "", "Tab"}) CHECK(rt::keyFromName(bad) == 0);
    CHECK(rt::keyName(VK_OEM_3) == "`" && rt::keyName(VK_F6) == "F6" && rt::keyName(VK_F24) == "F24");
    CHECK(rt::keyName('Q') == "Q" && rt::keyName('7') == "7" && rt::keyName(VK_PRIOR) == "PageUp" && rt::keyName(0xBB) == "0xBB");
    printf("key names ok\n");
}

// A window with every kind of widget, a game read that must be refused, and a callback that errors inside Begin.
const char* kDrawMod = R"(
    Draw.add(function()
        if ImGui.Begin("Test window") then
            ImGui.Text("100% %s %n")
            local v, pressed = ImGui.Checkbox("check", true)
            local f = ImGui.SliderFloat("slider", 0.5, 0, 1)
            local t = ImGui.InputText("input", "abc", 16)
            ImGui.Button("button")
            local child = ImGui.BeginChild("list", 0, 100, true)
            if child then ImGui.Button("in child") end
            ImGui.EndChild()
            DRAWN = v == true and pressed == false and f == 0.5 and t == "abc" and child == true
        end
        ImGui.End()
        local ok, e = pcall(Ecs.get, 1, "Health")
        BLOCKED = not ok and e:find("not available in Draw") ~= nil
    end)
    Draw.add(function() ImGui.Begin("Broken") ImGui.BeginChild("open child") error("boom") end)
)";

// One headless frame; returns the window stack depth after the Draw callbacks (1 = only ImGui's implicit window).
int drawFrame() {
    ImGui::NewFrame();
    rt::draw();
    int depth = ImGui::GetCurrentContext()->CurrentWindowStack.Size;
    ImGui::Render();
    return depth;
}

// Draw callbacks in a headless ImGui context: with RendererHasTextures, texture requests just go unanswered.
void checkDraw(FakeWorld& w) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.IniFilename = nullptr;
    rt::queue(kDrawMod);
    rt::tick(w.addr());
    CHECK(rt::hasDraw());
    CHECK(drawFrame() == 1 && drawFrame() == 1);  // the broken callback's Begin was closed for it
    rt::setOverlayOpen(true);
    rt::queue("print('draw', DRAWN, BLOCKED, Overlay.isOpen(), pcall(ImGui.Text, 'x'))");
    rt::tick(w.addr());
    rt::setOverlayOpen(false);
    ImGui::DestroyContext();
    size_t first = gOut.find("draw callback removed after an error");
    CHECK(first != std::string::npos && gOut.find("draw callback removed", first + 1) == std::string::npos);
    CHECK(gOut.find("draw\ttrue\ttrue\ttrue\tfalse\t") != std::string::npos);
    CHECK(gOut.find("only work inside Draw") != std::string::npos);
    printf("draw ok\n");
}

uint32_t& u32At(uint8_t* p) { return *reinterpret_cast<uint32_t*>(p); }
int64_t& i64At(uint8_t* p) { return *reinterpret_cast<int64_t*>(p); }
float f32At(uint8_t* p) { return *reinterpret_cast<float*>(p); }

// Admin commands through the real path: Lua queues, rt::tick arms, rt::applyAdmin (the consumer hook) writes. The
// offsets are the ones the game's code uses, so the reflection lookups are checked against them.
// Set daytime: action at PlayerInput + 0x478 {syncDaytime, syncDaytimeTick, version at + 0x10}, counter at + 0xb4.
void checkAdminFlow(FakeWorld& w) {
    uintptr_t world = w.addr();
    uint8_t *in = w.input(), *con = w.consumed();
    rt::queue("print('admin off', pcall(Admin.setTime, 12))");
    rt::tick(world);
    CHECK(gOut.find("admin off\tfalse\tadmin commands are unavailable") != std::string::npos);
    rt::setAdminAvailable(true);
    w.addPlayer();
    u32At(in + 0x488) = 5;                            // the live version
    rt::queue("Admin.setTime(6) Admin.setTime(12)");  // the later call replaces the earlier one
    rt::tick(world);
    CHECK(i64At(in + 0x478) == 0 && u32At(con + 0xb4) == 0);  // armed only: the game rebuilds its input in the tick
    rt::applyAdmin(nullptr);
    CHECK(i64At(in + 0x478) == 43200000000000 && i64At(in + 0x480) == 99999999);
    CHECK(u32At(in + 0x488) == 5 && u32At(con + 0xb4) == 6);
    i64At(in + 0x478) = 7;
    rt::applyAdmin(nullptr);  // nothing armed
    CHECK(i64At(in + 0x478) == 7);
    u32At(con + 0xb4) = 5;  // what the game's consumer does after running the command
    rt::tick(world);
    CHECK(gOut.find("was not run") == std::string::npos);
    rt::queue("Admin.setTime(1)");
    rt::tick(world);
    rt::applyAdmin(nullptr);  // and the game doesn't run it this time
    rt::tick(world);
    CHECK(u32At(con + 0xb4) == 5 && gOut.find("Admin command adminSetDaytime: not run by the game") != std::string::npos);
    i64At(in + 0x478) = 7;
    printf("admin flow ok\n");
}

// Admin.send: nested fields (teleport position at PlayerInput + 0x434, counter at + 0xac) and refused calls.
void checkAdminSend(FakeWorld& w) {
    uintptr_t world = w.addr();
    uint8_t *in = w.input(), *con = w.consumed();
    rt::queue("Admin.send('adminTeleport', {transform = {position = {x = 1.5, y = 2, z = -3}}, searchBestSpawnPosition = true})");
    rt::tick(world);
    rt::applyAdmin(nullptr);
    CHECK(f32At(in + 0x434) == 1.5f && f32At(in + 0x438) == 2.f && f32At(in + 0x43c) == -3.f && in[0x454] == 1);
    CHECK(u32At(con + 0xac) == 1);
    u32At(con + 0xac) = 0;
    for (const char* line : {"print('e1', pcall(Admin.send, 'nope', {}))",
                             "print('e2', pcall(Admin.send, 'adminSetDaytime', {nope = 1}))",
                             "print('e3', pcall(Admin.send, 'adminSetDaytime', {versionData = 1}))",
                             "print('e4', pcall(Admin.setTime, 24))",
                             "print('e5', pcall(Admin.send, 'adminKillFaction', {}))",
                             "print('e6', pcall(Admin.send, 'adminRepairAllItemsAction', {}))"})  // not hooked yet: inventory_actions
        rt::queue(line);
    rt::tick(world);
    rt::applyAdmin(nullptr);
    CHECK(i64At(in + 0x478) == 7);  // the failed calls queued nothing
    CHECK(gOut.find("e1\tfalse\t'nope' is not an admin command") != std::string::npos);
    CHECK(gOut.find("e2\tfalse\tkeen::ecs::AdminSetDayTimeAction has no field 'nope'") != std::string::npos);
    CHECK(gOut.find("e3\tfalse\tversionData is set by the runtime") != std::string::npos);
    CHECK(gOut.find("e4\tfalse") != std::string::npos && gOut.find("hours must be at least 0 and below 24") != std::string::npos);
    CHECK(gOut.find("e5\tfalse\t'adminKillFaction' is not an admin command") != std::string::npos);
    CHECK(gOut.find("e6\tfalse\tadminRepairAllItemsAction is unavailable in this game build") != std::string::npos);
    printf("admin send ok\n");
}

// Admin.* from a Draw callback (queueing touches no game memory), and nested tables in Events.post.
void checkAdminDraw(FakeWorld& w) {
    uintptr_t world = w.addr();
    ImGui::CreateContext();
    ImGui::GetIO().DisplaySize = ImVec2(800, 600);
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGui::GetIO().IniFilename = nullptr;
    rt::queue("Draw.add(function() if not SENT then SENT = true; Admin.setTime(18) end end)");
    rt::tick(world);
    drawFrame();
    rt::tick(world);
    rt::applyAdmin(nullptr);
    ImGui::DestroyContext();
    CHECK(i64At(w.input() + 0x478) == 64800000000000);
    u32At(w.consumed() + 0xb4) = 5;
    w.posterUsed() = 0;  // EntityDiedEvent.deadEntityId is an EntityId {id}: record header, timeStamp, then the id
    rt::queue("print('nested post', Events.post('EntityDiedEvent', {deadEntityId = {id = 9}}))");
    rt::tick(world);
    CHECK(gOut.find("nested post\ttrue") != std::string::npos && u32At(w.posterBuf.data() + 16) == 9);
    printf("admin draw ok\n");
}

// The helpers, at the offsets the game's code uses: teleport at PlayerInput + 0x434
// (orientation w + 0x1c, search flag + 0x20), flame level + 0x49c {version, level}, character level + 0x4a4
// {version, targetEntityId, level}. No kill faction: release builds have no system that reads it.
void checkAdminHelpers(FakeWorld& w) {
    uint8_t *in = w.input(), *con = w.consumed();
    auto run = [&](const char* line) {
        rt::queue(line);
        rt::tick(w.addr());
        rt::applyAdmin(nullptr);
    };
    run("Admin.teleport(1.5, 2, -3)");
    CHECK(f32At(in + 0x434) == 1.5f && f32At(in + 0x43c) == -3.f && f32At(in + 0x450) == 1.f && in[0x454] == 1);
    u32At(con + 0xac) = 0;
    run("Admin.teleport(1, 2, 3, true)");
    CHECK(f32At(in + 0x434) == 1.f && in[0x454] == 0 && u32At(con + 0xac) == 1);
    u32At(con + 0xac) = 0;
    run("Admin.setLevel(7) Admin.setFlameLevel(3)");
    CHECK(u32At(in + 0x4a8) == 1 && in[0x4ac] == 7 && u32At(con + 0xc0) == 1);  // target 0 became the player, entity 1
    CHECK(in[0x4a0] == 3 && u32At(con + 0xbc) == 1);
    u32At(con + 0xc0) = u32At(con + 0xbc) = 0;
    run("Admin.send('adminSetCharacterLevel', {targetEntityId = 5, level = 2})");
    CHECK(u32At(in + 0x4a8) == 5 && in[0x4ac] == 2);
    u32At(con + 0xc0) = 0;
    for (const char* line : {"print('h1', Admin.killFaction, pcall(Admin.send, 'adminKillFaction', {}))",
                             "print('h2', pcall(Admin.setLevel, 0))", "print('h3', pcall(Admin.setFlameLevel, 256))",
                             "print('h4', pcall(Admin.teleport, 1, 2))"})
        rt::queue(line);
    run("");
    CHECK(gOut.find("h1\tnil\tfalse\t'adminKillFaction' is not an admin command") != std::string::npos);
    for (const char* tag : {"h2\tfalse", "h3\tfalse", "h4\tfalse"}) CHECK(gOut.find(tag) != std::string::npos);
    CHECK(gOut.find("must be from 1 to 255") != std::string::npos);
    CHECK(u32At(con + 0xc0) == 0 && u32At(con + 0xbc) == 0);  // the refused calls queued nothing
    printf("admin helpers ok\n");
}

// The admin consumer's job as the game's query fill writes it: per-entity slots 1-8,
// then one group of the six resources, 4 read then 2 written, so ecs::GameSettings is slot 13 and Daytime slot 14.
// The settings start out like a normal world's.
struct FakeJob {
    std::vector<uint8_t> sys = std::vector<uint8_t>(0x800), first = std::vector<uint8_t>(0x408);
    std::vector<uint8_t> settings = std::vector<uint8_t>(0x90);
    uint8_t daytime[8] = {};
    uint8_t* sysp = sys.data();
    void* job = &sysp;

    template <class T>
    void put(uint8_t* p, T v) { memcpy(p, &v, sizeof v); }
    uint8_t* at(const char* name) {
        refl::Field f;
        CHECK(refl::field(refl::byName("GameSettings"), name, f));
        return settings.data() + f.offset;
    }
    FakeJob() {
        sys[0x4b0] = 1;  // 1 + 1 + 1 + 6 * 1: the resource groups start at slot 9
        sys[0x4b8] = 1;
        sys[0x4ba] = 1;
        put(&sys[0xc0], first.data());  // an empty group: its count byte (array + 0x400) is 0
        sys[0x7f0] = 6;
        put(&sys[0x7a8 + 4 * 8], settings.data());
        put(&sys[0x7a8 + 5 * 8], &daytime[0]);
        refl::forFields(refl::byName("GameSettings"), [&](const refl::Field& f) {
            if (f.type.resolved().prim() == refl::F32) put(settings.data() + f.offset, 1.f);
        });
        *at("enableDurability") = 1;
        *at("weatherFrequency") = 2;
        put(at("dayTimeDuration"), int64_t(1800000000000));
    }
};

void checkQuerySlot(FakeJob& j) {
    ecs::ResSlot q{};
    CHECK(ecs::querySlot(j.job, kSettingsSlot, q) && q.ptr == uintptr_t(j.settings.data()) && q.group == 6 && q.index == 4);
    CHECK(ecs::querySlot(j.job, 14, q) && q.ptr == uintptr_t(j.daytime) && q.index == 5);
    CHECK(!ecs::querySlot(j.job, 8, q) && !ecs::querySlot(j.job, 9, q) && !ecs::querySlot(j.job, 15, q));
    printf("query slots ok\n");
}

// Game settings, read and merged. The action sits at PlayerInput + 0x4c8 (version + 0x90 = 0x558, counter + 0xd0);
// GameSettings offsets: playerHealthFactor 0, enableDurability 0x14, enableStarvingDebuff 0x15, weatherFrequency
// 0x2e, enemyDamageFactor 0x58, dayTimeDuration 0x78.
void checkAdminSettings(FakeWorld& w, FakeJob& j) {
    uint8_t *in = w.input() + 0x4c8, *con = w.consumed();
    auto run = [&](const char* line) {
        rt::queue(line);
        rt::tick(w.addr());
        rt::applyAdmin(j.job);
    };
    rt::setSettingsSlot(4, 6);
    run("print('s0', Admin.settings())");  // the settings are taken in the consumer, after this line ran
    run("local s = Admin.settings() print('s1', s.enemyDamageFactor, s.enableStarvingDebuff, s.weatherFrequency, s.dayTimeDuration)");
    CHECK(gOut.find("s0\tnil") != std::string::npos && gOut.find("Game settings: ready") != std::string::npos);
    CHECK(gOut.find("s1\t1.0\tfalse\t2\t1800000000000") != std::string::npos);
    u32At(in + 0x90) = 3;
    run("Admin.changeSettings({enemyDamageFactor = 0.5}) Admin.send('adminChangeGameSettingsAction', {gameSettings = {weatherFrequency = 1}})");
    CHECK(f32At(in + 0x58) == 0.5f && in[0x2e] == 1 && f32At(in) == 1.f && in[0x14] == 1 && in[0x15] == 0);
    CHECK(i64At(in + 0x78) == 1800000000000 && u32At(in + 0x90) == 3 && u32At(con + 0xd0) == 4);
    u32At(con + 0xd0) = 3;
    run("print('g1', pcall(Admin.changeSettings, {weatherFrequency = 9})) print('g2', pcall(Admin.changeSettings, {nope = 1}))");
    CHECK(gOut.find("g1\tfalse") != std::string::npos && gOut.find("9 is not a value of keen::ecs::WeatherFrequency") != std::string::npos);
    CHECK(gOut.find("g2\tfalse") != std::string::npos && gOut.find("GameSettings has no field 'nope'") != std::string::npos);
    CHECK(u32At(con + 0xd0) == 3);
    *j.at("enableDurability") = 7;  // not a bool: the settings are refused, and so is a change
    rt::setSettingsSlot(4, 6);
    run("print('s2', Admin.settings()) Admin.changeSettings({enemyDamageFactor = 2})");
    CHECK(gOut.find("Game settings: not recognized yet (field enableDurability = 7), still trying") != std::string::npos);
    CHECK(gOut.find("s2\tnil") != std::string::npos);
    CHECK(gOut.find("adminChangeGameSettingsAction: not run, game settings unknown") != std::string::npos);
    CHECK(u32At(con + 0xd0) == 3 && f32At(in + 0x58) == 0.5f);
    *j.at("enableDurability") = 1;  // as a load finishing would: the next consumer call takes the settings
    size_t before = gOut.size();
    rt::applyAdmin(j.job);
    CHECK(gOut.find("Game settings: ready", before) != std::string::npos);
    rt::setSettingsSlot(5, 6);  // the descriptor's index must match the slot's
    rt::applyAdmin(j.job);
    CHECK(gOut.find("(slot 13 is 4 of 6, expected 5 of 6)") != std::string::npos);
    printf("admin settings ok\n");
}

// Every registerForEvent event, the registerHotkey and registerInput aliases, and an unknown event that must be refused.
const char* kLifeMod = R"lua(
    registerForEvent("onInit", function() print("init", GetMod("life") ~= nil) end)
    local upd
    registerForEvent("onUpdate", function(dt)
        if not upd then print("update", math.type(dt) == "float" and dt >= 0) end
        upd = true
    end)
    registerForEvent("onDraw", function() end)
    registerForEvent("onOverlayOpen", function() print("overlay open") end)
    registerForEvent("onOverlayClose", function() print("overlay close") end)
    registerForEvent("onWorldEnter", function() print("enter", Ecs.player() ~= nil) end)
    registerForEvent("onWorldLeave", function() print("leave", pcall(Ecs.player)) end)
    registerForEvent("onShutdown", function() print("shutdown", pcall(Ecs.player)) end)
    registerHotkey("h", "H", function() end)
    registerInput("i", "I", function(down) end)
    print("tweak", pcall(registerForEvent, "onTweak", print))
)lua";

size_t countOf(const std::string& s, const std::string& what) {
    size_t n = 0;
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) n++;
    return n;
}

// Worlds: a respawn and a load both pause ticks for over 2 s, but only a load starts the sim time again (the tick
// index at world + 0x4dc). A load whose leave went unseen leaves the old world first. A seen leave hands the mods to
// the main thread until the next world; a reload there runs no onWorldEnter.
void checkWorlds(FakeWorld& w) {
    uint32_t& tickIndex = *reinterpret_cast<uint32_t*>(w.world + 0x4dc);
    Sleep(2100);
    tickIndex = 4;  // a respawn: the session's sim time went on
    rt::tick(w.addr());
    CHECK(countOf(gOut, "enter\t") == 1 && countOf(gOut, "leave\t") == 0);
    Sleep(2100);
    tickIndex = 2;  // a load, and the leave went unseen
    rt::tick(w.addr());
    CHECK(gOut.find("Left the world\nleave\tfalse\tthe world is closing") != std::string::npos);
    CHECK(gOut.find("Entered a world\nenter\ttrue\n") != std::string::npos && countOf(gOut, "init\t") == 1);
    tickIndex = 3;
    rt::tick(w.addr());
    rt::worldLeft();
    CHECK(countOf(gOut, "leave\tfalse\tthe world is closing") == 2);
    rt::queue("print('menu line')");
    rt::tick(w.addr());  // the world still ticks while it shuts down: no mods there
    CHECK(gOut.find("menu line") == std::string::npos);
    rt::frame();
    CHECK(gOut.find("menu line\n") != std::string::npos);
    rt::queue("reload()");
    rt::frame();
    CHECK(countOf(gOut, "init\t") == 2 && countOf(gOut, "enter\t") == 2 && gOut.find("shutdown\ttrue\tnil\n") != std::string::npos);
    Sleep(2100);
    tickIndex = 2;  // the next world
    rt::tick(w.addr());
    CHECK(countOf(gOut, "enter\t") == 3 && countOf(gOut, "leave\t") == 2 && countOf(gOut, "init\t") == 2);
    tickIndex = 3;
    removeTree(gTmp);
    printf("worlds ok\n");
}

// Lifecycle events through rt::tick, after a reload in a world: onInit, then onWorldEnter with the game. Ecs.each over
// two components (entity 1 has PlayerInput since checkAdminFlow). In a world, the main thread's frame runs nothing.
void checkLifecycle(FakeWorld& w) {
    std::string mods = gTmp + "\\mods";
    CreateDirectoryA(gTmp.c_str(), nullptr);
    CreateDirectoryA(mods.c_str(), nullptr);
    writeFile(mods + "\\life", "init.lua", kLifeMod);
    rt::configure(mods, sink);
    gOut.clear();
    rt::queue("local n = 0 for id, hp, pi in Ecs.each('Health', 'PlayerInput') do n = n + id end print('each2', n)");
    rt::queue("reload()");
    rt::tick(w.addr());
    rt::tick(w.addr());
    rt::setOverlayOpen(true);
    rt::tick(w.addr());
    rt::setOverlayOpen(false);
    rt::tick(w.addr());
    CHECK(gOut.find("each2\t1\n") != std::string::npos && gOut.find("init\ttrue\nenter\ttrue\n") != std::string::npos);
    CHECK(gOut.find("update\ttrue\n") != std::string::npos && gOut.find("overlay open\noverlay close\n") != std::string::npos);
    CHECK(gOut.find("tweak\tfalse\t") != std::string::npos && gOut.find("unknown event (onInit") != std::string::npos);
    int dead = 0;
    std::vector<rt::BindInfo> rows = rt::bindings(dead);
    CHECK(rows.size() == 2 && rows[0].key == "life.h" && !rows[0].input && rows[1].key == "life.i" && rows[1].input);
    CHECK(rt::hasDraw());
    rt::queue("print('in world')");
    rt::frame();
    CHECK(gOut.find("in world") == std::string::npos);
    rt::tick(w.addr());
    CHECK(gOut.find("in world\n") != std::string::npos);
    printf("lifecycle ok\n");
    checkWorlds(w);
}

// The game closing, from inside a world: onWorldLeave, then onShutdown with only its files (the game frees its options
// and resources as it exits), then no mods ever again. Last, after checkOptions: the fake options are still attached.
const char* kByeMod = R"lua(
    local m = GameOptions.machine()
    registerForEvent("onWorldLeave", function() print("bye leave") end)
    registerForEvent("onShutdown", function()
        print("bye opts", pcall(GameOptions.user))
        print("bye res", pcall(Res.find, "ItemInfo", "Test_Wood_Log"))
        print("bye view", pcall(function() return m.version end))
        local f = io.open("saved.txt", "w")
        print("bye io", f ~= nil)
        if f then f:close() end
    end)
)lua";

void checkClosing(FakeWorld& w) {
    std::string mods = gTmp + "\\mods";
    CreateDirectoryA(gTmp.c_str(), nullptr);
    CreateDirectoryA(mods.c_str(), nullptr);
    writeFile(mods + "\\bye", "init.lua", kByeMod);
    rt::configure(mods, sink);
    rt::queue("reload()");
    rt::tick(w.addr());
    CHECK(rt::modsLoaded() && gOut.find("Mod loaded: bye\n") != std::string::npos);
    gOut.clear();
    rt::gameClosing();
    CHECK(gOut.find("Left the world\nbye leave\nGame closing: unloading mods\n") != std::string::npos);
    CHECK(gOut.find("bye opts\tfalse\tthe game is closing") != std::string::npos);
    CHECK(gOut.find("bye res\tfalse\tthe game is closing") != std::string::npos);
    CHECK(gOut.find("bye view\tfalse\t") != std::string::npos && gOut.find("bye io\ttrue\n") != std::string::npos);
    rt::frame();
    rt::tick(w.addr());
    CHECK(!rt::modsLoaded() && gOut.find("Mods: ") == std::string::npos);
    removeTree(gTmp);
    printf("closing ok\n");
}

// An access violation in Lua work (here a world at 0x10: ecs::simTime reads world + 0x4c0) turns the mods off for
// good, so this runs in a process of its own (main's --av). The lock must be free afterwards: Draw, on another
// thread, gets it, and nothing runs Lua again.
int checkAccessViolation() {
    SetErrorMode(SEM_NOGPFAULTERRORBOX);  // an escaped access violation fails the parent's check, no crash dialog
    freshMods();
    rt::frame();
    CHECK(rt::modsLoaded());
    rt::tick(0x10);
    CHECK(gOut.find("Lua: access violation, mods off until the game restarts\n") != std::string::npos);
    CHECK(!rt::modsLoaded());
    std::atomic<bool> drawn = false;
    std::thread([&] {
        rt::draw();
        drawn = true;
    }).detach();
    for (int i = 0; i < 200 && !drawn; i++) Sleep(10);
    CHECK(drawn);
    rt::queue("print('after')");
    rt::frame();
    CHECK(gOut.find("after") == std::string::npos);
    removeTree(gTmp);
    printf("access violation ok\n");
    return 0;
}

// Content hashes {size, hash0-2} of the fake text tables: keenglish, De_De and En_Us.
constexpr uint32_t kKeHash[4] = {0x30, 1, 2, 3}, kDeHash[4] = {0x30, 4, 5, 6}, kEnHash[4] = {0x40, 7, 8, 9};

// One entry of the content map: where a content hash's data starts.
struct FakeContent {
    const uint32_t* hash;
    uint32_t off;
    uint16_t container;
};

// A KFC4 index with just the parts res::load reads: one resource location, the keys and the values, then four
// containers and the content map.
std::vector<uint8_t> kfcIndex(const std::vector<res::Entry>& es, uint32_t total, const std::vector<FakeContent>& cs) {
    size_t ko = 0xa0, vo = ko + 32 * es.size(), co = vo + 8 * es.size(), hk = co + 16 * 4, hv = hk + 16 * cs.size();
    std::vector<uint8_t> f(hv + 16 * cs.size());
    memcpy(f.data(), "KFC4", 4);
    auto loc = [&](int i, size_t at, size_t count) {
        uint32_t v[2] = {uint32_t(at - (16 + 8 * i)), uint32_t(count)};  // offset from the field itself
        memcpy(&f[16 + 8 * i], v, 8);
    };
    const uint32_t where[3] = {total, total, uint32_t(es.size())};
    memcpy(&f[0x90], where, 12);
    loc(4, 0x90, 1);
    loc(10, ko, es.size());
    loc(11, vo, es.size());
    for (size_t i = 0; i < es.size(); i++) {
        const uint32_t key[4] = {es[i].type, es[i].part, 0, 0}, val[2] = {es[i].off, es[i].size};
        memcpy(&f[ko + 32 * i], es[i].guid, 16);
        memcpy(&f[ko + 32 * i + 16], key, 16);
        memcpy(&f[vo + 8 * i], val, 8);
    }
    loc(1, co, 4);
    loc(7, hk, cs.size());
    loc(8, hv, cs.size());
    for (size_t i = 0; i < cs.size(); i++) {
        memcpy(&f[hk + 16 * i], cs[i].hash, 16);
        memcpy(&f[hv + 16 * i], &cs[i].off, 4);
        memcpy(&f[hv + 16 * i + 6], &cs[i].container, 2);
    }
    return f;
}

// A LocaTagCollectionResourceData blob: 24-byte LocaTagResources {id, text, arguments, genericArguments}, then the
// texts.
std::vector<uint8_t> textTable(const std::vector<std::pair<uint32_t, std::string>>& tags) {
    std::vector<uint8_t> d(8 + 24 * tags.size());
    const uint32_t head[2] = {8, uint32_t(tags.size())};
    memcpy(d.data(), head, 8);
    for (size_t i = 0; i < tags.size(); i++) {
        size_t r = 8 + 24 * i;
        const uint32_t text[2] = {uint32_t(d.size() - (r + 4)), uint32_t(tags[i].second.size())};
        memcpy(&d[r], &tags[i].first, 4);
        memcpy(&d[r + 4], text, 8);
        d.insert(d.end(), tags[i].second.begin(), tags[i].second.end());
    }
    return d;
}

uint32_t fieldOff(refl::Type t, const char* name) {
    refl::Field f;
    CHECK(refl::field(t, name, f));
    return f.offset;
}

// A blob header {rel, count} at offset at of base, for data at offset to.
void putBlob(uint8_t* base, uint32_t at, uint32_t to, uint32_t count) {
    const uint32_t h[2] = {to - at, count};
    memcpy(base + at, h, 8);
}

// The LocaTagCollectionResource at l: keenglish, then the languages De_De and En_Us, each {language, data}.
void fakeLoca(uint8_t* l, refl::Type lc) {
    memcpy(l + fieldOff(lc, "keenglishData"), kKeHash, 16);
    putBlob(l, fieldOff(lc, "languages"), 40, 2);
    const uint32_t de = 3, en = 1;  // LanguageId
    memcpy(l + 40, &de, 4);
    memcpy(l + 48, kDeHash, 16);
    memcpy(l + 80, &en, 4);
    memcpy(l + 88, kEnHash, 16);
}

// Resources of real types laid out the way the game's are: an ItemInfo (GA) with a name, an English name (GN), a stack
// size, a tag array and a pidEntity reference to the TemplateResource (GB), whose components array holds one Health
// variant; a second part of GA has nothing set. Then the LocaTagCollectionResource (GA's GUID, its own type).
std::vector<uint8_t> fakeResources(const uint8_t* ga, const uint8_t* gb, const uint8_t* gn, std::vector<res::Entry>& es) {
    refl::Type item = refl::byName("keen::ItemInfo"), tmpl = refl::byName("TemplateResource"), hp = refl::byName("Health");
    refl::Type lc = refl::byName("keen::LocaTagCollectionResource");
    CHECK(item && item.size() == 0x828 && tmpl && tmpl.size() == 24 && lc && lc.size() == 40);
    std::vector<uint8_t> d(0x20a8);
    uint8_t *a = d.data(), *b = d.data() + 0x1000;
    memcpy(a + fieldOff(item, "name"), gn, 16);
    fakeLoca(d.data() + 0x2030, lc);
    putBlob(a, fieldOff(item, "debugName"), 0x828, 13);
    memcpy(a + 0x828, "Test_Wood_Log", 13);
    *reinterpret_cast<uint32_t*>(a + fieldOff(item, "itemId")) = 0x5eed;
    *reinterpret_cast<uint16_t*>(a + fieldOff(item, "maxStackSize")) = 50;
    putBlob(a, fieldOff(item, "tags"), 0x838, 2);  // two 24-byte ItemTagWeights
    *reinterpret_cast<uint32_t*>(a + 0x850 + 16) = 7;  // [1].nominator
    memcpy(a + fieldOff(item, "pidEntity"), gb, 16);
    putBlob(b, fieldOff(tmpl, "name"), 24, 9);
    memcpy(b + 24, "Wolf_Test", 9);
    putBlob(b, fieldOff(tmpl, "components"), 40, 1);
    const uint32_t variant[3] = {hp.hash(), 56 - 44, hp.size()};  // rel counts from its own field at 44
    memcpy(b + 40, variant, 12);
    *reinterpret_cast<int32_t*>(b + 56 + 8) = 77;  // Health.dataStorage[0]
    es = {{{}, item.hash(), 1, 0x1800, 0x828}, {{}, tmpl.hash(), 0, 0x1000, 96}, {{}, item.hash(), 0, 0, 0x868},
          {{}, lc.hash(), 0, 0x2030, 120}};
    memcpy(es[0].guid, ga, 16);
    memcpy(es[1].guid, gb, 16);
    memcpy(es[2].guid, ga, 16);
    memcpy(es[3].guid, ga, 16);
    return d;
}

const char* kResChecks = R"lua(
    local a = Res.get(GA, "ItemInfo")
    assert(a.debugName == "Test_Wood_Log" and a.maxStackSize == 50 and a.exchangeOnBroken == nil, "item fields")
    assert(#a.tags == 2 and a.tags[1].nominator == 7 and tostring(a.tags):sub(1, 1) == "{", "blob array")
    assert(a.pidEntity == GB, "reference")
    local t = Res.get(a.pidEntity)
    assert(t and t.name == "Wolf_Test" and #t.components == 1 and t.components[0].dataStorage[0] == 77, "variant")
    assert(Res.get(GA, "ItemInfo", 1).debugName == "" and Res.get(GA, "TemplateResource") == nil, "part and type")
    local n, parts = 0, 0
    for g, r, p in Res.each("ItemInfo") do n, parts = n + 1, parts + p assert(g == GA and r.maxStackSize) end
    assert(n == 2 and parts == 1, "each")
    local g, r = Res.find("TemplateResource", "wolf_TEST")
    assert(g == GB and r.name == "Wolf_Test" and Res.find("ItemInfo", "nope") == nil, "find")
    assert(not pcall(Res.get, GA, "Nope") and not pcall(Res.get, "xyz") and not pcall(Res.each, "Health"), "bad calls")
    assert(not pcall(function() return a.tags[2] end), "index past the end")
    a.maxStackSize = 99 a.tags[1].nominator = 9 a.exchangeOnBroken = GB
    assert(a.maxStackSize == 99 and a.tags[1].nominator == 9 and a.exchangeOnBroken == GB, "writes")
    a.exchangeOnBroken = nil
    assert(a.exchangeOnBroken == nil and not pcall(function() a.exchangeOnBroken = "nope" end), "GUID writes")
    assert(not pcall(function() a.maxStackSize = 70000 end) and a.maxStackSize == 99, "range")
    assert(not pcall(function() a.debugName = "x" end) and not pcall(function() a.tags = 1 end), "blobs are fixed")
    a.maxStackSize = 50
)lua";

// Admin.spawn of the fake TemplateResource gb by name: the spawn action at PlayerInput + 0x3d8 gets position (+8, the
// player's + 5 m on x), level (+0x20), templateGuid (+0x24) and count (+0x35); its counter is at + 0xa4.
void checkSpawn(FakeWorld& w, const uint8_t* gb, const char* itemGuid) {
    w.addTransform();
    uint8_t *in = w.input(), *con = w.consumed();
    rt::queue("Admin.spawn('wolf_TEST', 2, 7)");
    rt::tick(w.addr());
    rt::applyAdmin(nullptr);
    CHECK(i64At(in + 0x3e0) == 15ll << 32 && i64At(in + 0x3e8) == 2ll << 32 && i64At(in + 0x3f0) == -3ll << 32);
    CHECK(*reinterpret_cast<uint16_t*>(in + 0x3f8) == 7 && memcmp(in + 0x3fc, gb, 16) == 0 && in[0x40d] == 2);
    CHECK(u32At(con + 0xa4) == 1);
    u32At(con + 0xa4) = 0;
    rt::queue("print('sp1', pcall(Admin.spawn, 'nope'))");
    rt::queue(std::string("print('sp2', pcall(Admin.spawn, '") + itemGuid + "'))");  // an ItemInfo, not a template
    rt::queue("print('sp3', pcall(Admin.spawn, 'wolf_test', 0))");
    rt::tick(w.addr());
    rt::applyAdmin(nullptr);
    CHECK(u32At(con + 0xa4) == 0);  // the refused calls queued nothing
    CHECK(gOut.find("sp1\tfalse\t") != std::string::npos && countOf(gOut, "no template has this GUID or name") == 2);
    CHECK(gOut.find("sp2\tfalse\t") != std::string::npos);
    CHECK(gOut.find("sp3\tfalse\t") != std::string::npos && gOut.find("must be from 1 to 255") != std::string::npos);
    printf("spawn ok\n");
}

// Admin.give of the fake ItemInfo (item id 0x5eed, stack size 50) through inventory_actions' hook (sys 1): the create
// action at PlayerInput + 0x3c0 gets itemId, hasItemRarity (+0xa), rarity (+0xb), level (+0xc) and createFullStack
// (+0xe); its counter is at + 0xa0. 52 items: a full stack, then one item per tick.
void checkGive(FakeWorld& w) {
    uint8_t *in = w.input(), *con = w.consumed();
    rt::queue("print('gv0', pcall(Admin.give, 'test_wood_log'))");
    rt::tick(w.addr());
    CHECK(gOut.find("gv0\tfalse\tAdmin.give is unavailable") != std::string::npos);
    rt::setGiveAvailable(true);
    rt::queue("Admin.give('test_wood_log', 52, 7, 'Epic')");
    rt::tick(w.addr());
    rt::applyAdmin(nullptr, 0);  // the admin consumer: not its command
    CHECK(u32At(in + 0x3c0) == 0 && u32At(con + 0xa0) == 0);
    rt::applyAdmin(nullptr, 1);
    CHECK(u32At(in + 0x3c0) == 0x5eed && in[0x3ca] == 1 && in[0x3cb] == 3 && in[0x3cc] == 7 && in[0x3ce] == 1);
    CHECK(u32At(con + 0xa0) == 1);
    for (int i = 0; i < 2; i++) {
        u32At(con + 0xa0) = u32At(in + 0x3c4);  // the game ran it
        rt::tick(w.addr());
        rt::applyAdmin(nullptr, 1);
        CHECK(in[0x3ce] == 0 && u32At(con + 0xa0) == u32At(in + 0x3c4) + 1);
    }
    u32At(con + 0xa0) = u32At(in + 0x3c4);
    memset(in + 0x3c0, 0, 20);
    rt::tick(w.addr());
    rt::applyAdmin(nullptr, 1);
    CHECK(u32At(in + 0x3c0) == 0);  // all 52 given
    rt::queue("print('gv1', pcall(Admin.give, 'nope'))");
    rt::queue("print('gv2', pcall(Admin.give, 'test_wood_log', 1000))");
    rt::queue("print('gv3', pcall(Admin.give, 'test_wood_log', 1, 1, 'Shiny'))");
    rt::queue("Admin.give('test_wood_log', 3)");  // and the game won't run it
    rt::tick(w.addr());
    CHECK(gOut.find("gv1\tfalse\t") != std::string::npos && gOut.find("no item has this GUID or name") != std::string::npos);
    CHECK(gOut.find("gv2\tfalse\t") != std::string::npos && gOut.find("must be from 1 to 999") != std::string::npos);
    CHECK(gOut.find("gv3\tfalse\t") != std::string::npos && gOut.find("not a value of keen::ItemRarity") != std::string::npos);
    rt::applyAdmin(nullptr, 1);
    CHECK(u32At(in + 0x3c0) == 0x5eed && u32At(con + 0xa0) == u32At(in + 0x3c4) + 1);
    rt::tick(w.addr());
    CHECK(gOut.find("Admin command adminInventoryCreateAction: not run by the game") != std::string::npos);
    memset(in + 0x3c0, 0, 20);
    rt::queue("Admin.send('adminRepairAllItemsAction', {})");  // raw, through the same system: version at + 0x598
    rt::tick(w.addr());
    rt::applyAdmin(nullptr, 1);
    CHECK(u32At(in + 0x3c0) == 0 && u32At(con + 0xa0) == u32At(in + 0x3c4));  // the give was dropped, not sent again
    CHECK(u32At(con + 0xd4) == u32At(in + 0x598) + 1);
    u32At(con + 0xd4) = u32At(in + 0x598);
    printf("give ok\n");
}

// English names: where the En_Us table is (the fake LocaTagCollectionResource lists De_De first), the table checks,
// then Res.text, the Resources window's name lookup, Res.find and Admin.give by English name.
void checkNames(FakeWorld& w, const uint8_t* gn, const char* sa) {
    uint32_t c = 0, off = 0, size = 0;
    // Still streaming in: the data checks out, but the text collection is zeroes. Not found yet (main.cpp looks again).
    uint8_t* loca = res::data(*res::ofType(refl::byName("keen::LocaTagCollectionResource").hash())[0]);
    std::vector<uint8_t> keep(loca, loca + 120);
    memset(loca, 0, keep.size());
    CHECK(res::ready() && !res::textSource(c, off, size));
    memcpy(loca, keep.data(), keep.size());
    CHECK(res::textSource(c, off, size) && c == 3 && off == 0x2000 && size == 0x40);
    refl::Type lt = refl::byName("keen::LocaTagResource");
    CHECK(lt && lt.size() == 24 && fieldOff(lt, "id") == 0 && fieldOff(lt, "text") == 4);
    uint32_t id = refl::fnv({reinterpret_cast<const char*>(gn), 16});
    CHECK(!res::setText(textTable({{id, "Test Logs"}, {id - 1, "x"}})));  // ids not ascending
    std::vector<uint8_t> oob = textTable({{id - 1, "x"}, {id, "Test Logs"}});
    oob[8 + 24 + 8] = 0xff;  // the second text's length
    CHECK(!res::setText(oob) && res::textCount() == 0 && res::text(gn).empty());
    CHECK(res::setText(textTable({{id - 1, "x"}, {id, "Test Logs"}})) && res::textCount() == 2);
    CHECK(res::text(gn) == "Test Logs");
    uint8_t ga[16];
    CHECK(res::parseGuid(sa, ga) && res::nameText(*res::find(ga, res::typeByName("ItemInfo").hash())) == "Test Logs");
    std::string g = sa;
    rt::queue("local g, it = Res.find('ItemInfo', 'test LOGS') print('nm1', g == '" + g + "', Res.text(it.name))");
    rt::queue("print('nm2', Res.text('00000000-0000-0000-0000-000000000000'), pcall(Res.text, 'x'))");
    rt::queue("Admin.give('Test Logs')");
    rt::tick(w.addr());
    CHECK(gOut.find("nm1\ttrue\tTest Logs\n") != std::string::npos && gOut.find("nm2\tnil\tfalse\t") != std::string::npos);
    uint8_t* in = w.input();
    memset(in + 0x3c0, 0, 20);
    rt::applyAdmin(nullptr, 1);
    CHECK(u32At(in + 0x3c0) == 0x5eed);
    printf("names ok\n");
}

// The same resources on read-only pages, as the game keeps them after loading: an edit lifts the protection for its
// own write, puts it back, and says so once. Then back to data.
void checkReadOnly(lua_State* L, std::vector<uint8_t>& data) {
    auto ro = static_cast<uint8_t*>(VirtualAlloc(nullptr, data.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    CHECK(ro);
    memcpy(ro, data.data(), data.size());
    DWORD old;
    CHECK(VirtualProtect(ro, data.size(), PAGE_READONLY, &old));
    CHECK(res::attach(reinterpret_cast<uintptr_t>(ro)));
    rt::exec(L, "local a = Res.get(GA, 'ItemInfo') a.maxStackSize = 31 a.maxStackSize = 32 print('ro', a.maxStackSize)");
    CHECK(gOut.find("ro\t32\n") != std::string::npos && countOf(gOut, "the game keeps them read-only (protect 0x2)") == 1);
    CHECK(*reinterpret_cast<uint16_t*>(ro + fieldOff(refl::byName("keen::ItemInfo"), "maxStackSize")) == 32);
    MEMORY_BASIC_INFORMATION mi;
    CHECK(VirtualQuery(ro, &mi, sizeof mi) && mi.Protect == PAGE_READONLY);
    CHECK(res::attach(reinterpret_cast<uintptr_t>(data.data())));
    VirtualFree(ro, 0, MEM_RELEASE);
}

// The resource index and data: parsing, attaching, lookups, GUID text, then Res from Lua and from a Draw callback.
void checkResources(FakeWorld& w) {
    uint8_t v[16];
    const uint8_t want[16] = {0x33, 0x22, 0x11, 0x00, 0x55, 0x44, 0x77, 0x66,
                              0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};  // a GUID with every byte distinct
    CHECK(res::parseGuid("00112233-4455-6677-8899-AABBCCDDEEFF", v) && memcmp(v, want, 16) == 0);
    CHECK(res::guidStr(want) == "00112233-4455-6677-8899-aabbccddeeff");
    CHECK(!res::parseGuid("0011223-44556-6677-8899-aabbccddeeff", v) && !res::parseGuid("00112233-4455-6677-8899-aabbccddeefg", v));
    const char *sa = "11111111-2222-3333-4444-555555555555", *sb = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
    uint8_t ga[16], gb[16], gn[16];
    CHECK(res::parseGuid(sa, ga) && res::parseGuid(sb, gb) && res::parseGuid("12345678-9abc-def0-1234-56789abcdef0", gn));
    std::vector<res::Entry> es;
    static std::vector<uint8_t> data = fakeResources(ga, gb, gn, es);  // static: Res stays attached to it
    const std::vector<FakeContent> cs = {{kKeHash, 0x1000, 2}, {kDeHash, 0x3000, 1}, {kEnHash, 0x2000, 3}};
    std::vector<uint8_t> kfc = kfcIndex(es, uint32_t(data.size()), cs), bad = kfc;
    bad[0] = 'X';
    CHECK(!res::load(bad.data(), bad.size()));
    bad = kfc;
    bad[16 + 8 * 10 + 4] = 0xff;  // more keys than the file holds
    CHECK(!res::load(bad.data(), bad.size()));
    CHECK(res::load(kfc.data(), kfc.size()) && res::count() == 4 && !res::ready());
    std::vector<uint8_t> zeros(data.size());
    CHECK(!res::attach(reinterpret_cast<uintptr_t>(zeros.data())));  // all strings empty: not loaded yet
    CHECK(res::attach(reinterpret_cast<uintptr_t>(data.data())) && res::ready());
    refl::Type item = res::typeByName("ItemInfo");
    CHECK(item && item.p == res::typeByName("keen::ItemInfo").p && res::typeByName("TemplateResource") && !res::typeByName("Health"));
    const res::Entry *a = res::find(ga, item.hash()), *b = res::find(gb, 0), *a1 = res::find(ga, item.hash(), 1);
    CHECK(a && a->off == 0 && b && b->off == 0x1000 && a1 && a1->off == 0x1800 && !res::find(gb, item.hash()));
    CHECK(res::label(*a) == "Test_Wood_Log" && res::label(*b) == "Wolf_Test" && res::label(*a1).empty());
    CHECK(res::types().size() == 3 && res::ofType(item.hash()).size() == 2);
    // Group by: the game's own enum fields, top level and one struct down, read only inside the resource.
    std::vector<res::EnumField> fs = res::enumFields(item);
    auto named = [&](const char* p) {
        auto it = std::find_if(fs.begin(), fs.end(), [&](const res::EnumField& f) { return f.path == p; });
        return it == fs.end() ? nullptr : &*it;
    };
    const res::EnumField* cat = named("category");
    CHECK(cat && cat->offset == fieldOff(item, "category") && named("equipment.slot") && named("rarity") && !named("itemId"));
    data[cat->offset] = 2;  // Weapons
    uint64_t cv = 0;
    res::Entry cut = *a;
    cut.size = cat->offset;
    CHECK(res::enumValue(*a, *cat, cv) && cv == 2 && !res::enumValue(cut, *cat, cv));
    lua_State* L = rt::open();
    lua_pushstring(L, sa);
    lua_setglobal(L, "GA");
    lua_pushstring(L, sb);
    lua_setglobal(L, "GB");
    if (luaL_dostring(L, kResChecks) != LUA_OK) {
        printf("FAIL lua: %s\n", lua_tostring(L, -1));
        exit(1);
    }
    refl::Type it = refl::byName("keen::ItemInfo");
    CHECK(*reinterpret_cast<uint16_t*>(data.data() + fieldOff(it, "maxStackSize")) == 50 && data[0x850 + 16] == 9);
    checkReadOnly(L, data);
    lua_close(L);
    ImGui::CreateContext();
    ImGui::GetIO().DisplaySize = ImVec2(800, 600);
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGui::GetIO().IniFilename = nullptr;
    rt::queue(std::string("Draw.add(function() RESD = Res.get('") + sa + "', 'ItemInfo').maxStackSize end)");
    rt::tick(w.addr());
    drawFrame();
    rt::queue("print('resdraw', RESD)");
    rt::tick(w.addr());
    ImGui::DestroyContext();
    CHECK(gOut.find("resdraw\t50\n") != std::string::npos);
    printf("resources ok\n");
    checkSpawn(w, gb, sa);
    checkGive(w);
    checkNames(w, gn, sa);
}

// The Game Log's noise filter on rows from a real enshrouded.log, and the launch flag on this exe's own imports.
void checkStartup() {
    for (const char* row : {"[I 00:00:02,744]        ''::Enter #0", "[I 00:00:08,464]  triggered ''", "[I 00:00:08,464] ",
                            "[I 00:00:08,464]", "[I 00:00:08,498]\t''::Exit #0", "[I 00:00:08,481]        'X'::Execute #3"})
        CHECK(logNoise(row));
    for (const char* row : {"[I 00:00:08,464] [GameUi] Entered PreGame", "[I 00:00:00,109] }",
                            "[I 00:00:02,427] [render] async compute is disabled", "[W 00:00:01,000] x::Enter #0", "["})
        CHECK(!logNoise(row));
    std::wstring w = GetCommandLineW();
    std::string a = GetCommandLineA();
    CHECK(addLaunchFlag(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), "--selftest-flag"));
    CHECK(GetCommandLineW() == w + L" --selftest-flag" && GetCommandLineA() == a + " --selftest-flag");
    printf("startup ok\n");
}

// Offset of a field path like {"graphics", "render", "antiAliasing"} in t.
uint32_t pathOff(refl::Type t, std::initializer_list<const char*> path) {
    uint32_t off = 0;
    for (const char* name : path) {
        refl::Field f;
        CHECK(refl::field(t, name, f));
        off += f.offset;
        t = f.type.resolved();
    }
    return off;
}

uint32_t rd32(uintptr_t a) {
    uint32_t v;
    memcpy(&v, reinterpret_cast<const void*>(a), 4);
    return v;
}

// The dump of the fake options below, with a hosting password planted where a real one would be.
void checkOptionsDump(uint8_t* u, refl::Type ut) {
    static const char kSecret[] = "secret-pw";
    const char* sp = kSecret;
    uint64_t len = sizeof kSecret - 1;
    uint32_t pw = pathOff(ut, {"hosting", "inviteOnlyMode", "password"});
    memcpy(u + pw, &sp, 8);
    memcpy(u + pw + 8, &len, 8);
    std::string all;
    size_t n = opts::dump("", [&](const std::string& s) { all += s + '\n'; });
    CHECK(n > 100 && countOf(all, "\n") == n);
    CHECK(all.find("machine.graphics.render.antiAliasing = Dlss (4)\n") != std::string::npos);  // the default
    CHECK(all.find("user.gameplay.hideHUD = true (default false)\n") != std::string::npos);
    CHECK(all.find("machine.graphics.renderScale = 0 (default 1)\n") != std::string::npos);
    CHECK(all.find("user.graphics.fov = 90 (default 65)\n") != std::string::npos);  // named like the game's own log
    CHECK(all.find("(default unknown)") == std::string::npos);  // every value found its default
    CHECK(all.find("secret") == std::string::npos && all.find(".password") == std::string::npos &&
          all.find("bannedAccounts") == std::string::npos);
    size_t g = opts::dump("user.gameplay", [](const std::string& s) { CHECK(s.starts_with("user.gameplay.")); });
    CHECK(g > 40 && g < n);
}

// Whole structs into the fake settings: a table, a value of the same type, and the refusals.
void checkOptionsAssign(lua_State* L, const uint8_t* m, refl::Type mt, const uint8_t* u, refl::Type ut) {
    rt::exec(L, "local g = GameOptions.user().graphics g.fov = 90 local s = Types.copy(g) g.fov = 100 "
                "GameOptions.user().graphics = s print('fov', GameOptions.user().graphics.fov)");
    float fov;
    memcpy(&fov, u + pathOff(ut, {"graphics", "fov"}), 4);
    CHECK(gOut.find("fov\t90.0\n") != std::string::npos && fov == 90);
    uint32_t wx = pathOff(mt, {"graphics", "windowSize", "x"}), wy = pathOff(mt, {"graphics", "windowSize", "y"});
    uint32_t fx = pathOff(mt, {"graphics", "forceFullscreenResolution", "x"});
    uint32_t fy = pathOff(mt, {"graphics", "forceFullscreenResolution", "y"});
    rt::exec(L, "local g = GameOptions.machine().graphics g.windowSize = {x = 5, y = 6} g.forceFullscreenResolution = g.windowSize");
    CHECK(rd32(uintptr_t(m + wx)) == 5 && rd32(uintptr_t(m + wy)) == 6 && rd32(uintptr_t(m + fx)) == 5 &&
          rd32(uintptr_t(m + fy)) == 6);
    size_t mismatch = countOf(gOut, "can't assign a keen::int2 to a keen::uint2");
    rt::exec(L, "local g = GameOptions.machine().graphics g.windowSize = g.windowPosition");
    CHECK(countOf(gOut, "can't assign a keen::int2 to a keen::uint2") == mismatch + 1);
    size_t copies = countOf(gOut, "strings or arrays of variable size");
    rt::exec(L, "GameOptions.user().hosting = GameOptions.user().hosting");  // holds the hosting passwords
    rt::exec(L, "Types.copy(GameOptions.user())");
    CHECK(countOf(gOut, "strings or arrays of variable size") == copies + 2);
}

uintptr_t gPushedOpts;
uint8_t* gPushedSlot;
int gPushes;
void fakeCamPush(uintptr_t options, uint8_t* slot) {
    gPushedOpts = options;
    gPushedSlot = slot;
    gPushes++;
}

// A field of view set from Lua reaches the camera through one push of the user's slot; an unchanged one pushes nothing.
void checkFovPush(lua_State* L, uintptr_t app, const uint8_t* options, uint8_t* slot) {
    opts::setCamPush(reinterpret_cast<uintptr_t>(&fakeCamPush));
    opts::frame(app);  // the first frame hands the camera whatever is there
    int n = gPushes;
    CHECK(!opts::frame(app) && gPushes == n);
    rt::exec(L, "GameOptions.user().graphics.fov = 110");
    CHECK(!opts::frame(app) && gPushes == n + 1 && gPushedOpts == uintptr_t(options) && gPushedSlot == slot);
    CHECK(!opts::frame(app) && gPushes == n + 1);
    opts::setCamPush(0);
}

// The game's options: the hook's pattern and the getters' offsets on this exe, then a fake options object laid out the
// same way: attaching, GameOptions from Lua, the dump, the field of view push and the antialiasing switch.
void checkOptions(uintptr_t base) {
    CHECK(findOptionsUpdate(base) == base + 0x866D80);
    CHECK(findCamPush(base) == base + 0xE24F00);
    CHECK(memcmp(reinterpret_cast<const void*>(base + 0xE25750), "\x48\x8D\x81\xC0\x09\x00\x00\xC3", 8) == 0);  // kMachine
    // The user-slot getter at 0xE259E0: imul 0x308, add 0xa68, has-value at settings + 0x2f8.
    CHECK(opts::findLayout(base) && opts::slotSize == 0x308 && opts::slotsAt == 0xa68 && opts::hasAt == 0x300);
    refl::Type mt = opts::machineType(), ut = opts::userType();
    CHECK(mt && ut);
    static std::vector<uint8_t> o(opts::slotsAt + 16 * opts::slotSize);  // static: opts stays attached to it
    uint8_t *m = o.data() + opts::kMachine, *slot = o.data() + opts::slotsAt + opts::slotSize, *u = slot + 8;
    uint32_t aa = pathOff(mt, {"graphics", "render", "antiAliasing"}), hud = pathOff(ut, {"gameplay", "hideHUD"});
    m[0] = 13;
    m[aa] = 99;
    uintptr_t app[0x30] = {};
    app[opts::kAppOptions / 8] = reinterpret_cast<uintptr_t>(o.data());
    auto at = reinterpret_cast<uintptr_t>(app);
    const char* why = opts::frame(at);
    CHECK(why && std::string(why) == "machine field antiAliasing = 99" && !opts::ready());
    lua_State* L = rt::open();
    rt::exec(L, "GameOptions.machine()");
    CHECK(gOut.find("game options not found yet") != std::string::npos);
    m[aa] = 3;
    CHECK(!opts::frame(at) && opts::ready() && opts::machine() == m && !opts::user());
    *reinterpret_cast<uint32_t*>(slot) = 1;
    slot[opts::hasAt] = 1;
    CHECK(opts::user() == u);
    rt::exec(L, "local m = GameOptions.machine() m.graphics.render.antiAliasing = 'None' print('aa', m.graphics.render.antiAliasing)");
    CHECK(gOut.find("aa\t0\n") != std::string::npos && m[aa] == 0);
    rt::exec(L, "GameOptions.machine().graphics.render.antiAliasing = 'dlss' GameOptions.user().gameplay.hideHUD = true");
    CHECK(m[aa] == 4 && u[hud] == 1);
    size_t refused = countOf(gOut, "not a value of");
    rt::exec(L, "GameOptions.machine().graphics.render.antiAliasing = 'Bogus'");
    rt::exec(L, "GameOptions.machine().graphics.render.antiAliasing = 99");
    CHECK(countOf(gOut, "not a value of") == refused + 2 && m[aa] == 4);
    checkOptionsAssign(L, m, mt, u, ut);
    checkOptionsDump(u, ut);
    size_t before = gOut.size();  // Lua gets a Ds string's type name, never the planted password
    rt::exec(L, "print('pw', GameOptions.user().hosting.inviteOnlyMode.password)");
    CHECK(gOut.find("pw\t<", before) != std::string::npos && gOut.find("secret", before) == std::string::npos);
    size_t r = opts::dump("machine.graphics.render", [](const std::string&) {});
    rt::exec(L, "print('dumped', GameOptions.dump('machine.graphics.render'))");
    CHECK(gOut.find("dumped\t" + std::to_string(r) + "\n") != std::string::npos && r >= 21);
    checkFovPush(L, at, o.data(), slot);
    lua_close(L);
    m[aa] = 3;
    opts::setAaOff(true);
    CHECK(!opts::frame(at) && m[aa] == 0 && !opts::frame(at) && m[aa] == 0);
    m[aa] = 4;  // the game's menu applies another choice while it's off
    CHECK(!opts::frame(at) && m[aa] == 0);
    opts::setAaOff(false);
    CHECK(!opts::frame(at) && m[aa] == 4 && !opts::frame(at) && m[aa] == 4);
    printf("game options ok\n");
}

// The profiler on fake jobs: nothing counts while it's off; on, calls, time and world kinds add up per system, most
// time first; starting again resets. Then the same through Lua.
void checkProfiler(lua_State* L, const void* jobInv, const void* jobAdm) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    int64_t ms = f.QuadPart / 1000;  // QPC ticks in a millisecond
    sys::record(jobInv, 0, 5 * ms);
    CHECK(sys::stats().empty() && !sys::profiling());
    sys::setProfiling(true);
    sys::record(jobInv, 0, 2 * ms);
    sys::record(jobInv, 2, 3 * ms);
    sys::record(jobAdm, 0, 9 * ms);
    auto st = sys::stats();
    CHECK(st.size() == 2 && st[0].sys->name == "admin_player" && st[0].calls == 1 && st[0].kinds == 1);
    CHECK(st[1].sys->name == "inventory_actions" && st[1].calls == 2 && st[1].kinds == 5);
    CHECK(st[0].ms > 8.9 && st[0].ms < 9.1 && st[1].ms > 4.9 && st[1].ms < 5.1);
    CHECK(std::string(sys::worldName(st[1].kinds)) == "both" && std::string(sys::worldName(4)) == "client" &&
          std::string(sys::worldName(1)) == "server");
    sys::setProfiling(true);
    CHECK(sys::stats().empty());
    rt::exec(L, "Systems.profile(false) Systems.profile(true)");
    sys::record(jobAdm, 2, 4 * ms);
    rt::exec(L, "local s, secs = Systems.stats() print('prof', #s, s[1].name, s[1].calls, s[1].world, secs >= 0)");
    CHECK(gOut.find("prof\t1\tadmin_player\t1\tclient\ttrue\n") != std::string::npos);
    rt::exec(L, "Systems.profile(false)");
    sys::record(jobAdm, 0, ms);
    CHECK(!sys::profiling() && sys::stats().size() == 1 && sys::stats()[0].calls == 1);
}

// The game's systems: the runner's pattern and the descriptor scan on this exe, then switching one off from Lua and the
// runner's skip test on fake jobs laid out as the runner reads them.
void checkSystems(uintptr_t base) {
    CHECK(findRunner(base) == base + 0x9A2020);
    CHECK(sys::scan(base) == 861);
    const sys::System *inv = sys::find("inventory_actions"), *adm = sys::find("admin_player");
    CHECK(inv && inv->fn == base + 0x3FD610 && adm && adm->fn == base + 0x2CAE30 && !sys::find("no_such_system"));
    CHECK(std::is_sorted(sys::all().begin(), sys::all().end(), [](auto& a, auto& b) { return a.name < b.name; }));
    uintptr_t progInv[6] = {}, progAdm[6] = {};
    progInv[5] = inv->fn;
    progAdm[5] = adm->fn;
    uintptr_t jobInv[3] = {0, 0, uintptr_t(progInv)}, jobAdm[3] = {0, 0, uintptr_t(progAdm)};
    CHECK(!sys::skip(jobInv) && !sys::skip(jobAdm) && sys::offCount() == 0);
    lua_State* L = rt::open();
    rt::exec(L, "Systems.enable('inventory_actions', false)");
    CHECK(gOut.find("Systems is unavailable") != std::string::npos && sys::isOn(*inv));
    rt::setSystemsAvailable(true);
    rt::exec(L, "Systems.enable('inventory_actions', false) Systems.enable('inventory_actions', false) "
                "print('sys', Systems.enabled('inventory_actions'), Systems.enabled('admin_player'), #Systems.list('inventory'))");
    CHECK(gOut.find("sys\tfalse\ttrue\t") != std::string::npos && sys::offCount() == 1);
    CHECK(sys::skip(jobInv) && !sys::skip(jobAdm));
    rt::exec(L, "Systems.enable('no_such_system', false)");
    CHECK(gOut.find("unknown system 'no_such_system'") != std::string::npos);
    rt::exec(L, "Systems.enable('inventory_actions', true)");
    CHECK(!sys::skip(jobInv) && sys::offCount() == 0 && sys::isOn(*inv));
    checkProfiler(L, jobInv, jobAdm);
    lua_close(L);
    rt::setSystemsAvailable(false);
    printf("systems ok\n");
}

__declspec(noinline) uint64_t luaHookTarget(uint32_t n, const void* hp) {
    return uint64_t(n) * 31 + (hp != nullptr) * 7 + 13;
}
auto volatile callLuaHook = &luaHookTarget;
const void* gHookTestHp = nullptr;

void hookTestSink(const char* line) {
    sink(line);
    if (strcmp(line, "emit hook") == 0) callLuaHook(17, gHookTestHp);
}

std::string luaHookPattern() {
    std::string pattern;
    for (int i = 0; i < 24; ++i) {
        char b[4];
        snprintf(b, sizeof b, "%02X ", reinterpret_cast<uint8_t*>(&luaHookTarget)[i]);
        pattern += b;
    }
    return pattern;
}

void checkLuaHooks(FakeWorld& w) {
    std::string mods = freshMods(), pattern = luaHookPattern();
    std::string script = "local pat = '" + pattern + R"lua('
        local m = {count = 0}
        local opts = {name='values', pattern=pat, args={'u32','Health*'}, queue=2}
        opts.fn = function(ev)
            assert(ev.seq > 0 and ev.time >= 0 and ev.args[1] == 17)
            m.count = m.count + 1; m.last = ev
            print('hook event', ev.args[2].dataStorage[0], ev.dropped)
            ev.args[2].dataStorage[0] = 999
        end
        m.id = Hooks.observe(opts)
        m.add = function() return Hooks.observe(opts) end
        m.remove = function(id) Hooks.remove(id) end
        Tick.add(function() if m.last then assert(m.last.args[2].dataStorage[0] == 999) end end)
        Draw.add(function() assert(not pcall(m.add)) end)
        registerForEvent('onShutdown', function() assert(not pcall(m.add)) end)
        return m
    )lua";
    writeFile(mods + "\\hookmod", "init.lua", script.c_str());
    rt::requestReload();
    rt::tick(w.addr());
    CHECK(gOut.find("Mod loaded: hookmod") != std::string::npos);
    std::vector<uint8_t> hp(refl::lookup("Health").size());
    *reinterpret_cast<int32_t*>(hp.data() + 8) = 37;
    for (int i = 0; i < 5; ++i) CHECK(callLuaHook(17, hp.data()) == 547);
    rt::tick(w.addr());
    CHECK(gOut.find("hook event\t37\t3\n") != std::string::npos);
    CHECK(*reinterpret_cast<int32_t*>(hp.data() + 8) == 37);
    rt::queue("local m=GetMod('hookmod') assert(m.count==2) assert(not pcall(m.add)) m.remove(m.id) m.remove(m.id)");
    rt::tick(w.addr());
    size_t bytes = hk::retainedBytes();
    rt::requestReload();
    rt::tick(w.addr());
    CHECK(hk::retainedBytes() == bytes);
    gOut.clear();
    callLuaHook(17, hp.data());
    rt::tick(w.addr());
    CHECK(gOut.find("hook event\t37\t0\n") != std::string::npos);
    removeTree(gTmp);
    printf("Lua hooks ok\n");
}

void checkHookCallbacks(FakeWorld& w, const std::string& pattern) {
    std::string mods = freshMods();
    rt::requestReload(); rt::tick(w.addr());
    std::string script = "local pat='" + pattern + R"lua('
        local m = {}
        local function add(name, fn, args)
            return Hooks.observe{name=name, pattern=pat, args=args or {'u32','Health*'}, fn=fn}
        end
        m.id = add('once', function(ev)
            ev.args[2].dataStorage[0] = 999
            Hooks.remove(m.id)
            m.late = add('late', function(e)
                assert(e.args[1]==17 and e.args[2]==nil); print('late hook')
            end, {'u32','skip'})
            print('once hook'); print('emit hook')
        end)
        add('bad', function() print('bad hook'); error('intentional hook error') end)
        assert(not pcall(add, 'once', function() end))
        Draw.add(function()
            assert(not pcall(add, 'draw forbidden', function() end)); print('hook draw blocked')
        end)
        registerForEvent('onShutdown', function()
            assert(not pcall(add, 'shutdown forbidden', function() end)); print('hook shutdown blocked')
        end)
        return m
    )lua";
    writeFile(mods + "\\a_hooks", "init.lua", script.c_str());
    script = "local pat='" + pattern + R"lua('
        registerForEvent('onInit', function()
            assert(not pcall(Hooks.remove, GetMod('a_hooks').id)); print('hook ownership blocked')
        end)
        Hooks.observe{name='peer', pattern=pat, args={'u32','Health*'}, fn=function(e)
            assert(e.args[2].dataStorage[0]==37); print('peer hook')
        end}
    )lua";
    writeFile(mods + "\\b_hooks", "init.lua", script.c_str());
    rt::configure(mods, hookTestSink);
    uint64_t problems = hk::problems();
    rt::requestReload(); rt::tick(w.addr());
    CHECK(hk::problems() == problems + 1);
    CHECK(gOut.find("hook ownership blocked\n") != std::string::npos);
    std::vector<uint8_t> hp(refl::lookup("Health").size());
    *reinterpret_cast<int32_t*>(hp.data() + 8) = 37;
    gHookTestHp = hp.data();
    gOut.clear();
    callLuaHook(17, hp.data()); callLuaHook(17, hp.data());
    rt::tick(w.addr());
    CHECK(countOf(gOut, "once hook\n") == 1 && countOf(gOut, "bad hook\n") == 1);
    CHECK(countOf(gOut, "peer hook\n") == 2 && countOf(gOut, "late hook\n") == 0);
    CHECK(gOut.find("a_hooks: hook 'bad' removed after an error") != std::string::npos);
    rt::tick(w.addr());
    CHECK(countOf(gOut, "peer hook\n") == 3 && countOf(gOut, "late hook\n") == 1);
    CHECK(*reinterpret_cast<int32_t*>(hp.data() + 8) == 37);
    gHookTestHp = nullptr;
}

void checkHookDrawAndWorld(FakeWorld& w) {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.IniFilename = nullptr;
    CHECK(drawFrame() == 1);
    ImGui::DestroyContext();
    CHECK(gOut.find("hook draw blocked\n") != std::string::npos);
    callLuaHook(17, nullptr);
    rt::worldLeft(); rt::frame();
    CHECK(gOut.find("assertion failed") == std::string::npos && countOf(gOut, "peer hook\n") == 3);
    rt::requestReload(); rt::frame();
    CHECK(gOut.find("hook shutdown blocked\n") != std::string::npos);
    Sleep(2100);
    *reinterpret_cast<uint32_t*>(w.world + 0x4dc) = 1;
    rt::tick(w.addr());
    removeTree(gTmp);
    printf("hook callbacks and lifecycle ok\n");
}

void checkHooks(uintptr_t base);
int checkHookLimits();
int checkHookReadFault();

// --patterns <exe>: every code lookup the runtime makes at startup, on any build, so a game update can be checked
// before the game starts. Prints what each finds; the exit code is 0 only if all of them did.
int checkPatterns(const char* path) {
    HMODULE exe = LoadLibraryExA(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!exe) return 2;
    auto base = reinterpret_cast<uintptr_t>(exe);
    int bad = 0;
    auto row = [&](const char* what, bool ok, const std::string& detail) {
        printf("%-20s %s  %s\n", what, ok ? "ok  " : "FAIL", detail.c_str());
        bad += !ok;
    };
    auto hex = [](uint64_t v) {
        char b[24];
        snprintf(b, sizeof b, "%#llx", static_cast<unsigned long long>(v));
        return std::string(b);
    };
    auto at = [&](uintptr_t fn) { return fn ? "exe+" + hex(fn - base) : std::string("not found"); };
    printf("build %u (%s)\n", gameBuild(base), testedBuild(gameBuild(base)) ? "tested" : "untested");
    bool found = refl::init(base);  // each lookup runs before its row: argument order is unspecified
    row("reflection", found, std::to_string(refl::count()) + " types");
    uintptr_t admin = findAdminConsumer(base);
    for (auto [what, fn] : {std::pair{"world tick", findWorldTick(base)}, {"admin consumer", admin},
                            {"inventory_actions", systemFn(base, "inventory_actions")},
                            {"options update", findOptionsUpdate(base)}, {"camera push", findCamPush(base)},
                            {"system runner", findRunner(base)}})
        row(what, fn != 0, at(fn));
    uint32_t n = 0;
    int idx = admin ? writtenResource(base, admin, "ecs::GameSettings", n) : -1;
    row("game settings slot", idx >= 0, std::to_string(idx) + " of " + std::to_string(n));
    size_t systems = sys::scan(base);
    row("systems", systems > 0, std::to_string(systems) + " names");
    row("events", ecs::findEvents(base), "");
    found = opts::findLayout(base);
    row("options layout", found,
        "slots at " + hex(opts::slotsAt) + ", " + hex(opts::slotSize) + " apart, has-value at " + hex(opts::hasAt));
    return bad ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--hooks-read-fault") == 0) return checkHookReadFault();
    if (argc == 2 && strcmp(argv[1], "--hooks-limits") == 0) return checkHookLimits();
    if (argc == 2 && strcmp(argv[1], "--av") == 0) return checkAccessViolation();
    if (argc == 3 && strcmp(argv[1], "--patterns") == 0) return checkPatterns(argv[2]);
    if (argc < 2) {
        printf("usage: runtime_selftest <copy of enshrouded.exe> | --patterns <any enshrouded.exe copy>\n");
        return 2;
    }
    HMODULE exe = LoadLibraryExA(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);  // maps the image, runs no code
    CHECK(exe);
    auto base = reinterpret_cast<uintptr_t>(exe);
    checkReflection(base);
    CHECK(ecs::findEvents(base));  // the event map at schema + 0xe151b8 and the streams at world + 0xeb9e80, as FakeWorld
    CHECK(findWorldTick(base) == base + 0xA1E2E0);  // build 1792148057
    CHECK(findAdminConsumer(base) == base + 0x2CAE30);
    uint32_t n = 0;
    CHECK(writtenResource(base, base + 0x2CAE30, "ecs::GameSettings", n) == 4 && n == 6);
    CHECK(writtenResource(base, base + 0x2CAE30, "g38_daytime::Daytime", n) == 5);
    CHECK(writtenResource(base, base + 0x2CAE30, "nope", n) == -1);
    CHECK(writtenResource(base, base + 0x2CAE31, "ecs::GameSettings", n) == -1);
    CHECK(systemFn(base, "admin_player") == base + 0x2CAE30 && systemFn(base, "inventory_actions") == base + 0x3FD610);
    CHECK(!systemFn(base, "no_such_system"));
    printf("code patterns ok\n");
    checkHooks(base);
    checkStartup();
    rt::configure("", sink);
    FakeWorld w;
    checkEcs(w);
    checkLua(w);
    checkTypes(w);
    checkMenu();
    checkTick(w);
    checkBindings(w);
    checkEvents(w);
    checkLuaEvents(w);
    checkKeys();
    checkDraw(w);
    checkAdminFlow(w);
    checkAdminSend(w);
    checkAdminDraw(w);
    checkAdminHelpers(w);
    FakeJob j;
    checkQuerySlot(j);
    checkAdminSettings(w, j);
    checkLifecycle(w);
    checkResources(w);
    checkOptions(base);
    checkSystems(base);
    std::string hookPattern = luaHookPattern();
    checkLuaHooks(w);
    checkHookCallbacks(w, hookPattern);
    checkHookDrawAndWorld(w);
    checkClosing(w);
    char self[MAX_PATH];
    CHECK(GetModuleFileNameA(nullptr, self, MAX_PATH));
    fflush(stdout);  // keeps this output ahead of the child's
    CHECK(_spawnl(_P_WAIT, self, ("\"" + std::string(self) + "\"").c_str(), "--av", nullptr) == 0);
    CHECK(_spawnl(_P_WAIT, self, ("\"" + std::string(self) + "\"").c_str(), "--hooks-limits", nullptr) == 0);
    CHECK(_spawnl(_P_WAIT, self, ("\"" + std::string(self) + "\"").c_str(), "--hooks-read-fault", nullptr) == 0);
    FreeLibrary(exe);
    printf("selftest ok\n");
    return 0;
}
