// Shrouded Engine Tweaks. enshrouded.exe loads it as POWRPROF.dll from the game folder (docs/architecture.md).
// Hooks the ECS world tick and the game's per-frame options update: Lua mods run in the server world's tick in a world
// you host, and in the main thread's frame elsewhere. Also hooks the admin consumer, inventory_actions and the system
// runner, for Admin.* and Systems. Inert in any other exe.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "MinHook.h"
#include "exports.h"
#include "game.h"
#include "hooks.h"
#include "level.h"
#include "lua_api.h"
#include "overlay.h"
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
// (level.h). Called from the server, main, init, console and render threads.
void sink(const char* line) {
    ov::addLine(line);
    lv::Level l = lv::of(line);
    const char* text = line + (l != lv::none);
    EnterCriticalSection(&gOutCs);
    DWORD w;
    auto n = static_cast<DWORD>(strlen(text));
    for (HANDLE h : {gCon, gLog}) {
        if (h == INVALID_HANDLE_VALUE) continue;
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

// When the count of hook refusals and faults changed: logs the new ones and marks the Function Hooks Status row.
void hookStatus() {
    static std::atomic<uint64_t> seen{0};
    uint64_t n = hk::problems();
    if (seen.exchange(n) == n) return;
    hk::flushLogs();
    ov::setStatus("Function Hooks", std::to_string(n) + " refusal(s) or fault(s): see Hooks", lv::bad);
}

// Server world only (kind 0): pre-tick work, the game's tick, then Observe handlers while this tick's events are
// still in their streams (the session clears them after the tick returns). An access violation in Lua work turns the
// mods off (rt::tick catches it inside its lock).
void onTick(uintptr_t world, char flag) {
    bool server = *reinterpret_cast<const uint8_t*>(world + 8) == 0;
    if (server) { hookStatus(); rt::tick(world); }
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
        sink(LV_BAD "Admin commands: access violation, Admin.* off until the game restarts");
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
        out(LV_BAD "Admin.settings: game settings not found in the admin system (game updated?), unavailable");
        ov::setStatus("Game Settings", "not found: Admin.settings off", lv::bad);
    }
    auto target = reinterpret_cast<void*>(fn);
    if (!fn || MH_CreateHook(target, reinterpret_cast<void*>(&onAdminConsumer), reinterpret_cast<void**>(&gOrigAdmin)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        out(LV_BAD "Admin commands: consumer not found exactly once (game updated?), Admin.* unavailable");
        ov::setStatus("Admin Commands", "not found: Admin.* off", lv::bad);
        return;
    }
    rt::setAdminAvailable(true);
    out(LV_OK "Admin commands: hooked at exe+%llx", static_cast<unsigned long long>(fn - base));
    ov::setStatus("Admin Commands", "hooked", lv::ok);
}

// The game's inventory_actions system, which runs the admin create-item command: Admin.give's commands go into
// PlayerInput right before it, as with the admin consumer.
void onInventoryActions(void* job) {
    __try {
        rt::applyAdmin(job, 1);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        rt::setGiveAvailable(false);
        sink(LV_BAD "Admin.give: access violation, off until the game restarts");
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
        out(LV_BAD "Admin.give: inventory commands not found (game updated?), unavailable");
        ov::setStatus("Admin.give", "not found: Admin.give off", lv::bad);
        return;
    }
    rt::setGiveAvailable(true);
    out(LV_OK "Admin.give: hooked at exe+%llx", static_cast<unsigned long long>(fn - base));
    ov::setStatus("Admin.give", "hooked", lv::ok);
}

// Logs when the options check out, or once why they don't yet.
void optionsFrame(uintptr_t app) {
    static bool found, told;
    const char* why = opts::frame(app);
    if (!why && !std::exchange(found, true)) {
        out(LV_OK "Game options: found (machine settings v%u)", *reinterpret_cast<const uint32_t*>(opts::machine()));
        ov::setStatus("Game Options", "found", lv::ok);
    } else if (why && !found && !std::exchange(told, true)) {
        out(LV_WARN "Game options: not recognized yet (%s), still trying", why);
        ov::setStatus("Game Options", std::string("not recognized yet: ") + why, lv::warn);
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
            sink(LV_BAD "Game options: access violation, GameOptions off until the game restarts");
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
        out(LV_OK "Field of view: camera push at exe+%llx", static_cast<unsigned long long>(push - base));
    } else {
        out(LV_BAD "Field of view: camera push not found (game updated?), changes need the game's settings menu");
    }
    // The push only runs from the options hook, so the field of view applies live only once both are in.
    ov::setStatus("Field of View", "not found: set it in the game's menu", lv::bad);
    if (!opts::findLayout(base))
        out(LV_BAD "Game options: user settings layout not found (game updated?), GameOptions unavailable");
    uintptr_t fn = findOptionsUpdate(base);
    hk::reserve(fn, "options update");
    auto target = reinterpret_cast<void*>(fn);
    if (!fn || MH_CreateHook(target, reinterpret_cast<void*>(&onOptionsUpdate), reinterpret_cast<void**>(&gOrigOptions)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        out(LV_BAD "Game options: not found (game updated?), GameOptions unavailable");
        ov::setStatus("Game Options", "not found: GameOptions off", lv::bad);
        return;
    }
    out(LV_OK "Game options: hooked at exe+%llx", static_cast<unsigned long long>(fn - base));
    if (push) ov::setStatus("Field of View", "applies live", lv::ok);
}

// The game's program runner, which runs every ECS system, mostly on its worker threads: a system switched off is
// skipped whole, so the game's check that the system updated all its entities never runs either. While the profiler
// is on, each call is timed on its own thread.
void onRun(uintptr_t world, void* job, uint64_t index, uint64_t slot) {
    if (sys::skip(job)) return;
    if (!sys::profiling()) return gOrigRun(world, job, index, slot);
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    gOrigRun(world, job, index, slot);
    QueryPerformanceCounter(&b);
    sys::record(job, *reinterpret_cast<const uint8_t*>(world + 8), b.QuadPart - a.QuadPart);
}

// Optional, like hookAdmin: without it Systems.enable says it is unavailable, and everything else still runs.
void hookSystems(uintptr_t base) {
    size_t n = sys::scan(base);
    uintptr_t fn = findRunner(base);
    hk::reserve(fn, "system runner");
    auto target = reinterpret_cast<void*>(fn);
    if (!n || !fn || MH_CreateHook(target, reinterpret_cast<void*>(&onRun), reinterpret_cast<void**>(&gOrigRun)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        out(LV_BAD "Systems: not found (game updated?), %zu names, runner %s, Systems.enable unavailable", n, fn ? "found" : "not found");
        ov::setStatus("Systems", "not found: Systems off", lv::bad);
        return;
    }
    rt::setSystemsAvailable(true);
    out(LV_OK "Systems: %zu found, runner hooked at exe+%llx", n, static_cast<unsigned long long>(fn - base));
    ov::setStatus("Systems", std::to_string(n) + " found", lv::ok);
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
        out(LV_OK "Events: found");
        ov::setStatus("Events", "found", lv::ok);
        return;
    }
    out(LV_BAD "Events: event streams not found (game updated?), Observe and Events.post unavailable");
    ov::setStatus("Events", "not found: Observe and Events.post off", lv::bad);
}

bool installHook(uintptr_t base) {
    uintptr_t fn = findWorldTick(base);
    hk::reserve(fn, "world tick");
    if (!fn) {
        out(LV_BAD "World tick: pattern not found exactly once (game updated?), runtime off");
        ov::setStatus("World Tick", "not found: mods off", lv::bad);
        return false;
    }
    if (MH_Initialize() != MH_OK) {
        out(LV_BAD "World tick: hooking failed, runtime off");
        ov::setStatus("World Tick", "hook failed: mods off", lv::bad);
        return false;
    }
    hookAdmin(base);
    hookGive(base);
    hookOptions(base);
    hookSystems(base);
    findEvents(base);
    auto target = reinterpret_cast<void*>(fn);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&onTick), reinterpret_cast<void**>(&gOrigTick)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        out(LV_BAD "World tick: hooking failed, runtime off");
        ov::setStatus("World Tick", "hook failed: mods off", lv::bad);
        return false;
    }
    out(LV_OK "World tick: hooked at exe+%llx", static_cast<unsigned long long>(fn - base));
    ov::setStatus("World Tick", "hooked", lv::ok);
    ov::setStatus("Function Hooks", "ready (observe only)", lv::ok);
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
        out(LV_OK "English names: %zu texts (waited %.2f s)", res::textCount(), waits / 4.0);
        ov::setStatus("English Names", std::to_string(res::textCount()) + " texts", lv::ok);
    } else {
        out(LV_BAD "English names: not found (game updated?), Res.text unavailable");
        ov::setStatus("English Names", "not found: Res.text off", lv::bad);
    }
}

