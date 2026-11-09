// Shrouded Engine Tweaks. enshrouded.exe loads it as POWRPROF.dll from the game folder (docs/architecture.md).
// Hooks the ECS world tick and the game's per-frame options update: Lua mods run in the server world's tick in a world
// you host, and in the main thread's frame elsewhere. Also hooks the admin consumer, inventory_actions and the system
// runner, for Admin.* and Systems. Inert in any other exe.
#include <windows.h>
#include <commctrl.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "MinHook.h"
#include "contracts.h"
#include "exports.h"
#include "game.h"
#include "hooks.h"
#include "inspect.h"
#include "level.h"
#include "lua_api.h"
#include "overlay.h"
#include "report.h"
#include "version.h"

namespace {

using TickFn = void (*)(uintptr_t world, char flag);
TickFn gOrigTick;
using JobFn = void (*)(void* job);
JobFn gOrigAdmin, gOrigInventory;
using AppFn = void (*)(uintptr_t app);
AppFn gOrigOptions;
using RunFn = void (*)(uintptr_t world, void* job, uint64_t index, uint64_t slot);
RunFn gOrigRun;
volatile bool gOptsOff;  // set after an access violation in the game options
wchar_t gDir[MAX_PATH];  // game folder, with a trailing backslash
HANDLE gLog = INVALID_HANDLE_VALUE, gCon = INVALID_HANDLE_VALUE;
CRITICAL_SECTION gOutCs;
ov::Settings gSet;  // settings.ini, read in DllMain
bool gFlagged;      // the async compute launch flag went in
// The resources and English names are in, or never will be: the mods may load.
std::atomic<bool> gResDone;

// "[hh:mm:ss.mmm] " ahead of a set.log line. The caller holds gOutCs.
void stamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char s[24];
    int n = snprintf(s, sizeof s, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    DWORD w;
    WriteFile(gLog, s, static_cast<DWORD>(n), &w, nullptr);
}

// Console window text colors, by lv::Level: none is the console's default grey.
constexpr WORD kConColor[] = {FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE, FOREGROUND_GREEN | FOREGROUND_INTENSITY,
                              FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY, FOREGROUND_RED | FOREGROUND_INTENSITY};

// One line to the console window, set.log (with the time) and the overlay, colored or tagged by its level marker
// (level.h); an LV_LOG line goes to set.log only. Called from the server, main, init, console and render threads.
void sink(const char* line) {
    bool logOnly = *line == *LV_LOG;
    if (logOnly) line++;
    else ov::addLine(line);
    lv::Level l = lv::of(line);
    const char* text = line + (l != lv::none);
    EnterCriticalSection(&gOutCs);
    DWORD w;
    auto n = static_cast<DWORD>(strlen(text));
    for (HANDLE h : {gCon, gLog}) {
        if (h == INVALID_HANDLE_VALUE || (logOnly && h == gCon)) continue;
        if (h == gLog) {
            stamp();
            if (l >= lv::warn) WriteFile(h, l == lv::warn ? "[W] " : "[E] ", 4, &w, nullptr);
        } else if (l) {
            SetConsoleTextAttribute(h, kConColor[l]);
        }
        WriteFile(h, text, n, &w, nullptr);
        if (h == gCon && l) SetConsoleTextAttribute(h, kConColor[lv::none]);
        WriteFile(h, "\r\n", 2, &w, nullptr);
    }
    LeaveCriticalSection(&gOutCs);
}

void out(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sink(buf);
}

// Only act inside enshrouded.exe. Also sets gDir to the exe's folder.
bool isGame() {
    GetModuleFileNameW(nullptr, gDir, MAX_PATH);
    wchar_t* name = wcsrchr(gDir, L'\\');
    if (!name || _wcsicmp(name + 1, L"enshrouded.exe") != 0) return false;
    name[1] = 0;
    return true;
}

// Limit: ANSI paths for the mods' file access; a game folder with non-ANSI characters would need UTF-8 file loading.
std::string narrow(const std::wstring& w) {
    char buf[MAX_PATH * 2];
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, buf, sizeof buf, nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string();
}

// Creates shrouded_engine_tweaks\ and mods\, opens set.log, and points the Lua side at the mods folder. The last
// session's log stays as set.prev.log, so a crash's log survives the restart.
void openFiles() {
    std::wstring root = std::wstring(gDir) + L"shrouded_engine_tweaks", log = root + L"\\set.log";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW((root + L"\\mods").c_str(), nullptr);
    MoveFileExW(log.c_str(), (root + L"\\set.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    rep::checkLast(gDir);  // did that session close normally? Logged by logLastSession, shown by the overlay
    gLog = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                       nullptr);
    rt::configure(narrow(root + L"\\mods"), sink);
}

BOOL WINAPI swallowCtrl(DWORD) { return TRUE; }  // Ctrl+C in the console must not end the game

void openConsole() {
    AllocConsole();
    SetConsoleTitleW(L"Shrouded Engine Tweaks");
    SetConsoleOutputCP(CP_UTF8);
    if (HWND w = GetConsoleWindow()) DeleteMenu(GetSystemMenu(w, FALSE), SC_CLOSE, MF_BYCOMMAND);  // closing it would kill the game
    SetConsoleCtrlHandler(swallowCtrl, TRUE);
    gCon = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_EXISTING, 0, nullptr);
    // Keep the window ours: code that prints to the standard handles (e.g. Steam's Fossilize layer) goes nowhere,
    // as it would without a console.
    SetStdHandle(STD_OUTPUT_HANDLE, nullptr);
    SetStdHandle(STD_ERROR_HANDLE, nullptr);
}

// Typed lines go to the log and the overlay; the console window already echoes them.
void logInput(const char* line) {
    ov::addLine(("> " + std::string(line)).c_str());
    EnterCriticalSection(&gOutCs);
    DWORD w;
    if (gLog != INVALID_HANDLE_VALUE) {
        stamp();
        WriteFile(gLog, "> ", 2, &w, nullptr);
        WriteFile(gLog, line, static_cast<DWORD>(strlen(line)), &w, nullptr);
        WriteFile(gLog, "\r\n", 2, &w, nullptr);
    }
    LeaveCriticalSection(&gOutCs);
}

// Reads console lines forever and queues them for the mods' next tick or frame.
void readConsole() {
    HANDLE in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    DWORD mode = 0;
    GetConsoleMode(in, &mode);
    // QuickEdit pauses console output while text is selected, which would stall the game's threads in sink().
    SetConsoleMode(in, (mode & ~DWORD(ENABLE_QUICK_EDIT_MODE)) | ENABLE_EXTENDED_FLAGS);
    char buf[2048];
    DWORD n = 0;
    while (ReadConsoleA(in, buf, sizeof buf - 1, &n, nullptr)) {
        buf[n] = 0;
        buf[strcspn(buf, "\r\n")] = 0;
        // Drop control characters the console inserts for keys like F6 (^Z) or Ctrl+S (^S).
        char* end = std::remove_if(buf, buf + strlen(buf), [](char c) { return c > 0 && c < 0x20 && c != '\t'; });
        *end = 0;
        if (!*buf) continue;
        logInput(buf);
        rt::queue(buf);
    }
}

constexpr const char* kGone = "Not found (game updated?)";  // a feature's reason when its lookup failed

// An access violation turned a feature off: still compatible, off until the game restarts. A function of its own: the
// handlers' __try can't share one with C++ temporaries.
void lostFeature(const char* id) { rt::setFeature(id, true, "Access violation: Off until the game restarts", false); }

// When the count of hook refusals and faults changed: logs the new ones and marks the Function Hooks Status row.
void hookStatus() {
    static std::atomic<uint64_t> seen{0};
    uint64_t n = hk::problems();
    if (seen.exchange(n) == n) return;
    hk::flushLogs();
    ov::setStatus("Function Hooks", std::to_string(n) + " refusal(s) or fault(s): See Hooks", lv::bad);
}

// When a system ran while Lua had its world (the world tick's design rules it out): logged once, counted in Status.
void readPointStatus() {
    static std::atomic<uint64_t> seen{0};
    uint64_t n = rt::overlaps();
    uint64_t was = seen.exchange(n);
    if (was == n) return;
    if (!was) out(LV_BAD "Read Point: A game system ran while Lua read the world (please report this)");
    ov::setStatus("Read Point", std::to_string(n) + " system call(s) overlapped Lua", lv::bad);
}

// The Entities window's reads and edits, right after the Lua tick, still before the world's systems run. An access
// violation turns the inspector off, never the game or the mods.
void inspect() {
    uint64_t session = 0;
    uintptr_t world = rt::readyWorld(session);
    __try {
        insp::tick(world, session);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        insp::turnOff();
        sink(LV_BAD "Entities: Access violation, the inspector is off until the game restarts");
    }
}

// Server world only (kind 0): pre-tick work, the game's tick, then Observe handlers while this tick's events are
// still in their streams (the session clears them after the tick returns). An access violation in Lua work turns the
// mods off (rt::tick catches it inside its lock).
void onTick(uintptr_t world, char flag) {
    bool server = *reinterpret_cast<const uint8_t*>(world + 8) == 0;
    if (server) { hookStatus(); readPointStatus(); rt::tick(world); inspect(); }
    gOrigTick(world, flag);
    if (server) rt::afterTick(world);
}

// The game's admin-command consumer system, inside the world tick: Lua's queued admin commands go
// into PlayerInput right before the game reads it. An access violation turns admin commands off, never the game.
void onAdminConsumer(void* job) {
    __try {
        rt::applyAdmin(job);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        rt::setAdminAvailable(false);
        lostFeature("admin");
        sink(LV_BAD "Admin Commands: Access violation, Admin.* off until the game restarts");
    }
    gOrigAdmin(job);
}

// Optional: without it Admin.* says it is unavailable, and everything else still runs. Needs MH_Initialize.
void hookAdmin(uintptr_t base) {
    uintptr_t fn = findAdminConsumer(base);
    hk::reserve(fn, "admin consumer");
    uint32_t count = 0;
    int idx = fn ? writtenResource(base, fn, "ecs::GameSettings", count) : -1;
    rt::setSettingsSlot(idx, count);
    if (fn && idx < 0) {
        out(LV_BAD "Admin.settings: Game settings not found in the admin system (game updated?), unavailable");
        ov::setStatus("Game Settings", "Not found: Admin.settings off", lv::bad);
    }
    rt::setFeature("admin_settings", fn && idx >= 0, fn && idx >= 0 ? "" : kGone);
    auto target = reinterpret_cast<void*>(fn);
    if (!fn || MH_CreateHook(target, reinterpret_cast<void*>(&onAdminConsumer), reinterpret_cast<void**>(&gOrigAdmin)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        out(LV_BAD "Admin Commands: Consumer not found exactly once (game updated?), Admin.* unavailable");
        ov::setStatus("Admin Commands", "Not found: Admin.* off", lv::bad);
        rt::setFeature("admin", false, kGone);
        rt::setFeature("admin_settings", false, kGone);
        return;
    }
    rt::setAdminAvailable(true);
    out(LV_OK "Admin Commands: Hooked at exe+%llx", static_cast<unsigned long long>(fn - base));
    ov::setStatus("Admin Commands", "Hooked", lv::ok);
    rt::setFeature("admin", true);
}

// The game's inventory_actions system, which runs the admin create-item command: Admin.give's commands go into
// PlayerInput right before it, as with the admin consumer.
void onInventoryActions(void* job) {
    __try {
        rt::applyAdmin(job, 1);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        rt::setGiveAvailable(false);
        lostFeature("admin_give");
        sink(LV_BAD "Admin.give: Access violation, off until the game restarts");
    }
    gOrigInventory(job);
}

// Optional, like hookAdmin. Found by the system's name, not a byte pattern.
void hookGive(uintptr_t base) {
    uintptr_t fn = systemFn(base, "inventory_actions");
    hk::reserve(fn, "inventory actions");
    auto target = reinterpret_cast<void*>(fn);
    if (!fn || MH_CreateHook(target, reinterpret_cast<void*>(&onInventoryActions), reinterpret_cast<void**>(&gOrigInventory)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        out(LV_BAD "Admin.give: Inventory commands not found (game updated?), unavailable");
        ov::setStatus("Admin.give", "Not found: Admin.give off", lv::bad);
        rt::setFeature("admin_give", false, kGone);
        return;
    }
    rt::setGiveAvailable(true);
    out(LV_OK "Admin.give: Hooked at exe+%llx", static_cast<unsigned long long>(fn - base));
    ov::setStatus("Admin.give", "Hooked", lv::ok);
    rt::setFeature("admin_give", true);
}

// Logs when the options check out, or once why they don't yet.
void optionsFrame(uintptr_t app) {
    static bool found, told;
    const char* why = opts::frame(app);
    if (!why && !std::exchange(found, true)) {
        out(LV_OK "Game Options: Found (machine settings v%u)", *reinterpret_cast<const uint32_t*>(opts::machine()));
        ov::setStatus("Game Options", "Found", lv::ok);
        rt::setFeature("game_options", true);
    } else if (why && !found && !std::exchange(told, true)) {
        out(LV_WARN "Game Options: Not recognized yet (%s), still trying", why);
        ov::setStatus("Game Options", std::string("Not recognized yet: ") + why, lv::warn);
        rt::setFeature("game_options", true, std::string("Not recognized yet: ") + why, false);
    }
}

// The game's per-frame options update, on the main thread: finds the options and keeps the antialiasing switch. An
// access violation turns GameOptions off, never the game. Then the mods' frame: they load here at the main menu and
// run here outside a world.
void onOptionsUpdate(uintptr_t app) {
    hookStatus();
    if (!gOptsOff) {
        __try {
            optionsFrame(app);
        } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
            gOptsOff = true;
            lostFeature("game_options");
            sink(LV_BAD "Game Options: Access violation, GameOptions off until the game restarts");
        }
    }
    if (gResDone) rt::frame();
    gOrigOptions(app);
}

// Optional, like hookAdmin: without it GameOptions says the options weren't found, and everything else still runs.
void hookOptions(uintptr_t base) {
    uintptr_t push = findCamPush(base);
    if (push) {
        opts::setCamPush(push);
        out(LV_OK "Field of View: Camera push at exe+%llx", static_cast<unsigned long long>(push - base));
    } else {
        out(LV_BAD "Field of View: Camera push not found (game updated?), changes need the game's settings menu");
    }
    // The push only runs from the options hook, so the field of view applies live only once both are in.
    ov::setStatus("Field of View", "Not found: Set it in the game's menu", lv::bad);
    rt::setFeature("field_of_view", false, kGone);
    rt::setFeature("game_options", true, "Not found yet", false);  // until optionsFrame finds them
    if (!opts::findLayout(base))
        out(LV_BAD "Game Options: User settings layout not found (game updated?), GameOptions unavailable");
    uintptr_t fn = findOptionsUpdate(base);
    hk::reserve(fn, "options update");
    auto target = reinterpret_cast<void*>(fn);
    if (!fn || MH_CreateHook(target, reinterpret_cast<void*>(&onOptionsUpdate), reinterpret_cast<void**>(&gOrigOptions)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        out(LV_BAD "Game Options: Not found (game updated?), GameOptions unavailable");
        ov::setStatus("Game Options", "Not found: GameOptions off", lv::bad);
        rt::setFeature("game_options", false, kGone);
        return;
    }
    out(LV_OK "Game Options: Hooked at exe+%llx", static_cast<unsigned long long>(fn - base));
    if (!push) return;
    ov::setStatus("Field of View", "Applies live", lv::ok);
    rt::setFeature("field_of_view", true);
}

// The game's program runner, which runs every ECS system, mostly on its worker threads: a system switched off is
// skipped whole, so the game's check that the system updated all its entities never runs either. While the profiler
// is on, each call is timed on its own thread. Every call also checks the read point (rt::jobStart/jobEnd).
void onRun(uintptr_t world, void* job, uint64_t index, uint64_t slot) {
    if (sys::skip(job)) return;
    uint32_t gen = rt::jobStart();
    if (!sys::profiling()) {
        gOrigRun(world, job, index, slot);
    } else {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        gOrigRun(world, job, index, slot);
        QueryPerformanceCounter(&b);
        sys::record(job, *reinterpret_cast<const uint8_t*>(world + 8), b.QuadPart - a.QuadPart);
    }
    rt::jobEnd(world, gen);
}

// Optional, like hookAdmin: without it Systems.enable says it is unavailable, and everything else still runs.
void hookSystems(uintptr_t base) {
    size_t n = sys::scan(base);
    uintptr_t fn = findRunner(base);
    hk::reserve(fn, "system runner");
    auto target = reinterpret_cast<void*>(fn);
    if (!n || !fn || MH_CreateHook(target, reinterpret_cast<void*>(&onRun), reinterpret_cast<void**>(&gOrigRun)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        out(LV_BAD "Systems: Not found (game updated?), %zu names, runner %s, Systems.enable unavailable", n, fn ? "found" : "not found");
        ov::setStatus("Systems", "Not found: Systems off", lv::bad);
        rt::setFeature("systems", false, kGone);
        return;
    }
    rt::setSystemsAvailable(true);
    out(LV_OK "Systems: %zu found, runner hooked at exe+%llx", n, static_cast<unsigned long long>(fn - base));
    ov::setStatus("Systems", std::to_string(n) + " found", lv::ok);
    ov::setStatus("Read Point", "Checked at every system call: No overlap", lv::ok);
    rt::setFeature("systems", true);
}

BOOL CALLBACK isGameWindow(HWND w, LPARAM found) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || w == GetConsoleWindow() || !IsWindowVisible(w)) return TRUE;
    *reinterpret_cast<bool*>(found) = true;
    return FALSE;
}

// Waits for the game window (up to 2 min) so startup, including the SteamStub entry stub, is over before patching code.
void waitForGameWindow() {
    for (int i = 0; i < 480; i++) {
        bool found = false;
        EnumWindows(isGameWindow, reinterpret_cast<LPARAM>(&found));
        if (found) return;
        Sleep(250);
    }
}

// Optional, like hookAdmin: without it Observe never fires and Events.post says the world has no such stream.
void findEvents(uintptr_t base) {
    if (ecs::findEvents(base)) {
        out(LV_OK "Events: Found");
        ov::setStatus("Events", "Found", lv::ok);
        rt::setFeature("events", true);
        return;
    }
    out(LV_BAD "Events: Event streams not found (game updated?), Observe and Events.post unavailable");
    ov::setStatus("Events", "Not found: Observe and Events.post off", lv::bad);
    rt::setFeature("events", false, kGone);
}

// The interception catalog, checked against this build: an entry that fails is off, the others and everything else
// keep working.
void checkCatalog(uintptr_t base) {
    auto all = contracts::check(base, gameBuild(base), contracts::catalog());
    size_t ok = 0;
    for (const auto& s : all) {
        if (s.fn) out(LV_OK "Catalog: %s at exe+%llx", s.name.c_str(), static_cast<unsigned long long>(s.fn - base));
        else out(LV_WARN "Catalog: %s off (%s)", s.name.c_str(), s.why.c_str());
        ok += s.fn != 0;
    }
    out(LV_OK "Catalog: %zu of %zu targets checked out", ok, all.size());
    ov::setStatus("Catalog", std::to_string(ok) + " of " + std::to_string(all.size()) + " targets",
                  ok == all.size() ? lv::ok : lv::warn);
    contracts::setChecked(std::move(all));
}

bool installHook(uintptr_t base) {
    uintptr_t fn = findWorldTick(base);
    hk::reserve(fn, "world tick");
    if (!fn) {
        out(LV_BAD "World Tick: Pattern not found exactly once (game updated?), runtime off");
        ov::setStatus("World Tick", "Not found: Mods off", lv::bad);
        return false;
    }
    if (MH_Initialize() != MH_OK) {
        out(LV_BAD "World Tick: Hooking failed, runtime off");
        ov::setStatus("World Tick", "Hook failed: Mods off", lv::bad);
        return false;
    }
    hookAdmin(base);
    hookGive(base);
    hookOptions(base);
    hookSystems(base);
    findEvents(base);
    checkCatalog(base);
    auto target = reinterpret_cast<void*>(fn);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&onTick), reinterpret_cast<void**>(&gOrigTick)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        out(LV_BAD "World Tick: Hooking failed, runtime off");
        ov::setStatus("World Tick", "Hook failed: Mods off", lv::bad);
        return false;
    }
    out(LV_OK "World Tick: Hooked at exe+%llx", static_cast<unsigned long long>(fn - base));
    ov::setStatus("World Tick", "Hooked", lv::ok);
    ov::setStatus("Function Hooks", "Ready (Observe only)", lv::ok);
    return true;
}

