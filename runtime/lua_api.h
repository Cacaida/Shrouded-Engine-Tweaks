// Lua side of the runtime. Mods load once, at the main menu. Lua runs on the game's server thread inside the world-tick
// hook (main.cpp) while you are in a world you host, on the main thread (frame(), from the options hook) otherwise,
// and Draw callbacks on the render thread (draw(), from the overlay); a mutex keeps them apart.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct lua_State;

namespace rt {

using Sink = void (*)(const char* line);  // print output: console and log; may start with a level marker (level.h)

void configure(const std::string& modsDir, Sink sink);
lua_State* open();                                   // new state with the API, no mods loaded
void setWorld(uintptr_t world);                      // world that Ecs.* resolves against (self-test only)
void exec(lua_State* L, const std::string& line);    // REPL line; expression results are printed
void tick(uintptr_t serverWorld);                    // pre-tick: onWorldEnter, lines, hook handlers, Tick, Keys, reload
void afterTick(uintptr_t serverWorld);               // after the world tick: Observe handlers for this tick's events
void frame();                                        // main thread, each frame: loads mods, runs them outside a world
void worldLeft();                                    // the player left the world: onWorldLeave now, without the game
void gameClosing();                                  // the game is closing: onShutdown without the game, then no mods
void queue(std::string line);                        // console thread or the overlay
void draw();                                         // render thread, inside the ImGui frame: Draw callbacks
bool tryLockDraw();                                  // the Lua lock if free (no tick running), for drawLocked
void drawLocked();                                   // Draw callbacks under tryLockDraw's lock, then releases it
void testHoldLua(bool on);                           // overlay_selftest only: another thread holds the Lua lock
bool hasDraw();                                      // any Draw callback registered
void setOverlayOpen(bool open);                      // Overlay.isOpen(); Keys.on stays silent while open
void requestReload();                                // reload() at the mods' next tick or frame, from the toolbar
bool modsLoaded();                                   // a Lua state with the mods exists
int keyFromName(const char* name);                   // virtual-key code, or 0 for an unknown name
void setAdminAvailable(bool ok);                     // the admin consumer is hooked; Admin.* errors until then
void setSettingsSlot(int index, uint32_t count);     // ecs::GameSettings among the consumer's resources, or -1
void setGiveAvailable(bool ok);                      // inventory_actions is hooked; Admin.give errors until then
void setSystemsAvailable(bool ok);                   // the program runner is hooked; Systems.enable errors until then
void setGameBuild(uint32_t build);                   // the running exe's build, for GetGameBuild() and mod.json
bool systemsAvailable();                             // the Systems window, any thread
// System hooks, before the original: writes the armed commands of that system (0 admin_player, 1 inventory_actions).
void applyAdmin(void* job, int sys = 0);
std::string keyName(int vk);                         // the name keyFromName takes, 0xNN for keys without one

// The overlay's Bindings window, from the render thread. Mods register hotkeys and inputs (Keys.hotkey, Keys.input);
// their key combinations are saved in shrouded_engine_tweaks\bindings.ini.
enum KeyMod { ModCtrl = 1, ModShift = 2, ModAlt = 4 };
struct BindInfo {
    std::string key, mod, label, combo;  // key "<mod>.<id>"; combo like "Ctrl+F6", empty if unbound
    bool input;
};
std::vector<BindInfo> bindings(int& dead);                  // sorted by mod; dead: saved, but no loaded mod has it
void setBinding(const std::string& key, int mods, int vk);  // vk 0 unbinds; saved at once
void removeDeadBindings();
int heldMods();                                             // KeyMod bits held right now

// Internal: the ImGui bindings in lua_imgui.cpp, json in lua_json.cpp.
bool inDraw();
void openImGui(lua_State* L);  // ImGui, ImGuiCond, ImGuiWindowFlags
void imguiMark();              // before a Draw callback
void imguiRecover();           // after one errored: close what it left open
void openJson(lua_State* L);   // json.encode, json.decode

}  // namespace rt