// The game loads its resources about half a second after its window shows. Waits up to 2 min for them.
void attachResources() {
    if (!res::count()) return;
    for (int i = 0; i < 480 && !res::attach(kResourceBase); i++) Sleep(250);
    auto base = static_cast<unsigned long long>(kResourceBase);
    if (!res::ready()) {
        out(LV_BAD "Resources: data not found at 0x%llx (game updated?), Res unavailable", base);
        ov::setStatus("Resources", "data not found: Res off", lv::bad);
        return;
    }
    out(LV_OK "Resources: %zu in %zu types, data at 0x%llx (protect 0x%x)", res::count(), res::types().size(), base,
        res::protection());
    ov::setStatus("Resources", std::to_string(res::count()) + " in " + std::to_string(res::types().size()) + " types",
                  lv::ok);
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
        out(LV_WARN "Game log: enshrouded.log not readable, onWorldLeave runs when the next world starts and onShutdown not on exit");
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

const char* asyncFlag() { return gSet.asyncCompute == 1 ? "--enable-async-compute" : "--disable-async-compute"; }

// The first lines: which version, on which game build, and whether that build was tested.
void logStart(uintptr_t base) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    out(LV_OK "Shrouded Engine Tweaks %s (%04u-%02u-%02u)", RUNTIME_VERSION, t.wYear, t.wMonth, t.wDay);
    ov::setStatus("Runtime", RUNTIME_VERSION, lv::ok);
    uint32_t build = gameBuild(base);
    bool tested = testedBuild(build);
    out(tested ? LV_OK "Game build: %u" : LV_WARN "Game build: %u", build);  // warn: untested, each feature checks itself
    ov::setStatus("Game Build", std::to_string(build), tested ? lv::ok : lv::warn);
}