// The resource index, enshrouded.kfc. Read early, before the game opens it. A mod loader that patches the file does so
// in its DllMain, which finished before this thread could start.
void loadResIndex() {
    std::wstring path = std::wstring(gDir) + L"enshrouded.kfc";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    LARGE_INTEGER size{};
    std::vector<uint8_t> buf;
    DWORD got = 0;
    bool ok = f != INVALID_HANDLE_VALUE && GetFileSizeEx(f, &size) && size.QuadPart < (64 << 20);
    if (ok) {
        buf.resize(static_cast<size_t>(size.QuadPart));
        ok = ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got == buf.size();
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (ok && res::load(buf.data(), buf.size())) return;
    out(LV_BAD "Resources: enshrouded.kfc not read or not recognized (game updated?), Res unavailable");
    ov::setStatus("Resources", "enshrouded.kfc not recognized: Res off", lv::bad);
    rt::setFeature("resources", false, "enshrouded.kfc not recognized (game updated?)");
    rt::setFeature("english_names", false, "Needs resources");
}

// The English names: the game's text table, read once from the .dat file the index points to (about 1.7 MB). The game
// streams its resource data in for about 1.5 s, and the first resources can check out before the text collection is
// in, so it looks again for up to 30 s.
void loadText() {
    uint32_t c = 0, off = 0, size = 0;
    std::vector<uint8_t> buf;
    bool ok = false;
    int waits = 0;
    for (; waits < 120 && !(ok = res::textSource(c, off, size)); waits++) Sleep(250);
    ok = ok && size < (64u << 20);
    if (ok) {
        wchar_t name[32];
        swprintf(name, 32, L"enshrouded_%03u.dat", c);
        HANDLE f = CreateFileW((std::wstring(gDir) + name).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        LARGE_INTEGER at{};
        at.QuadPart = off;
        DWORD got = 0;
        buf.resize(size);
        ok = f != INVALID_HANDLE_VALUE && SetFilePointerEx(f, at, nullptr, FILE_BEGIN) &&
             ReadFile(f, buf.data(), size, &got, nullptr) && got == size;
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    if (ok && res::setText(std::move(buf))) {
        out(LV_OK "English Names: %zu texts (waited %.2f s)", res::textCount(), waits / 4.0);
        ov::setStatus("English Names", std::to_string(res::textCount()) + " texts", lv::ok);
        rt::setFeature("english_names", true);
    } else {
        out(LV_BAD "English Names: Not found (game updated?), Res.text unavailable");
        ov::setStatus("English Names", "Not found: Res.text off", lv::bad);
        rt::setFeature("english_names", false, kGone);
    }
}

// The game loads its resources about half a second after its window shows. Waits up to 2 min for them.
void attachResources() {
    if (!res::count()) return;
    for (int i = 0; i < 480 && !res::attach(kResourceBase); i++) Sleep(250);
    auto base = static_cast<unsigned long long>(kResourceBase);
    if (!res::ready()) {
        out(LV_BAD "Resources: Data not found at 0x%llx (game updated?), Res unavailable", base);
        ov::setStatus("Resources", "Data not found: Res off", lv::bad);
        rt::setFeature("resources", false, kGone);
        rt::setFeature("english_names", false, "Needs resources");
        return;
    }
    out(LV_OK "Resources: %zu in %zu types, data at 0x%llx (protect 0x%x)", res::count(), res::types().size(), base,
        res::protection());
    ov::setStatus("Resources", std::to_string(res::count()) + " in " + std::to_string(res::types().size()) + " types",
                  lv::ok);
    rt::setFeature("resources", true);
    loadText();
}

// Reads what the game appends to enshrouded.log, opened shared like the Game Log window, line by line:
// - Leaving a world: the game logs this line once per world session, as its server stops. If a patch changes it,
//   onWorldLeave runs when the next world starts.
// - Closing: the game's shutdown sequence starts with this line, only at the end, at least 2.36 s before it frees its
//   options and resources. If a patch changes it, onShutdown doesn't run on exit.
DWORD WINAPI watchLog(void*) {
    static constexpr std::string_view kLeave = "[game_server] Switching state from Run to Shutdown",
                                      kClose = "[GameFramework] Shutdown '";
    HANDLE f = CreateFileW((std::wstring(gDir) + L"enshrouded.log").c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        out(LV_WARN "Game Log: enshrouded.log not readable, onWorldLeave runs when the next world starts and onShutdown not on exit");
        return 0;
    }
    static char buf[64 * 1024];
    std::string part;  // a line that hasn't ended yet
    for (;;) {
        DWORD got = 0;
        while (ReadFile(f, buf, sizeof buf, &got, nullptr) && got > 0) {
            part.append(buf, got);
            size_t start = 0;
            for (size_t nl; (nl = part.find('\n', start)) != std::string::npos; start = nl + 1) {
                std::string_view line(part.data() + start, nl - start);
                if (line.find(kLeave) != std::string_view::npos) rt::worldLeft();
                if (line.find(kClose) == std::string_view::npos) continue;
                rt::gameClosing();
                CloseHandle(f);
                return 0;
            }
            part.erase(0, start);
        }
        Sleep(250);
    }
}

// Other DLL mods that also install as powrprof.dll (or any name): every .dll in shrouded_engine_tweaks\plugins\, by
// name, loaded from DllMain where their own DllMain would have run as the game's import. set.log isn't open yet, so
// each result is kept for init() to log.
std::vector<std::string> gPlugins;

void loadPlugins() {
    std::wstring dir = std::wstring(gDir) + L"shrouded_engine_tweaks\\plugins\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end());
    for (const std::wstring& n : names) {
        HMODULE m = LoadLibraryExW((dir + n).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        bool ok = m != nullptr;
        DWORD err = GetLastError();
        char line[MAX_PATH * 2 + 96];
        std::string name = narrow(n);
        // A copy of SET itself: its DllMain found this one running (firstCopy) and did nothing.
        if (ok && GetProcAddress(m, "rtOverlayFrames"))
            snprintf(line, sizeof line, LV_BAD "Plugins: %s is Shrouded Engine Tweaks itself: Remove it from plugins\\, "
                     "only the game folder's copy runs", name.c_str());
        else if (ok) snprintf(line, sizeof line, LV_OK "Plugins: Loaded %s", name.c_str());
        else snprintf(line, sizeof line, LV_BAD "Plugins: %s not loaded (Windows error %lu)", name.c_str(), err);
        gPlugins.push_back(line);
    }
}

void logPlugins() {
    for (const std::string& l : gPlugins) sink(l.c_str());
    if (gPlugins.empty()) return;
    size_t bad = std::count_if(gPlugins.begin(), gPlugins.end(),
                               [](const std::string& l) { return lv::of(l.c_str()) == lv::bad; });
    std::string v = std::to_string(gPlugins.size() - bad) + " loaded";
    if (bad) v += ", " + std::to_string(bad) + " failed: See logs";
    ov::setStatus("Plugins", v, bad ? lv::warn : lv::ok);
}

const char* asyncFlag() { return gSet.asyncCompute == 1 ? "--enable-async-compute" : "--disable-async-compute"; }

// The first lines: which version, on which game build, and whether that build was tested.
void logStart(uintptr_t base) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    out(LV_OK "Shrouded Engine Tweaks %s (%04u-%02u-%02u)", RUNTIME_VERSION, t.wYear, t.wMonth, t.wDay);
    ov::setStatus("Runtime", RUNTIME_VERSION, lv::ok);
    uint32_t build = gameBuild(base);
    rt::setGameBuild(build);  // before any mod loads
    bool tested = testedBuild(build);
    out(tested ? LV_OK "Game Build: %u" : LV_WARN "Game Build: %u", build);  // warn: untested, each feature checks itself
    ov::setStatus("Game Build", std::to_string(build), tested ? lv::ok : lv::warn);
}

// Every hardware GPU and its driver, through DXGI: no device is made, and it works without the overlay.
void logGpus() {
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) return;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        LARGE_INTEGER v{};
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            std::string name = ov::utf8(d.Description);
            if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v)))
                out("GPU: %s, driver %u.%u.%u.%u", name.c_str(), HIWORD(v.HighPart), LOWORD(v.HighPart),
                    HIWORD(v.LowPart), LOWORD(v.LowPart));
            else out("GPU: %s, driver version unknown", name.c_str());
        }
        a->Release();
    }
    f->Release();
}

