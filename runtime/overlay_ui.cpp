// The overlay's UI half: the toolbar and built-in windows (console, bindings, resources, systems, hooks, game log,
// settings, ImGui debug) and the game window's input. While the overlay is open, keyboard and mouse messages go to
// ImGui and never reach the game's window procedure, which is where the game reads all of them. The gamepad is not
// blocked.
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "game.h"
#include "hooks.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "inspect.h"
#include "lua_api.h"
#include "modsettings.h"
#include "overlay.h"
#include "report.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

std::mutex ov::uiMx;
std::wstring ov::gameDir;

namespace {

constexpr size_t kMaxLines = 2000, kMaxLogLines = 5000;
const ImVec4 kGreen(0.45f, 0.9f, 0.45f, 1.0f), kYellow(1.0f, 0.85f, 0.3f, 1.0f), kRed(1.0f, 0.4f, 0.4f, 1.0f);

constexpr char kSettings[] =
    "; Shrouded Engine Tweaks settings, read when the game starts.\r\n"
    "[runtime]\r\n"
    "; 1 = also open the separate console window. The in-game console is always available.\r\n"
    "console=0\r\n"
    "; Key that opens the in-game console: `, F1-F24, Insert, Home, End, Delete, PageUp, PageDown, Pause,\r\n"
    "; or a virtual-key code like 0xC0. Pick a key you don't type. Empty: the overlay asks for it at the next start.\r\n"
    "overlayKey=\r\n"
    "; Frame rate cap: 0 = off, or 10 and up.\r\n"
    "fpsCap=0\r\n"
    "; Async compute: default (the game decides by graphics card), on or off. Applies at the next start.\r\n"
    "asyncCompute=default\r\n"
    "; Antialiasing: default (the game's setting) or off, which the game's own menu doesn't offer.\r\n"
    "antiAliasing=default\r\n"
    "; Console history: how many entered lines Up and Down remember (1-10000). saveHistory=1 keeps them across game\r\n"
    "; starts in console_history.txt, which holds what you type; 0 forgets them when the game closes.\r\n"
    "historyLimit=100\r\n"
    "saveHistory=1\r\n"
    "; 1 = extra lines in set.log that help with bug reports (overlay key, drawing problems). 0 for normal play.\r\n"
    "debugLog=1\r\n";
const wchar_t kGitHubBugs[] = L"https://github.com/Cacaida/Shrouded-Engine-Tweaks/issues/new/choose";
const wchar_t kNexusBugs[] = L"https://www.nexusmods.com/enshrouded/mods/303?tab=bugs";
const wchar_t* const kAsyncNames[] = {L"default", L"on", L"off"};  // settings.ini asyncCompute, by Settings value

std::atomic<int> gToggleKey{VK_OEM_3};  // settings.ini overlayKey; ` and ~ on a US layout
std::string gKeyName = "`";  // as the keyboard layout labels it (keyLabel)
HWND gWnd;

// The keyboard layout the game window's input uses.
HKL inputLayout() { return GetKeyboardLayout(gWnd ? GetWindowThreadProcessId(gWnd, nullptr) : 0); }

// A key as the player's keyboard layout labels it: the character a punctuation key types there (Ü, ß or ´ on a German
// layout, ] on a US one), else its name. Only for display: stored names stay the same on every layout (rt::keyName).
// The character a punctuation key types on that layout, 0 for any other key. The top bit marks a dead key (´ ^).
wchar_t keyChar(int vk) {
    bool oem = (vk >= VK_OEM_1 && vk <= VK_OEM_3) || (vk >= VK_OEM_4 && vk <= VK_OEM_8) || vk == VK_OEM_102;
    return oem ? wchar_t(MapVirtualKeyExW(UINT(vk), MAPVK_VK_TO_CHAR, inputLayout()) & 0xFFFF) : 0;
}

std::string keyLabel(int vk) {
    wchar_t c = keyChar(vk);
    if (c <= L' ') return rt::keyName(vk);
    CharUpperBuffW(&c, 1);
    char s[8];
    int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, s, sizeof s, nullptr, nullptr);
    return std::string(s, size_t(std::max(n, 0)));
}
WNDPROC gOrig;
std::atomic<bool> gOpen, gDisabled;
std::atomic<bool> gAskKey;  // first start: the overlay is open until its key is picked
bool gAskOnStart;           // settings.ini has no overlayKey yet: none was ever picked
std::atomic<uint64_t> gHintEnd;  // the start hint shows until then (GetTickCount64), while the overlay is closed
bool gAskRefused;           // a key that can't be the overlay key was pressed
bool gDropChar;  // the WM_CHAR that follows the toggle key must not be typed
std::string gIni;
float gScale = 1;  // window height / 1080, applied to the style
std::atomic<float> gNewScale;  // a new window height's scale (ov::resize), applied before the next frame
bool gCrashPrompt;             // the last session didn't close normally and the Last Session window wasn't dismissed
std::atomic<bool> gCrashOnly;  // the overlay opened by itself for that window: only it shows, and closing it closes the overlay
std::string gReportMsg;

// The built-in windows, toggled from the toolbar.
enum Win { WinConsole, WinBinds, WinModSet, WinRes, WinEnt, WinSys, WinHooks, WinLog, WinCompat, WinSettings, WinDebug, WinCount };
const char* const kWinNames[WinCount] = {"Console", "Bindings", "Mod Settings",  "Resources", "Entities",   "Systems",
                                         "Hooks",   "Game Log", "Compatibility", "Settings",  "ImGui Debug"};
bool gShow[WinCount] = {true};
bool gMetrics, gIdStack;

// A scrolling list of text rows: the console output and the game log. The view wraps rows at the window's width and
// shows only those that pass the Ctrl+F filter.
struct Lines {
    std::deque<std::string> rows;
    uint64_t rev = 0;  // bumped on every change to rows
    bool autoScroll = true, scrollDown = false;
    bool finding = false, focusFind = false;
    ImGuiTextFilter find;
    std::vector<std::pair<uint32_t, uint32_t>> vis;  // one per screen line: its row, and where in the row it starts
    uint64_t visRev = ~0ull;                         // what vis was built for
    float visWidth = 0;
    std::string visFind;
};

std::mutex gLinesMx;  // console rows: lines arrive from any thread
Lines gCon;

char gInput[1024];
std::vector<std::string> gHistory;  // oldest first, at most gHistLimit; render thread only
int gHistPos = -1;
int gHistLimit = 100;
bool gSaveHist = true;  // settings.ini saveHistory: gHistory is kept in console_history.txt
bool gFocusInput;

// The game's own log, enshrouded.log in the game folder. Render thread only.
struct GameLog {
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD err = 0;
    uint64_t pos = 0;
    std::string partial;  // a line the game hasn't finished writing
    ULONGLONG next = 0;
    Lines lines;
} gLog;

std::string gCapture;  // the binding waiting for a key press in the Bindings window, empty if none
bool gWasDown[256];    // key states at the previous frame, while capturing

struct StatusRow {
    std::string key, value;
    lv::Level level;
};
std::mutex gStatusMx;
std::vector<StatusRow> gStatus;  // in the order first set (ov::setStatus)

ov::Settings gEdit;  // the Settings window's fields
char gKeyBuf[32];
bool gEditLoaded;
std::string gSettingsMsg, gStyleMsg;
enum StyleOp { StyleNone, StyleLoad, StyleDefault } gStyleOp;  // applied before the next frame

bool isInput(UINT m) {
    return m == WM_INPUT || m == WM_KEYDOWN || m == WM_KEYUP || m == WM_CHAR || m == WM_DEADCHAR || m == WM_UNICHAR ||
           (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_SETCURSOR || m == WM_MOUSEHOVER || m == WM_MOUSELEAVE;
}

void toggle() {
    bool open = !gOpen;
    if (open) {
        CallWindowProcW(gOrig, gWnd, WM_KILLFOCUS, 0, 0);  // the game releases keys held right now
        std::lock_guard lock(ov::uiMx);
        ImGui::GetIO().ClearInputKeys();
        gFocusInput = gCon.scrollDown = true;
        gCapture.clear();  // a capture left when the overlay closed
        gHintEnd = 0;      // the key is known now
    }
    if (!open) gCrashOnly = false;  // closed by its key: the next open is the normal overlay (Last Session included)
    gOpen = open;
    rt::setOverlayOpen(open);
    ov::log(open ? LV_LOG "Overlay: Opened" : LV_LOG "Overlay: Closed");  // Open/close line
}

// The overlay key pressed alone. With Ctrl or Shift held it's another binding (another mod's Ctrl+F11), so it passes
// on; Alt+key comes as WM_SYSKEYDOWN and always did. GetKeyState: the modifiers as of this message.
bool toggleKeyDown(UINT m, WPARAM w) {
    return m == WM_KEYDOWN && static_cast<int>(w) == gToggleKey && GetKeyState(VK_CONTROL) >= 0 &&
           GetKeyState(VK_SHIFT) >= 0 && GetKeyState(VK_MENU) >= 0;
}

// Open: input goes to ImGui only. WM_INPUT still needs DefWindowProc to free the raw input. System keys (Alt, F10)
// still go to the game, which only uses them to keep Windows from entering menu mode.
LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    bool toggleKey = toggleKeyDown(m, w);
    if (toggleKey && (gDisabled || gAskKey) && !(l & (1 << 30)))
        ov::debug(gDisabled ? "Overlay Key: Ignored, the overlay is off after an error"  // Ignored key
                            : "Overlay Key: Ignored while the key window is open");
    if (toggleKey && !gDisabled && !gAskKey) {
        if (!(l & (1 << 30))) toggle();  // bit 30: auto-repeat
        gDropChar = true;
        return 0;
    }
    if (m == WM_CHAR && gDropChar) {
        gDropChar = false;
        return 0;
    }
    if (m == WM_KEYDOWN || m == WM_DEADCHAR) gDropChar = false;
    if (!gOpen || !isInput(m)) return CallWindowProcW(gOrig, h, m, w, l);
    LRESULT r;
    {
        std::lock_guard lock(ov::uiMx);
        r = ImGui_ImplWin32_WndProcHandler(h, m, w, l);
    }
    if (m == WM_SETCURSOR) return r ? TRUE : DefWindowProcW(h, m, w, l);
    return m == WM_INPUT ? DefWindowProcW(h, m, w, l) : 0;
}

// Up/Down in the input line walk the history.
int onHistory(ImGuiInputTextCallbackData* d) {
    int n = static_cast<int>(gHistory.size());
    if (n == 0) return 0;
    if (d->EventKey == ImGuiKey_UpArrow) gHistPos = gHistPos < 0 ? n - 1 : std::max(0, gHistPos - 1);
    else if (gHistPos >= 0 && ++gHistPos >= n) gHistPos = -1;
    d->DeleteChars(0, d->BufTextLen);
    if (gHistPos >= 0) d->InsertChars(0, gHistory[gHistPos].c_str());
    return 0;
}

std::wstring historyPath() { return ov::gameDir + L"shrouded_engine_tweaks\\console_history.txt"; }

void trimHistory() {
    if (gHistory.size() > size_t(gHistLimit)) gHistory.erase(gHistory.begin(), gHistory.end() - gHistLimit);
}

// console_history.txt: one entered line per row, oldest first. Rewritten whole after every entry.
void saveHistory() {
    if (ov::gameDir.empty() || !gSaveHist) return;
    std::string all;
    for (const std::string& l : gHistory) all += l + '\n';
    HANDLE h = CreateFileW(historyPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, all.data(), static_cast<DWORD>(all.size()), &w, nullptr);
    CloseHandle(h);
}