DWORD WINAPI init(void*) {
    const ov::Settings& s = gSet;
    if (s.console) openConsole();
    openFiles();
    hk::setLog(sink);
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    logStart(base);
    if (!s.key) out(LV_WARN "Settings: unknown overlayKey '%s' in settings.ini, using `", s.keyName.c_str());
    if (s.asyncCompute && gFlagged) out(LV_OK "Async compute: %s added to the game's command line", asyncFlag());
    else if (s.asyncCompute) out(LV_WARN "Async compute: the game's command line imports weren't found, %s not added", asyncFlag());
    if (!refl::init(base)) {
        out(LV_BAD "Reflection: table not found, runtime off");
        ov::setStatus("Reflection", "not found: mods off", lv::bad);
        return 0;
    }
    out(LV_OK "Reflection: %zu types", refl::count());
    ov::setStatus("Reflection", std::to_string(refl::count()) + " types", lv::ok);
    loadResIndex();
    waitForGameWindow();
    if (!installHook(base)) return 0;
    if (HANDLE t = CreateThread(nullptr, 0, watchLog, nullptr, 0, nullptr)) CloseHandle(t);
    out(LV_OK "Ready: mods load once the game's resources are in. Type Lua in the console, e.g. print(Ecs.player())");
    attachResources();
    gResDone = true;
    if (s.console) readConsole();
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD why, void*) {
    if (why == DLL_PROCESS_ATTACH && isGame()) {
        InitializeCriticalSection(&gOutCs);
        ov::setup(gDir, sink);  // here, not in init(): it must happen before the game loads Vulkan
        gSet = ov::readSettings();  // here too: the launch flag must be in before the game's CRT reads its command line
        if (gSet.key) ov::setToggleKey(gSet.key, gSet.keyName);
        ov::setFpsCap(gSet.fpsCap);
        opts::setAaOff(gSet.aaOff);
        ov::setHistory(gSet.historyLimit, gSet.saveHistory);  // before the overlay attaches and loads the history
        auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (gSet.asyncCompute) gFlagged = addLaunchFlag(base, asyncFlag());
        if (HANDLE t = CreateThread(nullptr, 0, init, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