// Wine's wine_get_version export, null on Windows.
FARPROC onWine() { return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version"); }

// The facts a bug report needs: Windows build, Wine, administrator, GPUs.
void logSystem() {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    OSVERSIONINFOW v{sizeof v};
    if (auto get = reinterpret_cast<LONG(WINAPI*)(OSVERSIONINFOW*)>(GetProcAddress(nt, "RtlGetVersion"))) get(&v);
    auto wine = reinterpret_cast<const char* (*)()>(onWine());
    out("System: Windows %lu.%lu.%lu%s%s, Administrator: %s", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber,
        wine ? ", Linux (Wine " : "", wine ? (std::string(wine()) + ")").c_str() : "", ov::elevated() ? "Yes" : "No");
    logGpus();
}

void logLastSession() {
    const rep::LastSession& last = rep::lastSession();
    if (!last.crashed) return;
    out(LV_WARN "Last Session: Closed unexpectedly. Its log is set.prev.log, and the overlay offers to copy it");
    if (!last.report.empty()) out(LV_WARN "%s", last.report.c_str());
}

std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

constexpr wchar_t kTitle[] = L"Shrouded Engine Tweaks";
constexpr wchar_t kGitHub[] = L"https://github.com/Cacaida/Shrouded-Engine-Tweaks/issues/new/choose";
constexpr wchar_t kNexus[] = L"https://www.nexusmods.com/enshrouded/mods/303?tab=bugs";
constexpr UINT kTop = MB_TOPMOST | MB_SETFOREGROUND;

// The task dialog comes up on top of the game like the message boxes, and its links open in the browser.
HRESULT CALLBACK onDialog(HWND w, UINT msg, WPARAM, LPARAM l, LONG_PTR) {
    if (msg == TDN_CREATED) {
        SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(w);
    }
    if (msg == TDN_HYPERLINK_CLICKED)
        ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(l), nullptr, nullptr, SW_SHOWNORMAL);
    return S_OK;
}