void loadHistory() {
    if (ov::gameDir.empty() || !gSaveHist) return;
    HANDLE h = CreateFileW(historyPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    std::string all;
    DWORD got = 0;
    if (GetFileSizeEx(h, &size) && size.QuadPart < (16 << 20)) {
        all.resize(static_cast<size_t>(size.QuadPart));
        if (!ReadFile(h, all.data(), static_cast<DWORD>(all.size()), &got, nullptr)) got = 0;
    }
    CloseHandle(h);
    all.resize(got);
    for (size_t at = 0; at < all.size();) {
        size_t nl = std::min(all.find('\n', at), all.size());
        std::string line = all.substr(at, nl - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) gHistory.push_back(std::move(line));
        at = nl + 1;
    }
    trimHistory();
}

void submit() {
    std::string line = gInput;
    gInput[0] = 0;
    gHistPos = -1;
    if (line.find_first_not_of(' ') == std::string::npos) return;
    if (gHistory.empty() || gHistory.back() != line) {
        gHistory.push_back(line);
        trimHistory();
        saveHistory();
    }
    ov::log("> %s", line.c_str());  // echo to the overlay, the console window and set.log
    rt::queue(line);
    gCon.scrollDown = true;
}

// Splits the rows that pass the filter into screen lines of at most width, so the clipper below gets lines of one
// height. Rebuilt when the rows, the width or the filter change.
// Limit: rebuilds every row on any change; make it incremental if a mod printing every frame shows in frame times.
void wrapLines(Lines& l, float width) {
    if (l.visRev == l.rev && l.visWidth == width && l.visFind == l.find.InputBuf) return;
    l.visRev = l.rev;
    l.visWidth = width;
    l.visFind = l.find.InputBuf;
    l.vis.clear();
    ImFont* font = ImGui::GetFont();
    float size = ImGui::GetFontSize();
    for (uint32_t i = 0; i < l.rows.size(); i++) {
        const char *s = l.rows[i].data(), *end = s + l.rows[i].size(), *at = s + (lv::of(s) != lv::none);
        if (!l.find.PassFilter(at, end)) continue;
        do {
            l.vis.emplace_back(i, static_cast<uint32_t>(at - s));
            const char* next = font->CalcWordWrapPosition(size, at, end, width);
            at = next > at ? next : end;
            while (at < end && *at == ' ') at++;  // a wrapped line doesn't start with the spaces it broke at
        } while (at < end);
    }
}

// The Ctrl+F box: case-insensitive, "a,b" shows rows with either, "-a" hides rows with a. Esc or x closes it.
void drawFind(Lines& l) {
    if (l.focusFind) ImGui::SetKeyboardFocusHere();
    l.focusFind = false;
    float x = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(-(x + ImGui::GetStyle().ItemSpacing.x));
    if (ImGui::InputTextWithHint("##find", "Find: Text, a,b = either, -a = hide", l.find.InputBuf, sizeof l.find.InputBuf))
        l.find.Build();
    bool esc = ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape);
    ImGui::SameLine();
    if (ImGui::Button("x", ImVec2(x, 0)) || esc) {
        l.find.Clear();
        l.finding = false;
    }
}

// A row's color: its level marker (level.h), or the game's own "[W" and "[E" (warnings, errors). Null: plain.
const ImVec4* rowColor(const std::string& r) {
    switch (r.empty() ? 0 : r[0]) {
    case lv::ok: return &kGreen;
    case lv::warn: return &kYellow;
    case lv::bad: return &kRed;
    case '[': return r.size() < 2 ? nullptr : r[1] == 'W' ? &kYellow : r[1] == 'E' ? &kRed : nullptr;
    }
    return nullptr;
}

// A window's one-line result message (Save, Load...), colored by its level marker.
void drawMsg(const std::string& m) {
    if (m.empty()) return;
    const ImVec4* c = rowColor(m);
    const char* text = m.c_str() + (lv::of(m.c_str()) != lv::none);
    if (c) ImGui::TextColored(*c, "%s", text);
    else ImGui::TextUnformatted(text);
}

// Clear, Auto-scroll and the find box, then the rows, wrapped and colored (rowColor), in a child that leaves `footer`
// below it. With Auto-scroll on, the view follows new rows while it is at the bottom.
void drawLines(Lines& l, float footer) {
    if (ImGui::Button("Clear")) {
        l.rows.clear();
        l.rev++;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Auto-Scroll", &l.autoScroll) && l.autoScroll) l.scrollDown = true;
    bool here = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (here && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F))
        l.finding = l.focusFind = true;  // not Shortcut(): an active console input line would keep it
    ImGui::SameLine();
    if (l.finding) drawFind(l);
    else ImGui::TextDisabled("Ctrl+F: Find");
    if (ImGui::BeginChild("lines", ImVec2(0, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
        wrapLines(l, ImGui::GetContentRegionAvail().x);  // the fixed scrollbar keeps the width from flip-flopping
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(l.vis.size()));
        while (clip.Step())
            for (size_t i = size_t(clip.DisplayStart); i < size_t(clip.DisplayEnd); i++) {
                auto [row, at] = l.vis[i];
                const std::string& r = l.rows[row];
                size_t to = i + 1 < l.vis.size() && l.vis[i + 1].first == row ? l.vis[i + 1].second : r.size();
                const ImVec4* c = rowColor(r);
                if (c) ImGui::PushStyleColor(ImGuiCol_Text, *c);
                ImGui::TextUnformatted(r.data() + at, r.data() + to);
                if (c) ImGui::PopStyleColor();
            }
        if (l.autoScroll && (l.scrollDown || ImGui::GetScrollY() >= ImGui::GetScrollMaxY())) ImGui::SetScrollHereY(1.0f);
        l.scrollDown = false;
    }
    ImGui::EndChild();
}

void drawConsole() {
    float s = gScale;
    ImGui::SetNextWindowPos(ImVec2(40 * s, 40 * s), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(900 * s, 420 * s), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Shrouded Engine Tweaks", &gShow[WinConsole])) {
        {
            std::lock_guard lock(gLinesMx);
            drawLines(gCon, ImGui::GetFrameHeightWithSpacing());
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (gFocusInput) ImGui::SetKeyboardFocusHere();
        gFocusInput = false;
        auto flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory;
        if (ImGui::InputText("##input", gInput, sizeof gInput, flags, onHistory)) {
            submit();
            ImGui::SetKeyboardFocusHere(-1);  // keep typing after Enter
        }
    }
    ImGui::End();
}

// Appends what the game wrote to the log rows, one row per line, leaving out the ones with nothing for a modder
// (logNoise). enshrouded.log keeps them.
void addLogText(const char* p, size_t n) {
    for (const char* end = p + n; p < end;) {
        auto nl = static_cast<const char*>(memchr(p, '\n', static_cast<size_t>(end - p)));
        gLog.partial.append(p, nl ? nl : end);
        if (!nl) break;
        if (!gLog.partial.empty() && gLog.partial.back() == '\r') gLog.partial.pop_back();
        std::string row = std::exchange(gLog.partial, {});
        if (!logNoise(row)) gLog.lines.rows.push_back(std::move(row));
        p = nl + 1;
    }
    while (gLog.lines.rows.size() > kMaxLogLines) gLog.lines.rows.pop_front();
    gLog.lines.rev++;
}

// Reads what the game appended to enshrouded.log, at most every 250 ms. Opened shared, so the game keeps writing.
void pollGameLog() {
    ULONGLONG now = GetTickCount64();
    if (now < gLog.next) return;
    gLog.next = now + 250;
    if (gLog.file == INVALID_HANDLE_VALUE) {
        gLog.file = CreateFileW((ov::gameDir + L"enshrouded.log").c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        gLog.err = gLog.file == INVALID_HANDLE_VALUE ? GetLastError() : 0;
        if (gLog.err) return;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(gLog.file, &size)) return;
    if (static_cast<uint64_t>(size.QuadPart) < gLog.pos) {  // the file started over
        gLog.pos = 0;
        gLog.partial.clear();
        SetFilePointerEx(gLog.file, LARGE_INTEGER{}, nullptr, FILE_BEGIN);
    }
    static char buf[64 * 1024];
    DWORD got = 0;
    while (ReadFile(gLog.file, buf, sizeof buf, &got, nullptr) && got > 0) {
        gLog.pos += got;
        addLogText(buf, got);
    }
}

void drawGameLog() {
    float s = gScale;
    ImGui::SetNextWindowPos(ImVec2(40 * s, 480 * s), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(900 * s, 420 * s), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Game Log", &gShow[WinLog])) {
        if (ov::gameDir.empty()) {
            ImGui::TextUnformatted("The game log is only available in the game.");
        } else {
            pollGameLog();
            if (gLog.err) ImGui::TextColored(kRed, "Can't open enshrouded.log: Windows error %lu", gLog.err);
            else drawLines(gLog.lines, 0);
        }
    }
    ImGui::End();
}

bool keyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void snapKeys() {
    for (int vk = 1; vk < 256; vk++) gWasDown[vk] = keyDown(vk);
}

void startCapture(const std::string& key) {
    gCapture = key;
    snapKeys();
}

// Mouse buttons, modifiers and Win never count as a key press.
bool plainKey(int vk) {
    return vk > VK_XBUTTON2 && vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU && vk != VK_LWIN && vk != VK_RWIN &&
           !(vk >= VK_LSHIFT && vk <= VK_RMENU);
}

// The first plain key that went down since the last call (or snapKeys), 0 if none. Only while the game window is in
// front: keys typed in another program never count.
int pressedKey() {
    bool front = GetForegroundWindow() == gWnd;
    for (int vk = 1; vk < 256; vk++) {
        bool now = keyDown(vk), press = now && !gWasDown[vk];
        gWasDown[vk] = now;
        if (press && front && plainKey(vk)) return vk;
    }
    return 0;
}

// The first key that goes down since the click binds, with the modifiers held at that moment. Esc cancels. The
// overlay key can't be a binding's key.
void pollCapture() {
    int vk = pressedKey();
    if (!vk || vk == gToggleKey) return;
    if (vk != VK_ESCAPE) rt::setBinding(gCapture, rt::heldMods(), vk);
    gCapture.clear();
}

// The key left of 1, by its place on the keyboard (scan code 0x29): ` on a US layout, ^ on a German one. Not ` by its
// key code: on a German keyboard that's Ö, which types a letter (in the game's chat, say).
int leftOfOne() { return int(MapVirtualKeyExW(0x29, MAPVK_VSC_TO_VK, inputLayout())); }

// Keys that type nothing and close no menu. F10 is a system key: the window procedure never gets it as WM_KEYDOWN, so
// it could never open the overlay.
bool overlayKeyOk(int vk) {
    return vk == leftOfOne() || (vk >= VK_F1 && vk <= VK_F24 && vk != VK_F10) || vk == VK_INSERT || vk == VK_DELETE ||
           vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_PAUSE;
}

void setOverlayKey(int vk) {
    ov::Settings s = ov::readSettings();
    s.keyName = rt::keyName(vk);
    if (!ov::saveSettings(s)) ov::log(LV_BAD "Settings: Saving settings.ini failed, %s works until the game closes", s.keyName.c_str());
    ov::setToggleKey(vk);
    gEditLoaded = false;  // the Settings window shows the new key
    gAskKey = false;
    ov::log(LV_OK "Overlay Key: %s opens and closes the overlay", gKeyName.c_str());
}

// First start: a window in the middle that stays until the overlay key is picked. The overlay is open meanwhile, so
// the game gets no input.
void drawAskKey() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    auto flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Overlay Key", nullptr, flags)) {
        ImGui::TextUnformatted("Press the key that opens and closes this overlay.");
        ImGui::TextDisabled("%s  F1-F24 (not F10)  Insert  Delete  Home  End  PageUp  PageDown  Pause",
                            keyLabel(leftOfOne()).c_str());
        int vk = pressedKey();
        if (vk && rt::heldMods()) vk = 0;  // with Ctrl, Alt or Shift it's another app's hotkey (Alt+F9)
        if (vk && overlayKeyOk(vk)) setOverlayKey(vk);
        else if (vk) gAskRefused = true;
        if (gAskRefused) ImGui::TextColored(kRed, "That key types text, closes menus or is a system key: Pick one above.");
    }
    ImGui::End();
}

bool hintDue() { return !gOpen && GetTickCount64() < gHintEnd; }

// Every start, for a few seconds while the overlay is closed: which key opens it. Takes no input.
void drawHint() {
    ImGui::SetNextWindowPos(ImVec2(10 * gScale, 10 * gScale));
    ImGui::SetNextWindowBgAlpha(0.6f);
    auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##hint", nullptr, flags)) {
        if (gCrashPrompt)
            ImGui::TextColored(kYellow, "Shrouded Engine Tweaks: The last session closed unexpectedly. %s opens the "
                                        "overlay to copy its log for a bug report.", gKeyName.c_str());
        else ImGui::Text("Shrouded Engine Tweaks: %s opens the overlay", gKeyName.c_str());
    }
    ImGui::End();
}

