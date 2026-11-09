// In-game overlay: a Vulkan layer inside powrprof.dll (overlay_vk.cpp) that draws an ImGui console and the other
// built-in windows over the game (overlay_ui.cpp). docs/usage.md has the user side.
#pragma once
#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>

#include "level.h"

namespace ov {

using Sink = void (*)(const char* line);

// shrouded_engine_tweaks\settings.ini. The Settings window edits it too.
struct Settings {
    bool console = false;      // also open the separate console window
    std::string keyName = "`"; // overlayKey as written
    int key = VK_OEM_3;        // its virtual-key code, 0 if the name is unknown
    int fpsCap = 0;            // frames per second at most, 0 = off
    int asyncCompute = 0;      // 0 = the game decides, 1 = on, 2 = off (a launch flag: applies at the next start)
    bool aaOff = false;        // antialiasing kept at None, which the game's menu doesn't offer (opts::setAaOff)
    int historyLimit = 100;    // console lines Up and Down remember, 1 to 10000
    bool saveHistory = true;   // keep them across game starts, in shrouded_engine_tweaks\console_history.txt
    bool debugLog = true;      // extra set.log lines for bug reports (ov::debug)
};

void setup(const std::wstring& game, Sink sink);     // DllMain, game only: layer manifest and env vars for the loader
bool layerLoaded();                                  // the Vulkan loader called the layer
bool manifestOk();                                   // ov::setup wrote the layer's manifest
bool elevated();                                     // above medium integrity: the loader ignores our layer path
std::string loaderVersion();                         // the game's vulkan-1.dll, e.g. "1.4.341"
void setDebugLog(bool on);                           // settings.ini debugLog
void debug(const char* fmt, ...);                    // ov::log to set.log only, only with debugLog on
void addLine(const char* line);                      // console output, from any thread
void setToggleKey(int vk);                           // settings.ini, read before the game builds its swapchain
void setFpsCap(int fps);                             // 0 = off; applies from the next present
void setHistory(int limit, bool save);               // trims the console history; save off deletes its file
// A row of the Settings window's Status (what the runtime found), from any thread, colored by its level. A warn or
// bad row also puts a marker on the toolbar.
void setStatus(const std::string& key, const std::string& value, lv::Level level);
Settings readSettings();                             // written with the defaults if missing (the first start: the
                                                     // overlay asks for its key); defaults outside the game
bool saveSettings(const Settings& s);
extern std::wstring gameDir;                         // with a trailing backslash; set by setup, empty outside the game
std::string utf8(const std::wstring& w);

// Between the two halves.
void log(const char* fmt, ...);                      // to the runtime's sink, if setup ran
extern std::mutex uiMx;                              // held around every ImGui call
void attach(HWND wnd, const std::string& iniPath);   // first window only: ImGui context, Win32 backend, subclass
void resize(HWND wnd, uint32_t height);              // every swapchain: rescales the UI if the attached window's height changed
bool visible();                                      // open, or mods draw windows (Draw.add)
void disable();                                      // closes the overlay for good (after a crash in drawing)
void buildFrame();                                   // ImGui frame: own windows if open, then mods'; caller holds uiMx

}  // namespace ov