// Text with clickable <a href> links, in a task dialog. That needs Common Controls 6, which the game may not have
// active: the manifest in resource 128 (version.rc.in) turns it on for this call only. False if any step fails.
bool linkDialog(const std::wstring& text) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&linkDialog), &self);
    ACTCTXW ac{sizeof ac};
    ac.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    ac.hModule = self;
    ac.lpResourceName = MAKEINTRESOURCEW(128);
    HANDLE ctx = CreateActCtxW(&ac);
    if (ctx == INVALID_HANDLE_VALUE) return false;
    ULONG_PTR cookie = 0;
    bool ok = false;
    if (ActivateActCtx(ctx, &cookie)) {
        using TaskDlg = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
        HMODULE cc = LoadLibraryW(L"comctl32.dll");  // version 6, under this context
        auto show = cc ? reinterpret_cast<TaskDlg>(GetProcAddress(cc, "TaskDialogIndirect")) : nullptr;
        TASKDIALOGCONFIG c{sizeof c};
        c.dwFlags = TDF_ENABLE_HYPERLINKS;
        c.pszWindowTitle = kTitle;
        c.pszMainIcon = TD_INFORMATION_ICON;
        c.pszContent = text.c_str();
        c.dwCommonButtons = TDCBF_OK_BUTTON;
        c.pfCallback = onDialog;
        ok = show && SUCCEEDED(show(&c, nullptr, nullptr, nullptr));
        DeactivateActCtx(0, cookie);
    }
    ReleaseActCtx(ctx);
    return ok;
}