void copyLog(bool prev) {
    gReportMsg = rep::copyText(rep::logText(ov::gameDir, prev))
                     ? LV_OK "Copied to the clipboard: Paste it into the bug report."
                     : LV_BAD "Copying failed. The file is shrouded_engine_tweaks\\set.log (set.prev.log for the last session).";
}

// In the browser. Its own thread: the shell can take a while, and it wants COM.
void openPage(const wchar_t* url) {
    std::thread([url] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
}

// Settings > Bug Report: both copy buttons and the two places to report.
void drawReportButtons() {
    if (ImGui::Button("Copy to Clipboard")) copyLog(false);
    ImGui::SameLine();
    if (ImGui::Button("Copy last session's logs")) copyLog(true);
    if (ImGui::Button("Report on GitHub")) openPage(kGitHubBugs);
    ImGui::SameLine();
    if (ImGui::Button("Report on Nexus")) openPage(kNexusBugs);
    drawMsg(gReportMsg);
}

// After a session that didn't close normally: what the game's crash report says, and its log one click from the
// clipboard.
void drawLastSession() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    auto flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Last Session", &gCrashPrompt, flags)) {
        const std::string& r = rep::lastSession().report;
        ImGui::PushTextWrapPos(480 * gScale);
        ImGui::TextUnformatted("The last session didn't close normally (a crash, unexpectedly, or from the outside).");
        ImGui::TextColored(kYellow, "%s", r.empty() ? "No game crash report was written." : r.c_str());
        ImGui::TextUnformatted("\"Copy to Clipboard\" copies that session's log (set.prev.log) to your clipboard, use "
                               "it when reporting bugs. Your Windows user folder in it shows as %USERPROFILE%.");
        if (gCrashOnly)
            ImGui::TextDisabled("Dismiss goes back to the game. Settings > Bug Report in the overlay (%s) copies it later too.",
                                gKeyName.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Copy to Clipboard")) copyLog(true);
        ImGui::SameLine();
        if (ImGui::Button("Dismiss")) gCrashPrompt = false;
        // The report pair on the right edge of the same row.
        const ImGuiStyle& st = ImGui::GetStyle();
        float right = ImGui::CalcTextSize("Report on GitHub").x + ImGui::CalcTextSize("Report on Nexus").x +
                      4 * st.FramePadding.x + st.ItemSpacing.x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - right));
        if (ImGui::Button("Report on GitHub")) openPage(kGitHubBugs);
        ImGui::SameLine();
        if (ImGui::Button("Report on Nexus")) openPage(kNexusBugs);
        drawMsg(gReportMsg);
    }
    ImGui::End();
    if (!gCrashPrompt && gCrashOnly) {  // dismissed (button or X): the overlay it opened closes with it
        gCrashOnly = false;
        gOpen = false;
        rt::setOverlayOpen(false);
        ov::log(LV_LOG "Overlay: Closed");  // Open/close line
    }
}

void drawBindRow(const rt::BindInfo& b) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(b.label.c_str());
    if (b.input) {
        ImGui::SameLine();
        ImGui::TextDisabled("(input)");
    }
    ImGui::TableNextColumn();
    ImGui::PushID(b.key.c_str());
    bool capturing = gCapture == b.key;
    // The stored combo ends in the key's stored name; it shows the layout's label instead (Ctrl+Semicolon -> Ctrl+Ü).
    std::string shown = b.combo.substr(0, b.combo.size() - std::min(b.combo.size(), rt::keyName(b.vk).size())) + keyLabel(b.vk);
    std::string text = capturing ? "Waiting for a key..." : b.combo.empty() ? "Unbound" : shown;
    float x = ImGui::GetFrameHeight();
    // The space: ImGui hides a label from its first "##", so a key labeled # (+ "###bind") would show nothing.
    if (ImGui::Button((text + " ###bind").c_str(), ImVec2(-(x + ImGui::GetStyle().ItemSpacing.x), 0)) && !capturing)
        startCapture(b.key);
    ImGui::SameLine();
    ImGui::BeginDisabled(b.combo.empty());
    if (ImGui::Button("x", ImVec2(x, 0))) rt::setBinding(b.key, 0, 0);
    ImGui::EndDisabled();
    ImGui::PopID();
}