// After Yes: where to report, as links. Without the task dialog, the same as plain text in a message box.
void copiedDialog(bool both) {
    std::wstring done = both ? L"Logs copied to the clipboard. Attach them to any bug report(s).\n\n"
                             : L"Log copied to the clipboard. Attach it to any bug report(s).\n\n";
    std::wstring gh = kGitHub, nx = kNexus;
    if (!linkDialog(done + L"GitHub: <a href=\"" + gh + L"\">" + gh + L"</a>\nNexus: <a href=\"" + nx + L"\">" + nx + L"</a>"))
        MessageBoxW(nullptr, (done + L"GitHub: " + gh + L"\nNexus: " + nx).c_str(), kTitle, MB_OK | MB_ICONINFORMATION | kTop);
}

struct OverlayMsg {
    std::wstring text;
    bool ask;  // Yes/No to copy the logs, else just OK
};

DWORD WINAPI overlayDialog(void* p) {
    std::unique_ptr<OverlayMsg> m(static_cast<OverlayMsg*>(p));
    if (!m->ask) {
        MessageBoxW(nullptr, m->text.c_str(), kTitle, MB_OK | MB_ICONWARNING | kTop);
        return 0;
    }
    if (MessageBoxW(nullptr, m->text.c_str(), kTitle, MB_YESNO | MB_ICONWARNING | kTop) != IDYES) return 0;
    bool both = rep::lastSession().crashed;
    std::string log = rep::logText(gDir, false);
    if (both)  // the crashed session's log first, then this one's, which says why no overlay
        log = rep::logText(gDir, true) + "\r\n\r\n===== This session (set.log) =====\r\n\r\n" + log;
    if (!rep::copyText(log)) {
        MessageBoxW(nullptr, L"Copying failed. The logs are set.log and set.prev.log in the game folder's "
                             L"shrouded_engine_tweaks.", kTitle, MB_OK | MB_ICONWARNING | kTop);
        return 0;
    }
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);  // for the links
    copiedDialog(both);
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

// Why the layer didn't load. Admin and Linux are known limits, not bugs; anything else is.
enum class NoOverlay { wine, admin, bug };

// The overlay can't load, so nothing can show in the game: a Windows message box, on its own thread so init goes on.
// It also stands in for the overlay's Last Session window when the last session didn't close normally.
void showOverlayDialog(NoOverlay why) {
    const rep::LastSession& last = rep::lastSession();
    bool bug = why == NoOverlay::bug;
    std::wstring t;
    if (why == NoOverlay::wine)
        t = L"The in-game overlay couldn't load this session:\n\nThe game is running on Linux (Proton/Wine), where the "
            L"overlay isn't supported yet. This isn't a bug.\n\n";
    else if (why == NoOverlay::admin)
        t = L"The in-game overlay couldn't load this session:\n\nSteam or the game is running as administrator. Windows "
            L"blocks the overlay then, this isn't a bug.\n\nTo fix it: close the game and Steam, then start Steam "
            L"normally. If that doesn't help, untick \"Run this program as administrator\" under Properties > "
            L"Compatibility for steam.exe and enshrouded.exe.\n\n";
    else
        t = L"The in-game overlay couldn't load this session. SET checked: the game isn't running as administrator, so "
            L"this is likely a bug.\n\n" +
            std::wstring(ov::manifestOk() ? L"" : L"Writing shrouded_engine_tweaks\\layer\\shrouded_engine_tweaks.json failed.\n") +
            L"Vulkan Loader " + wide(ov::loaderVersion()) + L"\n\n";
    t += L"Mods and Console will still work.\n\n";
    if (last.crashed)
        t += L"The last session didn't close normally (a crash, unexpectedly, or from the outside). " +
             wide(last.report.empty() ? "No game crash report was written." : last.report) + L"\n\n";
    if (bug)
        t += last.crashed ? L"Please report it: \"Yes\" copies the logs (set.prev.log and set.log) to your clipboard, "
                            L"attach them to the bug report."
                          : L"Please report it: \"Yes\" copies the log (set.log) to your clipboard, attach it to the bug report.";
    else if (last.crashed)
        t += L"\"Yes\" copies the logs (set.prev.log and set.log) to your clipboard, use them when reporting it.";
    while (!t.empty() && t.back() == L'\n') t.pop_back();
    auto* m = new OverlayMsg{t, bug || last.crashed};
    if (HANDLE h = CreateThread(nullptr, 0, overlayDialog, m, 0, nullptr)) CloseHandle(h);
    else delete m;
}