// Hotkeys and inputs that mods registered, one collapsible table per mod (rows come sorted by mod). Saved as they
// change. "###mod" keeps a mod's open state when its count changes.
void drawBindings() {
    ImGui::SetNextWindowPos(ImVec2(1560 * gScale, 40 * gScale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340 * gScale, 420 * gScale), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Bindings", &gShow[WinBinds])) {
        if (!gCapture.empty()) pollCapture();
        if (!gCapture.empty())
            ImGui::TextColored(kYellow, "Press the key now, with the overlay open (Ctrl, Shift, Alt allowed). Esc cancels.");
        int dead = 0;
        std::vector<rt::BindInfo> rows = rt::bindings(dead);
        if (rows.empty()) ImGui::TextWrapped("No mod has registered a hotkey or input (Keys.hotkey, Keys.input).");
        for (size_t i = 0, j; i < rows.size(); i = j) {
            for (j = i; j < rows.size() && rows[j].mod == rows[i].mod; j++) {}
            ImGui::PushID(rows[i].mod.c_str());
            std::string head = rows[i].mod + " (" + std::to_string(j - i) + ")###mod";
            if (ImGui::CollapsingHeader(head.c_str(), ImGuiTreeNodeFlags_DefaultOpen) &&
                ImGui::BeginTable("binds", 2, ImGuiTableFlags_SizingStretchSame)) {
                for (size_t k = i; k < j; k++) drawBindRow(rows[k]);
                ImGui::EndTable();
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::BeginDisabled(dead == 0);
        if (ImGui::Button(("Remove dead bindings (" + std::to_string(dead) + ")").c_str())) rt::removeDeadBindings();
        ImGui::EndDisabled();
    }
    ImGui::End();
}

// One setting's widget. An edit goes to the store at once: the mod gets onChange at its next tick, and the file is
// saved within a second. The key shows as the tooltip, for the mod's author.
void drawSetting(const std::string& mod, mset::Field& f) {
    constexpr double kExact = 9007199254740992.0;  // an int setting without min or max reaches 2^53
    ImGui::PushID(f.key.c_str());
    const char* label = f.label.c_str();
    bool changed = false;
    if (f.type == mset::Bool) {
        bool v = f.num != 0;
        changed = ImGui::Checkbox(label, &v);
        f.num = v;
    } else if (f.type == mset::Number) {
        changed = std::isfinite(f.lo) && std::isfinite(f.hi)
                      ? ImGui::SliderScalar(label, ImGuiDataType_Double, &f.num, &f.lo, &f.hi, "%.3g")
                      : ImGui::InputDouble(label, &f.num, 0, 0, "%.6g");
    } else if (f.type == mset::Int) {
        long long v = static_cast<long long>(f.num), lo = static_cast<long long>(f.lo), hi = static_cast<long long>(f.hi);
        long long step = 1;
        changed = f.lo > -kExact && f.hi < kExact ? ImGui::SliderScalar(label, ImGuiDataType_S64, &v, &lo, &hi)
                                                  : ImGui::InputScalar(label, ImGuiDataType_S64, &v, &step);
        f.num = static_cast<double>(v);
    } else if (f.type == mset::Choice) {
        size_t at = static_cast<size_t>(f.num);
        if (ImGui::BeginCombo(label, f.values[at].c_str())) {
            for (size_t i = 0; i < f.values.size(); i++)
                if (ImGui::Selectable(f.values[i].c_str(), i == at)) f.num = static_cast<double>(i), changed = true;
            ImGui::EndCombo();
        }
    } else {
        std::vector<char> buf(f.maxLen + 1);
        memcpy(buf.data(), f.text.data(), std::min(f.text.size(), f.maxLen));
        changed = ImGui::InputText(label, buf.data(), buf.size());
        f.text = buf.data();
    }
    ImGui::SetItemTooltip("%s", f.key.c_str());
    if (changed) mset::set(mod, f.key, f.num, f.text, true);
    ImGui::PopID();
}

// The mods' Settings.define pages, one collapsible section per mod.
void drawModSettings() {
    ImGui::SetNextWindowSize(ImVec2(440 * gScale, 420 * gScale), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Mod Settings", &gShow[WinModSet])) {
        std::vector<mset::Page> pages = mset::pages();
        if (pages.empty()) ImGui::TextWrapped("No mod has settings (Settings.define).");
        for (mset::Page& p : pages) {
            ImGui::PushID(p.mod.c_str());
            if (ImGui::CollapsingHeader(p.mod.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                for (mset::Field& f : p.fields) drawSetting(p.mod, f);
            ImGui::PopID();
        }
    }
    ImGui::End();
}

void setEdit(const ov::Settings& s) {
    gEdit = s;
    strncpy_s(gKeyBuf, s.keyName.c_str(), _TRUNCATE);
}

// Every game option as "machine.graphics.vsync = false" lines, to the console and set.log. Hosting passwords and
// banned accounts are never in it (opts::dump).
void dumpOptions() {
    if (!opts::ready()) return ov::log(LV_WARN "Game Options: Not found yet, see set.log");
    size_t n = opts::dump("", [](const std::string& s) { ov::log("%s", s.c_str()); });
    ov::log(LV_OK "Game Options: %zu values", n);
}

// Load re-reads settings.ini, Save writes it, Default only resets the fields. The key, the frame rate cap,
// antialiasing and the console history apply on Save. A cap under 10 isn't saved: the overlay itself would run that
// slow.
// Mod Settings and Compatibility open from here, right-aligned on the Dump Game Options row, lit while open.
void drawSettingsWindows() {
    const ImGuiStyle& st = ImGui::GetStyle();
    float right = ImGui::CalcTextSize("Mod Settings").x + ImGui::CalcTextSize("Compatibility").x +
                  4 * st.FramePadding.x + st.ItemSpacing.x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - right));
    for (Win w : {WinModSet, WinCompat}) {
        if (w == WinCompat) ImGui::SameLine();
        bool on = gShow[w];
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(kWinNames[w])) gShow[w] = !on;
        if (on) ImGui::PopStyleColor();
    }
}

void drawSettingsBody() {
    static const char* const kAsync[] = {"Game Default", "On", "Off"};
    ImGui::Checkbox("Separate Console Window (Applies on Restart)", &gEdit.console);
    if (ImGui::InputText("Overlay Key", gKeyBuf, sizeof gKeyBuf)) gEdit.keyName = gKeyBuf;
    int vk = rt::keyFromName(gKeyBuf);
    if (!vk) ImGui::TextColored(kRed, "Unknown key: Use `, F1-F24, Insert, Home, End, Delete, PageUp, PageDown, Pause or 0xNN");
    ImGui::InputInt("FPS Cap (0 = Off)", &gEdit.fpsCap);
    bool capOk = gEdit.fpsCap == 0 || gEdit.fpsCap >= 10;
    if (!capOk) ImGui::TextColored(kRed, "FPS Cap: 0 (Off), or 10 and up");
    ImGui::Combo("Async Compute (Applies on Restart)", &gEdit.asyncCompute, kAsync, IM_ARRAYSIZE(kAsync));
    ImGui::Checkbox("Antialiasing Off (the game's menu has no off)", &gEdit.aaOff);
    if (ImGui::InputInt("Console History", &gEdit.historyLimit))
        gEdit.historyLimit = std::clamp(gEdit.historyLimit, 1, 10000);
    ImGui::Checkbox("Keep Console History", &gEdit.saveHistory);
    ImGui::Checkbox("Debug Logs (set.log For Bug Reports)", &gEdit.debugLog);  // DebugLog
    if (ImGui::Button("Load")) {
        setEdit(ov::readSettings());
        gSettingsMsg = LV_OK "Loaded";
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!vk || !capOk);
    if (ImGui::Button("Save")) {
        bool ok = ov::saveSettings(gEdit);
        if (ok) {
            ov::setToggleKey(vk);
            ov::setFpsCap(gEdit.fpsCap);
            opts::setAaOff(gEdit.aaOff);
            ov::setHistory(gEdit.historyLimit, gEdit.saveHistory);
            ov::setDebugLog(gEdit.debugLog);  // DebugLog
            saveHistory();
        }
        gSettingsMsg = ok ? LV_OK "Saved" : LV_BAD "Saving settings.ini failed";
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Default")) {
        setEdit(ov::Settings{});
        gSettingsMsg = LV_WARN "Defaults set: Save to keep them";
    }
    drawMsg(gSettingsMsg);
    ImGui::Separator();
    if (ImGui::Button("Dump Game Options")) dumpOptions();
    drawSettingsWindows();
}

// What the runtime found as the game started: the first thing to check, and to send, when something doesn't work.
void drawStatus() {
    ImGui::SeparatorText("Status");
    std::lock_guard lock(gStatusMx);
    for (const StatusRow& r : gStatus) {
        ImGui::TextUnformatted(r.key.c_str());
        ImGui::SameLine(150 * gScale);
        ImGui::TextColored(r.level == lv::ok ? kGreen : r.level == lv::warn ? kYellow : kRed, "%s", r.value.c_str());
    }
}

// The worst Status level: what the toolbar marker shows.
lv::Level statusWorst() {
    std::lock_guard lock(gStatusMx);
    lv::Level w = lv::none;
    for (const StatusRow& r : gStatus) w = std::max(w, r.level);
    return w;
}

void drawSettings() {
    if (!std::exchange(gEditLoaded, true)) setEdit(ov::readSettings());
    ImGui::SetNextWindowPos(ImVec2(980 * gScale, 40 * gScale), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Settings", &gShow[WinSettings], ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ov::gameDir.empty()) ImGui::TextUnformatted("settings.ini is only available in the game.");
        else drawSettingsBody();
        drawStatus();
        ImGui::SeparatorText("Bug Report");
        ImGui::TextUnformatted("Copy a log to the clipboard. Use when reporting bugs.");
        drawReportButtons();
    }
    ImGui::End();
}

// style.dat: the raw ImGuiStyle, kept only for the same ImGui build. The scale it was saved at lets a different
// window height rescale it.
// Limit: binary, so it can't be shared as a theme; a text format if modders want to share themes.
struct StyleFile {
    int version;
    uint32_t size;
    float scale;
    ImGuiStyle style;
};

std::wstring stylePath() { return ov::gameDir + L"shrouded_engine_tweaks\\style.dat"; }

const char* saveStyle() {
    if (ov::gameDir.empty()) return LV_WARN "style.dat is only available in the game";
    StyleFile f{IMGUI_VERSION_NUM, sizeof(ImGuiStyle), gScale, ImGui::GetStyle()};
    HANDLE h = CreateFileW(stylePath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return LV_BAD "Saving style.dat failed";
    DWORD w = 0;
    bool ok = WriteFile(h, &f, sizeof f, &w, nullptr) && w == sizeof f;
    CloseHandle(h);
    return ok ? LV_OK "style saved" : LV_BAD "saving style.dat failed";
}

const char* loadStyle() {
    if (ov::gameDir.empty()) return LV_WARN "style.dat is only available in the game";
    HANDLE h = CreateFileW(stylePath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return LV_WARN "No saved style";
    StyleFile f;
    DWORD got = 0;
    bool ok = ReadFile(h, &f, sizeof f, &got, nullptr) && got == sizeof f;
    CloseHandle(h);
    if (!ok || f.version != IMGUI_VERSION_NUM || f.size != sizeof(ImGuiStyle) || !(f.scale > 0))
        return LV_WARN "style.dat is from another runtime build: Not loaded";
    ImGuiStyle& style = ImGui::GetStyle();
    style = f.style;
    if (f.scale != gScale) {
        style.ScaleAllSizes(gScale / f.scale);
        style.FontScaleMain *= gScale / f.scale;
    }
    return LV_OK "Style loaded";
}

void defaultStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark();
    style.ScaleAllSizes(gScale);
    style.FontScaleMain = gScale;
}

// The style editor plus ImGui's metrics and ID stack tools. Load and Default wait for the next frame.
void drawDebug() {
    ImGui::SetNextWindowPos(ImVec2(980 * gScale, 220 * gScale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560 * gScale, 680 * gScale), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("ImGui Debug", &gShow[WinDebug])) {
        ImGui::Checkbox("Metrics", &gMetrics);
        ImGui::SameLine();
        ImGui::Checkbox("ID Stack Tool", &gIdStack);
        if (ImGui::Button("Save Style")) gStyleMsg = saveStyle();
        ImGui::SameLine();
        if (ImGui::Button("Load Style")) gStyleOp = StyleLoad;
        ImGui::SameLine();
        if (ImGui::Button("Default Style")) gStyleOp = StyleDefault;
        drawMsg(gStyleMsg);
        ImGui::Separator();
        ImGui::ShowStyleEditor();
    }
    ImGui::End();
}

// ---- Resources: browses and edits the game's resources straight in its memory, on the render thread: the game
// keeps them from startup until it exits (game.h, namespace res) ----

struct ResView {
    std::vector<std::pair<refl::Type, size_t>> types;  // the index's types, filled on first use
    int type = -1;                                       // into types
    char typeFind[64] = "";                              // the type picker's search box
    char filter[96] = "";
    int builtType = -1;  // what rows was built for
    std::string builtFilter;
    std::vector<const res::Entry*> rows;  // the type's resources that match the filter, by label
    const res::Entry* sel = nullptr;
    std::vector<res::EnumField> fields;  // the type's enum fields: what Group by offers
    int fieldsType = -1;                 // what fields was built for
    int field = -1, builtField = -1;     // into fields; -1: no groups
    struct Group {
        std::string label;
        std::vector<const res::Entry*> rows;
        bool open = false;
    };
    std::vector<Group> groups;
    std::vector<std::pair<size_t, const res::Entry*>> lines;  // what the grouped list draws: a group, then a row or null
};
ResView gRv;
constexpr ImGuiTreeNodeFlags kLeaf = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_Bullet;

std::string lowerStr(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(tolower(static_cast<unsigned char>(c)));
    return out;
}

// True if s holds every space-separated word of f. Both lowercase.
bool hasWords(const std::string& s, std::string_view f) {
    for (size_t i = 0, j; i < f.size(); i = j + 1) {
        j = std::min(f.find(' ', i), f.size());
        if (j > i && s.find(f.substr(i, j - i)) == std::string::npos) return false;
    }
    return true;
}

// The selected type's resources whose label, English name and GUID hold every word of the filter (case-insensitive),
// by label, the ones without a label last. Rebuilt only when the type or the filter changes.
void buildGroups();

void buildRows() {
    std::string f = lowerStr(gRv.filter);
    if (gRv.builtType == gRv.type && gRv.builtFilter == f && gRv.builtField == gRv.field) return;
    gRv.builtType = gRv.type;
    gRv.builtFilter = f;
    gRv.builtField = gRv.field;
    std::vector<std::pair<std::string, const res::Entry*>> keyed;
    for (const res::Entry* e : res::ofType(gRv.types[size_t(gRv.type)].first.hash())) {
        std::string l = lowerStr(res::label(*e)), g = res::guidStr(e->guid);
        if (hasWords(l + ' ' + lowerStr(res::nameText(*e)) + ' ' + g, f)) keyed.emplace_back(l.empty() ? "~" + g : l, e);
    }
    std::sort(keyed.begin(), keyed.end());
    gRv.rows.clear();
    for (const auto& k : keyed) gRv.rows.push_back(k.second);
    buildGroups();
}

// The lines the grouped list draws: each group's header, then its rows while it's open.
void buildLines() {
    gRv.lines.clear();
    for (size_t g = 0; g < gRv.groups.size(); g++) {
        gRv.lines.emplace_back(g, nullptr);
        if (gRv.groups[g].open)
            for (const res::Entry* e : gRv.groups[g].rows) gRv.lines.emplace_back(g, e);
    }
}

// Groups by the chosen enum field, exactly as the game stores it: one group per value in the enum's order, named by
// the enum (its first name for a value), then values the enum doesn't list, then resources too small to hold the
// field, then extra parts (part > 0), which needn't share the type's layout. Groups start open only while filtering.
void buildGroups() {
    gRv.groups.clear();
    if (gRv.field >= 0) {
        const res::EnumField& f = gRv.fields[size_t(gRv.field)];
        uint64_t mask = f.type.size() >= 8 ? ~0ull : (1ull << (8 * f.type.size())) - 1;
        std::map<uint64_t, size_t> at;
        refl::forEnum(f.type, [&](std::string_view name, int64_t v) {
            if (at.try_emplace(uint64_t(v) & mask, gRv.groups.size()).second) gRv.groups.push_back({std::string(name)});
        });
        std::map<uint64_t, std::vector<const res::Entry*>> unlisted;
        ResView::Group unreadable{"Unreadable"}, parts{"Extra parts"};
        for (const res::Entry* e : gRv.rows) {
            uint64_t v;
            if (e->part) parts.rows.push_back(e);
            else if (!res::enumValue(*e, f, v)) unreadable.rows.push_back(e);
            else if (auto it = at.find(v); it != at.end()) gRv.groups[it->second].rows.push_back(e);
            else unlisted[v].push_back(e);
        }
        for (auto& [v, rows] : unlisted) gRv.groups.push_back({"Value " + std::to_string(v), std::move(rows)});
        gRv.groups.push_back(std::move(unreadable));
        gRv.groups.push_back(std::move(parts));
        std::erase_if(gRv.groups, [](const ResView::Group& g) { return g.rows.empty(); });
        for (auto& g : gRv.groups) g.open = !gRv.builtFilter.empty();
    }
    buildLines();
}

// "Material_T1_Wood_Logs  (Wood Logs)": the label, or the GUID, then the English name if it has one.
std::string rowText(const res::Entry& e) {
    std::string_view l = res::label(e), t = res::nameText(e);
    std::string s = l.empty() ? res::guidStr(e.guid) : std::string(l);
    if (!t.empty()) s += "  (" + std::string(t) + ")";
    return e.part ? s + "  #" + std::to_string(e.part) : s;
}

// Shows e: its type in the list, filtered to its GUID.
void selectRes(const res::Entry* e) {
    for (size_t i = 0; i < gRv.types.size(); i++) {
        if (gRv.types[i].first.hash() == e->type) gRv.type = int(i);
    }
    strncpy_s(gRv.filter, res::guidStr(e->guid).c_str(), _TRUNCATE);
    gRv.sel = e;
}

// The type picker: a search box above the list, which keeps the types whose name holds every word typed. The list is a
// child of its own so the box stays in view while it scrolls.
void drawTypePicker() {
    std::string preview = gRv.type < 0 ? "Pick a Type" : std::string(gRv.types[size_t(gRv.type)].first.qname());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!ImGui::BeginCombo("##type", preview.c_str(), ImGuiComboFlags_HeightLargest)) return;
    if (ImGui::IsWindowAppearing()) {
        gRv.typeFind[0] = 0;
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##typefind", "Search Types", gRv.typeFind, sizeof gRv.typeFind);
    std::string f = lowerStr(gRv.typeFind);
    ImGui::BeginChild("types", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 20));
    for (size_t i = 0; i < gRv.types.size(); i++) {
        std::string name(gRv.types[i].first.qname());
        if (!hasWords(lowerStr(name), f)) continue;
        if (ImGui::Selectable((name + "  " + std::to_string(gRv.types[i].second)).c_str(), int(i) == gRv.type)) {
            gRv.type = int(i);
            ImGui::CloseCurrentPopup();  // a Selectable in a child doesn't close the popup by itself
        }
    }
    ImGui::EndChild();
    ImGui::EndCombo();
}

// What Group by offers for the selected type. A new type starts on its category or family field when it has one.
void pickFields() {
    if (gRv.fieldsType == gRv.type) return;
    gRv.fieldsType = gRv.type;
    gRv.fields = res::enumFields(gRv.types[size_t(gRv.type)].first);
    gRv.field = -1;
    for (size_t i = 0; i < gRv.fields.size() && gRv.field < 0; i++)
        if (gRv.fields[i].path == "category" || gRv.fields[i].path == "family") gRv.field = int(i);
}

// The Group by picker: no groups, or one of the type's enum fields.
void drawGroupPicker() {
    if (gRv.fields.empty()) return;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const char* preview = gRv.field < 0 ? "No groups" : gRv.fields[size_t(gRv.field)].path.c_str();
    if (!ImGui::BeginCombo("##group", preview)) return;
    if (ImGui::Selectable("No groups", gRv.field < 0)) gRv.field = -1;
    for (size_t i = 0; i < gRv.fields.size(); i++)
        if (ImGui::Selectable(gRv.fields[i].path.c_str(), gRv.field == int(i))) gRv.field = int(i);
    ImGui::EndCombo();
}

void drawResRow(const res::Entry* e) {
    ImGui::PushID(e);
    if (ImGui::Selectable(rowText(*e).c_str(), gRv.sel == e)) gRv.sel = e;
    ImGui::PopID();
}

// Grouped: one line per header or open row, so the clipper still draws only what's on screen. A header toggled this
// frame changes the lines after the loop.
void drawGroupLines() {
    bool toggled = false;
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(gRv.lines.size()));
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            auto [g, e] = gRv.lines[size_t(i)];
            ResView::Group& grp = gRv.groups[g];
            if (e) {
                ImGui::Indent();
                drawResRow(e);
                ImGui::Unindent();
                continue;
            }
            ImGui::PushID(int(g));
            ImGui::SetNextItemOpen(grp.open, ImGuiCond_Always);
            std::string head = grp.label + " (" + std::to_string(grp.rows.size()) + ")";
            auto flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (bool open = ImGui::TreeNodeEx(head.c_str(), flags); open != grp.open) {
                grp.open = open;
                toggled = true;
            }
            ImGui::PopID();
        }
    }
    if (toggled) buildLines();
}