DWORD WINAPI init(void*) {
    const ov::Settings& s = gSet;
    if (s.console) openConsole();
    openFiles();
    hk::setLog(sink);
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    logStart(base);
    logSystem();
    logLastSession();
    logPlugins();
    if (!s.key) out(LV_WARN "Settings: Unknown overlayKey '%s' in settings.ini, using `", s.keyName.c_str());
    if (s.asyncCompute && gFlagged) out(LV_OK "Async Compute: %s added to the game's command line", asyncFlag());
    else if (s.asyncCompute) out(LV_WARN "Async Compute: The game's command line imports weren't found, %s not added", asyncFlag());
    if (!refl::init(base)) {
        out(LV_BAD "Reflection: Table not found, runtime off");
        ov::setStatus("Reflection", "Not found: Mods off", lv::bad);
        return 0;
    }
    out(LV_OK "Reflection: %zu types", refl::count());
    ov::setStatus("Reflection", std::to_string(refl::count()) + " types", lv::ok);
    loadResIndex();
    waitForGameWindow();
    if (!installHook(base)) return 0;
    if (HANDLE t = CreateThread(nullptr, 0, watchLog, nullptr, 0, nullptr)) CloseHandle(t);
    out(LV_OK "Ready: Mods load once the game's resources are in. Type Lua in the console, e.g. print(Ecs.player())");
    attachResources();
    gResDone = true;
    // The game's Vulkan instance and its resources come in no fixed order: give the layer 10 s before calling it
    // missing. After gResDone, so mods never wait on it; here because set.log isn't open yet in DllMain.
    for (int i = 0; i < 40 && !ov::layerLoaded(); i++) Sleep(250);
    if (ov::layerLoaded()) {
        out(LV_OK "Overlay: Loaded, Vulkan loader %s", ov::loaderVersion().c_str());  // Loader version
        rt::setFeature("overlay", true);
    } else {
        // Wine first: on Linux that is the cause, whatever the token says
        NoOverlay why = onWine() ? NoOverlay::wine : ov::elevated() ? NoOverlay::admin : NoOverlay::bug;
        std::string v = ov::loaderVersion();
        if (why == NoOverlay::wine)
            out(LV_WARN "Overlay: Not loaded, Linux (Proton/Wine) isn't supported yet (not a bug). Vulkan loader %s.", v.c_str());
        else if (why == NoOverlay::admin)
            out(LV_WARN "Overlay: Not loaded, Steam or the game is running as administrator (not a bug). Start Steam and "
                        "the game normally. Vulkan loader %s.", v.c_str());
        else
            out(LV_BAD "Overlay: The game's Vulkan loader didn't load it, and the game isn't running as administrator: "
                       "Likely a bug, please report it with this log. %sVulkan loader %s.",
                ov::manifestOk() ? "" : "Writing shrouded_engine_tweaks\\layer\\shrouded_engine_tweaks.json failed. ", v.c_str());
        rt::setFeature("overlay", false, why == NoOverlay::wine    ? "Not supported on Linux (Proton/Wine) yet"
                                         : why == NoOverlay::admin ? "Steam or the game runs as administrator"
                                                                   : "The Vulkan loader didn't load it: See set.log");
        showOverlayDialog(why);
    }
    if (s.console) readConsole();
    return 0;
}