void drawResList() {
    if (gRv.types.empty()) gRv.types = res::types();
    drawTypePicker();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "Filter: Name or GUID.", gRv.filter, sizeof gRv.filter);
    if (gRv.type < 0) return;
    pickFields();
    buildRows();
    ImGui::TextDisabled("%zu of %zu", gRv.rows.size(), gRv.types[size_t(gRv.type)].second);
    drawGroupPicker();
    ImGui::BeginChild("rows");
    if (gRv.field >= 0) {
        drawGroupLines();
    } else {
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(gRv.rows.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) drawResRow(gRv.rows[size_t(i)]);
    }
    ImGui::EndChild();
}

// A field row: the name in the first column (a tree node if it opens), then the cursor in the value column. True if
// the node is open; the caller pops it.
bool nameCell(const std::string& name, bool node) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    bool open = ImGui::TreeNodeEx(name.c_str(), node ? ImGuiTreeNodeFlags_SpanFullWidth : kLeaf);
    ImGui::TableSetColumnIndex(1);
    return node && open;
}

ImGuiDataType dataType(refl::Type u) {
    switch (u.prim()) {
    case refl::S8: return ImGuiDataType_S8;
    case refl::S16: return ImGuiDataType_S16;
    case refl::S32: return ImGuiDataType_S32;
    case refl::S64: return ImGuiDataType_S64;
    case refl::F32: return ImGuiDataType_Float;
    case refl::F64: return ImGuiDataType_Double;
    default:  // unsigned, bitmasks, enums that list no values
        return u.size() == 1 ? ImGuiDataType_U8 : u.size() == 2 ? ImGuiDataType_U16
                                                : u.size() == 4 ? ImGuiDataType_U32 : ImGuiDataType_U64;
    }
}

// The game keeps resource pages read-only, so edits go through res::write.
void resWrite(uint8_t* p, const void* v, size_t n) {
    uint32_t prot;
    res::write(p, v, n, prot);
}

// Where a field tree's values come from: a resource, read and edited in place, or the inspector's copy of an
// entity's component, whose edits become requests the next tick checks (insp::edit).
struct Src {
    uint8_t* base;
    uint64_t size;
    const res::Entry* res;  // null for a component copy
    uint32_t entity, comp;  // the component copy's entity and type hash
    uint64_t serial;        // the selection the copy belongs to
};

// A field's ImGui id. A component copy is replaced at every refresh, so its widgets go by offset, not address: an
// address would change under a field being typed into and drop the edit.
void pushFieldId(const Src& s, const uint8_t* p) {
    if (s.res) ImGui::PushID(p);
    else ImGui::PushID(int(p - s.base));
}

bool inside(const Src& s, const uint8_t* p, uint64_t n) {
    if (s.res) return res::inside(*s.res, p, n);
    return p >= s.base && n <= s.size && uint64_t(p - s.base) <= s.size - n;
}

void writeValue(const Src& s, uint8_t* p, refl::Type u, const void* v, const std::string& path) {
    if (s.res) return resWrite(p, v, std::min<uint32_t>(u.size(), 8));
    insp::edit(s.serial, s.comp, uint32_t(p - s.base), u, p, v, path);
}

void editEnum(const Src& s, uint8_t* p, refl::Type u, const std::string& path) {
    uint64_t v = 0;
    uint32_t n = std::min<uint32_t>(u.size(), 8);
    memcpy(&v, p, n);
    if (!ImGui::BeginCombo("##v", refl::text(p, u).c_str())) return;
    refl::forEnum(u, [&](std::string_view name, int64_t e) {
        std::string item = std::string(name) + " (" + std::to_string(e) + ")";
        if (ImGui::Selectable(item.c_str(), uint64_t(e) == v)) writeValue(s, p, u, &e, path);
    });
    ImGui::EndCombo();
}

// Edits a scalar: bools as a checkbox, enums as a list of their values, numbers on Enter. A resource field reads the
// game every frame; a component field shows the inspector's latest copy.
void editScalar(const Src& s, uint8_t* p, refl::Type u, const std::string& path) {
    uint8_t k = u.prim();
    pushFieldId(s, p);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (k == refl::Bool) {
        bool b = *p != 0;
        if (ImGui::Checkbox("##v", &b)) {
            uint8_t v = b ? 1 : 0;
            writeValue(s, p, u, &v, path);
        }
    } else if (k == refl::Enum && u.fieldCount()) {
        editEnum(s, p, u, path);
    } else {
        bool mask = k >= refl::Bitmask8 && k <= refl::Bitmask64;
        const char* fmt = k == refl::F32 || k == refl::F64 ? "%g" : !mask ? nullptr : u.size() == 8 ? "%llX" : "%X";
        ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | (mask ? ImGuiInputTextFlags_CharsHexadecimal : 0);
        uint64_t v = 0;
        memcpy(&v, p, u.size());
        if (ImGui::InputScalar("##v", dataType(u), &v, nullptr, nullptr, fmt, flags)) writeValue(s, p, u, &v, path);
    }
    if (!s.res && ImGui::BeginPopupContextItem("watch")) {  // right-click: into the watchlist
        if (ImGui::MenuItem("Watch this value")) insp::pin(s.entity, s.comp, uint32_t(p - s.base), u, path);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

// An EntityId field of a component: the id, and a button that selects that entity.
void drawEntityLink(const Src& s, const uint8_t* p) {
    uint32_t id = 0;
    memcpy(&id, p, 4);
    if (!id) return ImGui::TextDisabled("None");
    ImGui::Text("%u", id);
    ImGui::SameLine();
    pushFieldId(s, p);
    if (ImGui::SmallButton("Select")) insp::select(id);
    ImGui::PopID();
}

void drawString(const res::Entry& e, const uint8_t* p) {
    res::Blob b = res::blob(p, refl::BlobString);
    const uint8_t* s = p + b.rel;
    if (!b.count) ImGui::TextDisabled("\"\"");
    else if (!res::inside(e, s, b.count)) ImGui::TextDisabled("<Outside the resource>");
    else ImGui::Text("\"%.*s\"", int(b.count), reinterpret_cast<const char*>(s));
}

// A GUID, then its English text for a LocaTag reference, or Go when it names a resource: of the reference's own type
// first, then of any type (a TemplateReference names a TemplateResource).
void drawRef(const Src& s, const uint8_t* p, refl::Type u) {
    static constexpr uint8_t kNone[16] = {};
    if (memcmp(p, kNone, 16) == 0) {
        ImGui::TextDisabled("None");
        return;
    }
    ImGui::TextUnformatted(res::guidStr(p).c_str());
    if (res::isText(u)) {
        std::string_view t = res::text(p);
        ImGui::SameLine();
        if (t.empty()) ImGui::TextDisabled("(No English text)");
        else ImGui::Text("\"%.*s\"", int(t.size()), t.data());
        return;
    }
    const res::Entry* to = u.prim() == refl::ObjectReference && u.inner() ? res::find(p, u.inner().hash()) : nullptr;
    if (!to) to = res::find(p, 0);
    if (!to) return;
    ImGui::SameLine();
    pushFieldId(s, p);
    if (ImGui::SmallButton("Go")) selectRes(to);
    ImGui::PopID();
}

void drawValue(const Src& s, const std::string& name, const std::string& path, uint64_t off, refl::Type t, int depth);

void drawStruct(const Src& s, const std::string& name, const std::string& path, uint64_t off, refl::Type u, int depth) {
    bool open = nameCell(name, true);
    ImGui::TextDisabled("%.*s", int(u.name().size()), u.name().data());
    if (!open) return;
    refl::forFields(u, [&](const refl::Field& f) {
        std::string n(f.name);
        drawValue(s, n, path.empty() ? n : path + "." + n, off + f.offset, f.type, depth + 1);
    });
    ImGui::TreePop();
}

// Limit: shows the first 500 elements; paging if someone needs the rest of a big registry. Blob arrays live in
// resources only; a component copy shows its static arrays.
void drawArray(const Src& s, const std::string& name, const std::string& path, uint64_t off, refl::Type u, int depth) {
    refl::Type el = u.inner();
    uint64_t size = el ? el.size() : 0, start = off, n = size ? u.size() / size : 0;
    if (u.prim() == refl::BlobArray) {
        res::Blob b = res::blob(s.base + off, refl::BlobArray);
        start = off + b.rel;
        n = b.count;
    }
    bool ok = !n || (start <= s.size && inside(s, s.base + start, n * size));
    bool open = nameCell(name, n && ok);
    if (!ok) {
        ImGui::TextDisabled(s.res ? "<Outside the resource>" : "<Outside the component>");
        return;
    }
    ImGui::TextDisabled("%llu x %.*s", n, int(el.name().size()), el.name().data());
    if (!open) return;
    for (uint64_t i = 0; i < n && i < 500; i++) {
        std::string at = "[" + std::to_string(i) + "]";
        drawValue(s, at, path + at, start + i * size, el, depth + 1);
    }
    if (n > 500) {
        nameCell("...", false);
        ImGui::TextDisabled("%llu more", n - 500);
    }
    ImGui::TreePop();
}

bool isEntityId(refl::Type t) { return t.prim() == refl::Struct && t.qname().ends_with("ecs::EntityId"); }

// One field or element, read from the source and bounds-checked against it. Blob values exist in resources only.
void drawValue(const Src& s, const std::string& name, const std::string& path, uint64_t off, refl::Type t, int depth) {
    refl::Type u = refl::unwrap(t.resolved());
    uint8_t k = u.prim();
    uint8_t* p = s.base + off;
    if (depth > 32 || off > s.size || !inside(s, p, u.size())) {
        nameCell(name, false);
        ImGui::TextDisabled(s.res ? "<Outside the resource>" : "<Outside the component>");
        return;
    }
    if (k == refl::Struct) return drawStruct(s, name, path, off, u, depth);
    if (k == refl::StaticArray || (k == refl::BlobArray && s.res)) return drawArray(s, name, path, off, u, depth);
    if ((k == refl::BlobOptional || k == refl::BlobVariant) && s.res) {
        res::Blob b = res::blob(p, k);
        refl::Type v = k == refl::BlobVariant ? refl::byHash(b.hash) : u.inner();
        if (b.count && v) return drawValue(s, name, path, off + b.rel, v, depth + 1);
        nameCell(name, false);
        ImGui::TextDisabled("None");
        return;
    }
    nameCell(name, false);
    if (k == refl::BlobString && s.res) drawString(*s.res, p);
    else if (!s.res && isEntityId(t.resolved())) drawEntityLink(s, p);
    else if ((k == refl::ObjectReference || k == refl::Guid) && u.size() == 16) drawRef(s, p, u);
    else if (k >= refl::Bool && k <= refl::Bitmask64 && u.size() <= 8) editScalar(s, p, u, path);
    else {
        ImGui::TextDisabled("<%.*s>", int(u.qname().size()), u.qname().data());
    }
}

void drawResFields(const res::Entry& e) {
    refl::Type t = refl::byHash(e.type);
    std::string g = res::guidStr(e.guid);
    ImGui::TextUnformatted(std::string(t.qname()).c_str());
    ImGui::TextUnformatted(g.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy GUID")) ImGui::SetClipboardText(g.c_str());
    if (e.part) ImGui::TextDisabled("%u bytes, part %u", e.size, e.part);
    else ImGui::TextDisabled("%u bytes", e.size);
    if (!ImGui::BeginTable("fields", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
        return;
    Src s{res::data(e), e.size, &e, 0, 0, 0};
    refl::forFields(t, [&](const refl::Field& f) { drawValue(s, std::string(f.name), std::string(f.name), f.offset, f.type, 0); });
    ImGui::EndTable();
}

void drawResources() {
    ImGui::SetNextWindowPos(ImVec2(420 * gScale, 120 * gScale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1000 * gScale, 700 * gScale), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Resources", &gShow[WinRes])) {
        if (!res::ready()) {
            ImGui::TextWrapped("The game's resources aren't available yet. The game loads them right after its window "
                               "opens; if this stays, set.log says why.");
        } else {
            ImGui::BeginChild("list", ImVec2(360 * gScale, 0), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
            drawResList();
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("detail", ImVec2(0, 0), ImGuiChildFlags_Borders);
            if (gRv.sel) drawResFields(*gRv.sel);
            else ImGui::TextDisabled("Pick a type, then a resource on the left.");
            ImGui::EndChild();
        }
    }
    ImGui::End();
}

// ---- Entities: the inspector's copies (insp::view), never the game's memory ----

char gEntFilter[64];
uint32_t gEntSearch;

void drawEntFilter() {
    ImGui::SetNextItemWidth(150 * gScale);
    bool go = ImGui::InputTextWithHint("##comp", "Component", gEntFilter, sizeof gEntFilter, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetItemTooltip("Only entities with this component, like Health. Empty for all.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * gScale);
    go |= ImGui::InputScalar("##id", ImGuiDataType_U32, &gEntSearch, nullptr, nullptr, "%u", ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetItemTooltip("An entity id to look up; 0 for the nearest ones.");
    ImGui::SameLine();
    if (ImGui::Button("Show") || go) insp::setFilter(gEntFilter, gEntSearch);
}

void drawEntRow(const insp::View& v, const insp::Row& r) {
    static const refl::Type faction = [] {
        refl::Field f;
        return refl::field(refl::byName("Faction"), "faction", f) ? f.type.resolved() : refl::Type{};
    }();
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    char id[16];
    snprintf(id, sizeof id, "%u", r.id);
    if (ImGui::Selectable(id, v.sel == r.id && !v.selGone, ImGuiSelectableFlags_SpanAllColumns)) insp::select(r.id);
    ImGui::TableNextColumn();
    if (r.dist >= 0) ImGui::Text("%.1f m", r.dist);
    else ImGui::TextDisabled("-");
    ImGui::TableNextColumn();
    if (r.hasHp) ImGui::Text("%d", r.hp);
    else ImGui::TextDisabled("-");
    ImGui::TableNextColumn();
    uint8_t f = uint8_t(r.faction);
    if (r.faction >= 0 && faction) ImGui::TextUnformatted(refl::text(&f, faction).c_str());
    else ImGui::TextDisabled("-");
    ImGui::TableNextColumn();
    ImGui::Text("%u", r.comps);
}

void drawEntList(const insp::View& v) {
    drawEntFilter();
    ImGui::TextDisabled("Showing %zu/%u, Nearest first.", v.rows.size(), v.matched);
    if (!ImGui::BeginTable("ents", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    for (const char* c : {"Id", "Distance", "HP", "Faction", "Comps"}) ImGui::TableSetupColumn(c);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(v.rows.size()));
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) drawEntRow(v, v.rows[size_t(i)]);
    ImGui::EndTable();
}

// A component of the selected entity: its fields once expanded and copied. Expanding tells the inspector to copy it.
void drawEntComp(const insp::View& v, const insp::Comp& c) {
    char unknown[32];
    snprintf(unknown, sizeof unknown, "type %08x", c.hash);
    std::string name = c.type ? std::string(c.type.name()) : unknown;
    ImGui::PushID(int(c.hash));
    bool open = nameCell(name, bool(c.type));
    if (c.type) ImGui::TextDisabled("%u bytes%s", c.type.size(), c.truncated ? ", not refreshed: Over this refresh's budget" : "");
    else ImGui::TextDisabled("Not in the reflection table");
    if (c.type && open != c.open) insp::expand(c.hash, open);
    if (open) {
        if (c.data.empty()) {
            nameCell("...", false);
            ImGui::TextDisabled("Copied at the next refresh");
        } else {
            Src s{const_cast<uint8_t*>(c.data.data()), c.data.size(), nullptr, v.sel, c.hash, v.selSerial};
            refl::forFields(c.type, [&](const refl::Field& f) {
                std::string n(f.name);
                drawValue(s, n, name + "." + n, f.offset, f.type, 1);
            });
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void drawEntDetail(const insp::View& v, float reserve) {
    if (!v.sel) return ImGui::TextDisabled("Pick an entity on the left.");
    if (v.selGone) {
        ImGui::TextColored(kYellow, "Entity %u is gone", v.sel);
        ImGui::TextWrapped("It left the world, or another world started. If something came back under this id, it's "
                           "a new object: Pick it again in the list.");
        return;
    }
    ImGui::Text("Entity %u", v.sel);
    ImGui::TextDisabled("Edits apply at the next game tick, only if the value is still what you see here.");
    ImGui::TextDisabled("Right-click a value to watch it. (?)");
    ImGui::SetItemTooltip("An entity the game removes and re-creates under the same id within one game tick can't be "
                          "told apart from the old one.");
    if (!v.note.empty()) ImGui::TextWrapped("%s", v.note.c_str());
    ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("comps", 2, flags, ImVec2(0, -reserve))) return;
    for (const insp::Comp& c : v.comps) drawEntComp(v, c);
    ImGui::EndTable();
}

void drawWatchlist(const insp::View& v) {
    ImGui::SeparatorText("Watchlist");
    if (!ImGui::BeginTable("pins", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) return;
    for (const insp::Pin& p : v.pins) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p.label.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%u", p.id);
        ImGui::TableNextColumn();
        if (p.gone) ImGui::TextColored(kYellow, "Gone");
        else if (!p.have || !p.leaf) ImGui::TextDisabled("No such component now");
        else ImGui::TextUnformatted(refl::text(p.value, p.leaf).c_str());
        ImGui::TableNextColumn();
        ImGui::PushID(int(p.id));  // by the pin, not its address: the view is replaced at every refresh
        ImGui::PushID(int(p.comp));
        ImGui::PushID(int(p.off));
        if (ImGui::SmallButton("x")) insp::unpin(p.id, p.comp, p.off);
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void drawEntities() {
    ImGui::SetNextWindowPos(ImVec2(440 * gScale, 140 * gScale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1000 * gScale, 700 * gScale), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Entities", &gShow[WinEnt])) {
        std::shared_ptr<const insp::View> v = insp::view();
        if (v->off) {
            ImGui::TextWrapped("The inspector hit an access violation and is off until the game restarts. set.log has "
                               "the line.");
        } else if (!v->inWorld) {
            ImGui::TextWrapped("The inspector reads a world you host. Load one, and the list fills in within a second.");
        } else {
            ImGui::BeginChild("list", ImVec2(400 * gScale, 0), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
            drawEntList(*v);
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("detail", ImVec2(0, 0), ImGuiChildFlags_Borders);
            float line = ImGui::GetFrameHeightWithSpacing();
            drawEntDetail(*v, v->pins.empty() ? 0 : line * float(std::min<size_t>(v->pins.size(), 8) + 2));
            if (!v->pins.empty()) drawWatchlist(*v);
            ImGui::EndChild();
        }
    }
    ImGui::End();
}

char gSysFilter[128];
bool gSysOffOnly;

// The profiler's last second, per system (index in sys::all()): rates from two snapshots of its totals a second apart.
struct SysRate {
    double ms = 0, calls = 0;
    uint8_t kinds = 0;
};
std::vector<SysRate> gRates;
std::vector<std::pair<uint64_t, double>> gPrev;  // calls and ms at the last snapshot
double gPrevAt, gRateMs[2];                      // snapshot time; server and client ms/s

void updateRates() {
    const auto& all = sys::all();
    double now = ImGui::GetTime();
    if (gPrev.size() == all.size() && now - gPrevAt < 1.0) return;
    std::vector<std::pair<uint64_t, double>> cur(all.size());
    std::vector<uint8_t> kinds(all.size());
    for (const sys::Stat& s : sys::stats()) {
        size_t i = size_t(s.sys - all.data());
        cur[i] = {s.calls, s.ms};
        kinds[i] = s.kinds;
    }
    if (gPrev.size() == all.size()) {
        double dt = now - gPrevAt;
        gRates.assign(all.size(), {});
        gRateMs[0] = gRateMs[1] = 0;
        for (size_t i = 0; i < all.size(); i++) {
            auto prev = cur[i].first < gPrev[i].first ? std::pair<uint64_t, double>() : gPrev[i];  // restarted
            gRates[i] = {(cur[i].second - prev.second) / dt, double(cur[i].first - prev.first) / dt, kinds[i]};
            gRateMs[(kinds[i] & 4) && !(kinds[i] & 3)] += gRates[i].ms;
        }
    }
    gPrev = std::move(cur);
    gPrevAt = now;
}

// The 30 systems with the most CPU time since the profiler started, to the console and set.log.
void logTopSystems() {
    double secs = sys::profiledSecs();
    if (secs <= 0) return;
    ov::log("Systems: Top 30 by CPU ms per second, over %.0f s", secs);
    auto st = sys::stats();
    for (size_t i = 0; i < st.size() && i < 30; i++)
        ov::log("  %-48.*s %8.2f ms/s %8.0f calls/s  %s", int(st[i].sys->name.size()), st[i].sys->name.data(),
                st[i].ms / secs, double(st[i].calls) / secs, sys::worldName(st[i].kinds));
}

// The filter, the off count and Turn all on, then the profiler's controls and its totals for the last second.
void drawSysControls() {
    const auto& all = sys::all();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##sysfilter", "Filter: Words in the name.", gSysFilter, sizeof gSysFilter);
    ImGui::Checkbox("Off Only", &gSysOffOnly);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu systems, %zu off", all.size(), sys::offCount());
    if (sys::offCount()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Turn All On"))
            for (const sys::System& s : all) sys::setOn(s, true);
    }
    bool prof = sys::profiling();
    if (ImGui::Checkbox("Profile", &prof)) {
        sys::setProfiling(prof);
        gPrev.clear();
        gRates.clear();
    }
    ImGui::SetItemTooltip("Times every system call. CPU time summed over all threads, so it can pass 1000 ms per second.");
    if (!prof) return;
    ImGui::SameLine();
    if (ImGui::SmallButton("Log Top 30")) logTopSystems();
    updateRates();
    ImGui::Text("FPS %.0f | CPU ms/s: %.0f (server %.0f, client %.0f) | %.0f s measured", ImGui::GetIO().Framerate,
                gRateMs[0] + gRateMs[1], gRateMs[0], gRateMs[1], sys::profiledSecs());
}

// One row: the on/off checkbox with the name, then the last second's numbers while profiling, else the address.
void drawSysRow(const sys::System& s, uintptr_t base, bool prof) {
    bool on = sys::isOn(s);
    ImGui::PushID(&s);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (ImGui::Checkbox(std::string(s.name).c_str(), &on)) sys::setOn(s, on);
    size_t i = size_t(&s - sys::all().data());
    if (prof) {
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", gRates[i].ms);
        ImGui::TableNextColumn();
        ImGui::Text("%.0f", gRates[i].calls);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(gRates[i].kinds ? sys::worldName(gRates[i].kinds) : "");
    }
    ImGui::TableNextColumn();
    ImGui::TextDisabled("exe+%llx", static_cast<unsigned long long>(s.fn - base));
    ImGui::PopID();
}

// The game's systems with a checkbox each: unticked, the game skips that system (Systems.enable). While profiling, the
// list is sorted by the last second's CPU time. The address is the system's update function in the exe, for reverse
// engineering.
void drawSystems() {
    ImGui::SetNextWindowPos(ImVec2(420 * gScale, 120 * gScale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(720 * gScale, 640 * gScale), ImGuiCond_FirstUseEver);
    bool open = ImGui::Begin("Systems", &gShow[WinSys]);
    if (open && !rt::systemsAvailable()) {
        ImGui::TextDisabled("Systems is unavailable in this game build: See set.log.");
    } else if (open) {
        const auto& all = sys::all();
        ImGui::TextWrapped("Unticked, the game stops running a system in every world, until it's ticked again or the "
                           "game closes. Some are needed: If the world breaks, tick them again or reload the world.");
        drawSysControls();
        std::string f = lowerStr(gSysFilter);
        std::vector<const sys::System*> rows;
        for (const sys::System& s : all)
            if (!(gSysOffOnly && sys::isOn(s)) && hasWords(std::string(s.name), f)) rows.push_back(&s);
        bool prof = sys::profiling() && gRates.size() == all.size();
        if (prof)
            std::stable_sort(rows.begin(), rows.end(), [&](const sys::System* a, const sys::System* b) {
                return gRates[size_t(a - all.data())].ms > gRates[size_t(b - all.data())].ms;
            });
        uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (ImGui::BeginTable("systems", prof ? 5 : 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthStretch);
            if (prof) {
                ImGui::TableSetupColumn("ms/s");
                ImGui::TableSetupColumn("calls/s");
                ImGui::TableSetupColumn("World");
            }
            ImGui::TableSetupColumn("Function");
            ImGui::TableHeadersRow();
            ImGuiListClipper clip;
            clip.Begin(static_cast<int>(rows.size()));
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) drawSysRow(*rows[size_t(i)], base, prof);
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

// Reload Mods asks first, so a stray click can't restart every mod. It's off while no mods are loaded: the first
// seconds of a game start, or once the game is closing.
void drawReload() {
    bool loaded = rt::modsLoaded();
    ImGui::BeginDisabled(!loaded);
    if (ImGui::Button("Reload Mods")) ImGui::OpenPopup("reload");
    ImGui::EndDisabled();
    if (!loaded) ImGui::SetItemTooltip("Mods aren't loaded: See set.log");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopup("reload")) return;
    ImGui::TextUnformatted("Reload all mods? Restarts via init.lua");
    if (ImGui::Button("Reload")) {
        rt::requestReload();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// The toolbar at the top center: one button per built-in window, lit while the window is open, then Reload Mods.
void drawToolbar() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y), ImGuiCond_Always, ImVec2(0.5f, 0));
    auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##toolbar", nullptr, flags)) {
        for (int i = 0; i < WinCount; i++) {
            if (i == WinModSet || i == WinCompat) continue;  // opened from Settings
            if (i) ImGui::SameLine();
            bool on = gShow[i];
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(kWinNames[i])) gShow[i] = !on;
            if (on) ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        drawReload();
        if (lv::Level w = statusWorst(); w >= lv::warn) {
            ImGui::SameLine();
            ImGui::TextColored(w == lv::warn ? kYellow : kRed, "(!)");
            ImGui::SetItemTooltip("Something wasn't found, or this game build is untested: See Settings > Status");
        }
    }
    ImGui::End();
}

double hookRate(uintptr_t fn, uint64_t calls) {
    struct Rate { uintptr_t fn; uint64_t calls; double time, rate; };
    static std::vector<Rate> rates;
    double now = ImGui::GetTime();
    auto it = std::find_if(rates.begin(), rates.end(), [&](const Rate& r) { return r.fn == fn; });
    if (it == rates.end()) { rates.push_back({fn, calls, now, 0}); return 0; }
    if (now - it->time >= 1) {
        it->rate = double(calls - it->calls) / (now - it->time);
        it->calls = calls;
        it->time = now;
    }
    return it->rate;
}

void drawHookSubs(const hk::TargetInfo& t) {
    if (!ImGui::BeginTable("subscribers", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX)) return;
    for (const char* col : {"Mod / Name", "State", "Delivered", "Dropped", "Discarded", "High-Water / Capacity", "Lock Waits"})
        ImGui::TableSetupColumn(col);
    ImGui::TableHeadersRow();
    for (const auto& s : t.subs) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(s.key.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(t.faulted ? "Faulted" : s.active ? "Active" : "Inactive");
        ImGui::TableNextColumn(); ImGui::Text("%llu", s.delivered);
        ImGui::TableNextColumn(); ImGui::Text("%llu", s.dropped);
        ImGui::TableNextColumn(); ImGui::Text("%llu", s.discarded);
        ImGui::TableNextColumn(); ImGui::Text("%u / %u", s.highWater, s.capacity);
        ImGui::TableNextColumn(); ImGui::Text("%llu", s.lockWaits);
    }
    ImGui::EndTable();
}

// Rules run in the order listed. A conflict is another mod's rule changing the same value: both apply, in order.
void drawHookRules(const hk::TargetInfo& t) {
    if (t.rules.empty() || !ImGui::BeginTable("rules", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) return;
    for (const char* col : {"Rule", "State", "Hits", "Refused", "Faults", "Also Changed By"}) ImGui::TableSetupColumn(col);
    ImGui::TableHeadersRow();
    for (const auto& r : t.rules) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.key.c_str());
        ImGui::TableNextColumn();
        if (r.faulted) ImGui::TextColored(kRed, "Faulted");
        else ImGui::TextUnformatted(r.active ? "Active" : "Removed");
        ImGui::TableNextColumn(); ImGui::Text("%llu", r.hits);
        ImGui::TableNextColumn(); ImGui::Text("%llu", r.refused);
        ImGui::TableNextColumn(); ImGui::Text("%llu", r.faults);
        ImGui::TableNextColumn();
        if (!r.conflict.empty()) ImGui::TextColored(kYellow, "%s", r.conflict.c_str());
    }
    ImGui::EndTable();
}

void drawHookTarget(const hk::TargetInfo& t) {
    ImGui::PushID(reinterpret_cast<void*>(t.fn));
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    ImGui::Separator();
    uint64_t calls = std::max(t.calls, t.ruleCalls);
    ImGui::Text("exe+%llx (%p)  |  %.0f calls/s", t.fn - base, reinterpret_cast<void*>(t.fn), hookRate(t.fn, calls));
    bool timing = t.timing;
    if (ImGui::Checkbox("Timing", &timing)) hk::setTiming(t.fn, timing);
    ImGui::SameLine();
    ImGui::TextDisabled("%llu Observed, %llu Ruled calls; %llu Metadata lock waits", t.calls, t.ruleCalls, t.lockWaits);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    auto us = [&](uint64_t ticks, uint64_t n) { return double(ticks) * 1e6 / double(freq.QuadPart) / double(n); };
    if (t.timedCalls) ImGui::Text("Timed capture: %.3f us/call (%llu samples)", us(t.captureTicks, t.timedCalls), t.timedCalls);
    if (t.ruleTimed) ImGui::Text("Timed rules: %.3f us/call (%llu samples)", us(t.ruleTicks, t.ruleTimed), t.ruleTimed);
    if (t.faulted) ImGui::TextColored(kRed, "Observation disabled after a fault. Restart the game before retrying it.");
    drawHookRules(t);
    if (!t.subs.empty()) drawHookSubs(t);
    ImGui::PopID();
}

void drawHooks() {
    ImGui::SetNextWindowSize(ImVec2(1000 * gScale, 540 * gScale), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Hooks", &gShow[WinHooks])) {
        auto targets = hk::info();
        ImGui::Text("%zu/%d Retained targets; %.2f/64 MiB queue & delivery budget.",
                    targets.size(), hk::kMaxTargets, double(hk::retainedBytes()) / (1024 * 1024));
        ImGui::TextWrapped("Rules change a call's arguments as it happens, in mod order. Observations reach mods on the "
                           "next tick.");
        ImGui::TextWrapped("Dropped = queue overflow; discarded = removed, reloaded, faulted or invalidated by a world "
                           "transition. Timing adds overhead.");
        if (targets.empty()) ImGui::TextDisabled("No function hooks registered.");
        for (const auto& t : targets) drawHookTarget(t);
        auto notes = hk::diagnostics();
        if (!notes.empty() && ImGui::CollapsingHeader("Recent Refusals and Faults"))
            for (const auto& line : notes) ImGui::TextWrapped("%s", line.c_str());
    }
    ImGui::End();
}

// What works on this game build and right now: the same rows Features() gives mods.
void drawCompat() {
    ImGui::SetNextWindowSize(ImVec2(760 * gScale, 420 * gScale), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Compatibility", &gShow[WinCompat])) {
        ImGui::TextColored(kGreen, "Green = Works.");
        ImGui::TextColored(kYellow, "Yellow = Works on this build, but not right now.");
        ImGui::TextColored(kRed, "Red = Off on this build. Mods read the same with Features(), by ID.");
        if (ImGui::BeginTable("features", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Feature");
            ImGui::TableSetupColumn("Id");
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const rt::Feature& f : rt::features()) {
                bool build = f.id == "game_build";
                const char* state = build ? (f.compatible ? "Tested" : "Untested")
                                          : f.available ? "Available" : f.compatible ? "Not available" : "Off";
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(f.label.c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", f.id.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(f.available ? kGreen : f.compatible || build ? kYellow : kRed, "%s%s%s", state,
                                   f.reason.empty() ? "" : ": ", f.reason.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void drawWindows() {
    if (gAskKey) return drawAskKey();
    if (gCrashOnly) return drawLastSession();
    drawToolbar();
    if (gCrashPrompt) drawLastSession();
    if (gShow[WinConsole]) drawConsole();
    if (gShow[WinBinds]) drawBindings();
    if (gShow[WinModSet]) drawModSettings();
    if (gShow[WinRes]) drawResources();
    if (gShow[WinEnt]) drawEntities();
    if (gShow[WinSys]) drawSystems();
    if (gShow[WinHooks]) drawHooks();
    if (gShow[WinLog]) drawGameLog();
    if (gShow[WinCompat]) drawCompat();
    if (gShow[WinSettings]) drawSettings();
    if (gShow[WinDebug]) drawDebug();
    if (gMetrics) ImGui::ShowMetricsWindow(&gMetrics);
    if (gIdStack) ImGui::ShowIDStackToolWindow(&gIdStack);
}

// Consolas ships with Windows; ImGui's own font is the fallback. Sizes scale with the window height (1080p = 1).
// A saved style (style.dat) replaces the default.
float scaleFor(float height) { return std::clamp(height / 1080.0f, 1.0f, 3.0f); }

void setupStyle(HWND wnd) {
    RECT rc{};
    GetClientRect(wnd, &rc);
    gScale = scaleFor(static_cast<float>(rc.bottom - rc.top));
    ImGuiIO& io = ImGui::GetIO();
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 16.0f)) io.Fonts->AddFontDefault();
    defaultStyle();
    loadStyle();
}

}  // namespace

// overlay_selftest only: every built-in window open, so its open frames draw all of them.
extern "C" __declspec(dllexport) void rtOverlayOpenAll() {
    for (bool& b : gShow) b = true;
    gMetrics = gIdStack = true;
    sys::setProfiling(true);  // the Systems window's profiler view
    gCon.finding = gLog.lines.finding = true;
    auto field = [](const char* key, mset::Type type) {  // a page with every kind of setting, never saved (no edits)
        mset::Field f;
        f.key = f.label = key;
        f.type = type;
        return f;
    };
    mset::Page page{"settings test", "", {field("god", mset::Bool), field("regen", mset::Number),
                    field("count", mset::Int), field("free", mset::Int), field("mode", mset::Choice), field("name", mset::Text)}};
    page.fields[1].lo = 0, page.fields[1].hi = 100, page.fields[1].num = 5;
    page.fields[2].lo = 1, page.fields[2].hi = 10, page.fields[2].num = 3;
    page.fields[3].lo = -9007199254740992.0, page.fields[3].hi = 9007199254740992.0;
    page.fields[4].values = {"off", "slow", "fast"}, page.fields[4].num = 1;
    page.fields[5].text = "hello", page.fields[5].maxLen = 3;  // longer than allowed: drawn cut, not past the buffer
    mset::define(std::move(page));
    std::string row;  // a console row that wraps
    for (int i = 0; i < 60; i++) row += "wrap test ";
    ov::addLine(row.c_str());
    insp::View v{};  // the Entities window: a list, an entity with an expanded and a truncated component, two pins
    v.inWorld = true;
    for (uint32_t i = 1; i <= 300; i++) v.rows.push_back({i, i == 2 ? -1.0f : float(i), {}, 7, i % 2 == 0, int32_t(i), -1});
    v.matched = 300, v.sel = 1, v.selSerial = 1, v.note = "Health.dataStorage[0]: Set to 123";
    v.comps = {{0x1234, {}, true, false, std::vector<uint8_t>(40)}, {0x5678, {}, true, true, {}}, {0x9abc, {}, false, false, {}}};
    v.pins = {{1, 0x1234, 8, {}, "Health.dataStorage[0]", false, true, {}}, {2, 0x1234, 8, {}, "Health.dataStorage[0]", true, false, {}}};
    insp::testView(std::move(v));
}

// overlay_selftest only: shows or ends the first-start key window.
extern "C" __declspec(dllexport) void rtOverlayAskKey(int on) { gAskKey = on != 0; }

// overlay_selftest only: a long mod tick holds the Lua lock while a mod Draw is registered.
extern "C" __declspec(dllexport) void rtTestHoldLua(int on) { rt::testHoldLua(on != 0); }

ov::Settings ov::readSettings() {
    Settings s;
    if (gameDir.empty()) return s;
    std::wstring path = gameDir + L"shrouded_engine_tweaks\\settings.ini";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {  // first start: the defaults, with their comments
        DWORD w;
        WriteFile(f, kSettings, sizeof kSettings - 1, &w, nullptr);
        CloseHandle(f);
    }
    wchar_t key[32], async[16], aa[16];
    GetPrivateProfileStringW(L"runtime", L"overlayKey", L"", key, 32, path.c_str());
    gAskOnStart = !*key;  // asks until a key is picked, even if an earlier start ended before that
    if (gAskOnStart) wcscpy_s(key, L"`");
    GetPrivateProfileStringW(L"runtime", L"asyncCompute", L"default", async, 16, path.c_str());
    GetPrivateProfileStringW(L"runtime", L"antiAliasing", L"default", aa, 16, path.c_str());
    s.aaOff = _wcsicmp(aa, L"off") == 0;
    s.console = GetPrivateProfileIntW(L"runtime", L"console", 0, path.c_str()) != 0;
    int cap = static_cast<int>(GetPrivateProfileIntW(L"runtime", L"fpsCap", 0, path.c_str()));
    s.fpsCap = cap <= 0 ? 0 : std::max(cap, 10);
    s.historyLimit = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"runtime", L"historyLimit", 100, path.c_str())), 1, 10000);
    s.saveHistory = GetPrivateProfileIntW(L"runtime", L"saveHistory", 1, path.c_str()) != 0;
    s.debugLog = GetPrivateProfileIntW(L"runtime", L"debugLog", 1, path.c_str()) != 0;  // DebugLog
    s.keyName = utf8(key);
    s.key = rt::keyFromName(s.keyName.c_str());
    for (int i = 0; i < 3; i++)
        if (_wcsicmp(async, kAsyncNames[i]) == 0) s.asyncCompute = i;
    return s;
}

// WritePrivateProfileString keeps the file's comments. Without a game folder it would write into the Windows folder.
bool ov::saveSettings(const Settings& s) {
    if (gameDir.empty()) return false;
    readSettings();  // recreates the file with its comments if it was deleted
    std::wstring path = gameDir + L"shrouded_engine_tweaks\\settings.ini";
    std::wstring key(s.keyName.begin(), s.keyName.end());  // key names are ASCII (rt::keyFromName)
    return WritePrivateProfileStringW(L"runtime", L"console", s.console ? L"1" : L"0", path.c_str()) &&
           WritePrivateProfileStringW(L"runtime", L"overlayKey", key.c_str(), path.c_str()) &&
           WritePrivateProfileStringW(L"runtime", L"fpsCap", std::to_wstring(s.fpsCap).c_str(), path.c_str()) &&
           WritePrivateProfileStringW(L"runtime", L"asyncCompute", kAsyncNames[std::clamp(s.asyncCompute, 0, 2)],
                                      path.c_str()) &&
           WritePrivateProfileStringW(L"runtime", L"antiAliasing", s.aaOff ? L"off" : L"default", path.c_str()) &&
           WritePrivateProfileStringW(L"runtime", L"historyLimit", std::to_wstring(s.historyLimit).c_str(), path.c_str()) &&
           WritePrivateProfileStringW(L"runtime", L"saveHistory", s.saveHistory ? L"1" : L"0", path.c_str()) &&
           WritePrivateProfileStringW(L"runtime", L"debugLog", s.debugLog ? L"1" : L"0", path.c_str());  // DebugLog
}

void ov::setStatus(const std::string& key, const std::string& value, lv::Level level) {
    std::lock_guard lock(gStatusMx);
    auto r = std::find_if(gStatus.begin(), gStatus.end(), [&](const StatusRow& s) { return s.key == key; });
    if (r == gStatus.end()) gStatus.push_back({key, value, level});
    else *r = {key, value, level};
}

void ov::setHistory(int limit, bool save) {
    gHistLimit = std::clamp(limit, 1, 10000);
    gSaveHist = save;
    trimHistory();
    if (!save && !gameDir.empty()) DeleteFileW(historyPath().c_str());  // typed lines don't stay on disk
}

// Names the overlay key as the game window's layout labels it, once that window is known (not from DllMain). A key
// that types a letter there gets a warning: ` by its code is Ö on a German keyboard, and 0.1.x accepted it.
void nameToggleKey() {
    int vk = gToggleKey;
    gKeyName = keyLabel(vk);
    if (keyChar(vk) > L' ' && IsCharAlphaW(keyChar(vk)))
        ov::log(LV_WARN "Overlay Key: %s types a letter on this keyboard, so typing it opens the overlay: Pick another "
                        "key in Settings", gKeyName.c_str());
}

void ov::attach(HWND wnd, const std::string& iniPath) {
    if (gWnd || !wnd) return;
    gWnd = wnd;
    gIni = iniPath;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = gIni.empty() ? nullptr : gIni.c_str();
    setupStyle(wnd);
    loadHistory();
    ImGui_ImplWin32_Init(wnd);
    // The subclass outlives the Vulkan instance, and the loader unloads layers with their instance: stay loaded.
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&wndProc), &self);
    gOrig = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wndProc)));
    gCrashPrompt = rep::lastSession().crashed;
    gHintEnd = GetTickCount64() + (gCrashPrompt ? 20000 : 5000);  // the crash hint asks for more, so it stays longer
    if (!gAskOnStart) {
        nameToggleKey();
        ov::log(LV_OK "Overlay: Attached. %s opens and closes it.", gKeyName.c_str());
        if (gCrashPrompt) {  // the Last Session window opens by itself, taking input like the first-start key window
            gCrashOnly = gOpen = true;
            rt::setOverlayOpen(true);
        }
        return;
    }
    snapKeys();  // a key held right now isn't the answer
    gAskKey = gOpen = true;
    rt::setOverlayOpen(true);
    ov::log(LV_OK "Overlay: Attached. Pick its key in the window in the middle of the screen.");
}

void ov::resize(HWND wnd, uint32_t height) {
    if (wnd != gWnd || !height) return;
    float s = scaleFor(static_cast<float>(height));
    if (s != gScale) gNewScale = s;
}

void ov::setToggleKey(int vk) {
    gToggleKey = vk;
    if (gWnd) nameToggleKey();
}

bool ov::visible() { return !gDisabled && (gOpen || hintDue() || rt::hasDraw()); }

void ov::disable() {
    gDisabled = true;
    gOpen = false;
    rt::setOverlayOpen(false);
}

// While closed, mod windows still show but take no input: the mouse is parked off-screen so nothing is hovered.
// A style load or reset waits for this point, outside any frame. While a mod's tick holds the Lua lock, no frame is
// built: the caller renders the previous frame's draw data again, so the game's present never waits for Lua. Mods
// draw first, under that lock, so the built-in windows never run under it.
void ov::buildFrame() {
    bool mods = rt::hasDraw() && !gAskKey;  // mods' windows would cover the key window
    if (mods && !rt::tryLockDraw()) return;
    if (float s = gNewScale.exchange(0.0f); s > 0 && s != gScale) {  // the game changed resolution
        gScale = s;
        defaultStyle();
        loadStyle();  // a saved style, rescaled from the scale it was saved at
        ov::log("Overlay: UI scale %.2f for the new window size", s);
    }
    if (gStyleOp == StyleLoad) gStyleMsg = loadStyle();
    if (gStyleOp == StyleDefault) {
        defaultStyle();
        gStyleMsg = LV_WARN "Default style: Save Style to keep it";
    }
    gStyleOp = StyleNone;
    ImGui_ImplWin32_NewFrame();
    if (!gOpen) ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    ImGui::NewFrame();
    if (mods) rt::drawLocked();
    insp::setOpen(gOpen && !gAskKey && !gCrashOnly && gShow[WinEnt]);  // the inspector refreshes only while it's seen
    if (gOpen) drawWindows();
    else if (hintDue()) drawHint();
    ImGui::Render();
}

// Multi-line output (tracebacks) is split so every entry is one row, which the list clipper needs.
void ov::addLine(const char* line) {
    std::lock_guard lock(gLinesMx);
    lv::Level l = lv::of(line);
    for (const char* p = line;;) {
        const char* end = strchr(p, '\n');
        std::string row(p, end ? end : p + strlen(p));
        if (p != line && l) row.insert(row.begin(), char(l));  // every row of a traceback keeps the line's color
        if (!row.empty() && row.back() == '\r') row.pop_back();
        gCon.rows.push_back(std::move(row));
        if (!end) break;
        p = end + 1;
    }
    while (gCon.rows.size() > kMaxLines) gCon.rows.pop_front();
    gCon.rev++;
}