// One runtime per game. A second copy (SET's own DLL put in plugins\) stays inert, and loadPlugins says so. The
// first copy's handle stays open for the whole session.
bool firstCopy() {
    std::wstring name = L"Local\\ShroudedEngineTweaks_" + std::to_wstring(GetCurrentProcessId());
    HANDLE m = CreateMutexW(nullptr, FALSE, name.c_str());
    if (m && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(m);
        return false;
    }
    return true;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD why, void*) {
    if (why == DLL_PROCESS_ATTACH && isGame() && firstCopy()) {
        InitializeCriticalSection(&gOutCs);
        ov::setup(gDir, sink);  // here, not in init(): it must happen before the game loads Vulkan
        gSet = ov::readSettings();  // here too: the launch flag must be in before the game's CRT reads its command line
        if (gSet.key) ov::setToggleKey(gSet.key);
        ov::setDebugLog(gSet.debugLog);  // DebugLog
        ov::setFpsCap(gSet.fpsCap);
        opts::setAaOff(gSet.aaOff);
        ov::setHistory(gSet.historyLimit, gSet.saveHistory);  // before the overlay attaches and loads the history
        auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (gSet.asyncCompute) gFlagged = addLaunchFlag(base, asyncFlag());
        loadPlugins();
        if (HANDLE t = CreateThread(nullptr, 0, init, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
