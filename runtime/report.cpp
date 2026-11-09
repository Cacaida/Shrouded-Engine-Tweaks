// The bug report helpers (report.h). Files are read sharing write access: set.log stays open for writing all session.
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <string_view>
#include <vector>

#include "report.h"

namespace {

constexpr size_t npos = std::string::npos;
rep::LastSession gLast;     // written once by checkLast (init thread), then read by the overlay's threads
std::atomic<bool> gReady;   // checkLast is done

std::string utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string readFile(const std::wstring& path, FILETIME* written = nullptr) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    if (written) GetFileTime(f, nullptr, nullptr, written);
    LARGE_INTEGER size{};
    std::string s;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64ll << 20)) {
        s.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(f, s.data(), static_cast<DWORD>(s.size()), &got, nullptr)) got = 0;
        s.resize(got);
    }
    CloseHandle(f);
    return s;
}

// The string value of "key" in the game's crash report: printable ASCII only, at most 200 characters. The first
// match is the top-level one, which comes before the arrays.
std::string jsonStr(std::string_view j, std::string_view key) {
    std::string pat = "\"" + std::string(key) + "\": \"";
    size_t p = j.find(pat);
    if (p == npos) return {};
    std::string out;
    for (p += pat.size(); p < j.size() && j[p] != '"' && out.size() < 200; p++) {
        if (j[p] == '\\' && p + 1 < j.size()) p++;
        if (j[p] >= 0x20 && j[p] < 0x7f) out += j[p];
    }
    return out;
}

// The number after "key": between from and to, 0 if none.
uint64_t jsonNum(std::string_view j, std::string_view key, size_t from, size_t to) {
    std::string pat = "\"" + std::string(key) + "\": ";
    size_t p = j.find(pat, from);
    if (p == npos || p >= to) return 0;
    return strtoull(j.data() + p + pat.size(), nullptr, 10);
}

// Base and size of the module entry whose "file" ends with dll.
bool moduleRange(std::string_view j, const std::string& dll, uint64_t& base, uint64_t& size) {
    size_t m = j.find("\"modules\": [");
    std::string want = lower(dll);
    for (size_t p = j.find("\"file\": \"", m); m != npos && p != npos; p = j.find("\"file\": \"", p + 1)) {
        size_t s = p + 9, e = j.find('"', s);
        if (e == npos) break;
        std::string file = lower(std::string(j.substr(s, e - s)));
        if (file.size() < want.size() || file.compare(file.size() - want.size(), npos, want) != 0) continue;
        size_t from = j.rfind('{', p), to = j.find('}', p);
        base = jsonNum(j, "addr", from, to);
        size = jsonNum(j, "size", from, to);
        return base && size;
    }
    return false;
}

std::vector<uint64_t> callstack(std::string_view j) {
    std::vector<uint64_t> out;
    size_t from = j.find("\"callstack\": ["), to = j.find(']', from);
    for (size_t p = j.find("\"addr\": ", from); from != npos && p != npos && p < to; p = j.find("\"addr\": ", p + 1))
        out.push_back(strtoull(j.data() + p + 8, nullptr, 10));
    return out;
}

void replaceCI(std::string& s, const std::string& what, const std::string& with) {
    std::string ls = lower(s), lw = lower(what), out;
    size_t from = 0;
    for (size_t p; (p = ls.find(lw, from)) != npos; from = p + what.size()) out.append(s, from, p - from) += with;
    s = out.append(s, from);
}

uint64_t ticks(FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

// The newest crash_dumps\*\enshrouded.info written at or after since (60 s slack: the game writes it while SET may
// still be logging), summarized against this DLL.
std::string newestReport(const std::wstring& gameDir, FILETIME since) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((gameDir + L"crash_dumps\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring best;
    uint64_t bestT = 0, slack = 60ull * 10'000'000;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        std::wstring p = gameDir + L"crash_dumps\\" + fd.cFileName + L"\\enshrouded.info";
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) continue;
        uint64_t t = ticks(a.ftLastWriteTime);
        if (t + slack >= ticks(since) && t > bestT) bestT = t, best = p;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (best.empty()) return {};
    HMODULE self = nullptr;
    wchar_t dll[MAX_PATH] = L"";
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&newestReport), &self);
    GetModuleFileNameW(self, dll, MAX_PATH);
    std::string path = utf8(dll);
    std::replace(path.begin(), path.end(), '\\', '/');
    return rep::crashSummary(readFile(best), path);
}

}  // namespace

bool rep::crashed(const std::string& log) {
    // Both spellings: the first start after an update reads a log the previous version wrote.
    auto has = [&](const char* a, const char* b) { return log.find(a) != npos || log.find(b) != npos; };
    return log.find("] Ready: ") != npos && !has("] Game closing", "] Game Closing") &&
           !has("Game log: enshrouded.log not readable", "Game Log: enshrouded.log not readable");
}

std::string rep::crashSummary(const std::string& info, const std::string& dll) {
    std::string why = jsonStr(info, "info"), ex = jsonStr(info, "exception"), src = jsonStr(info, "source");
    if (!ex.empty()) why = why.empty() ? ex : why + ", " + ex;
    std::string s = "Game Crash Report: " + (why.empty() ? "No reason given" : why) +
                    (src.empty() ? "" : " (source " + src + ")") + ".";
    uint64_t base = 0, size = 0;
    if (!moduleRange(info, dll, base, size)) return s + " Shrouded Engine Tweaks isn't in its module list.";
    int in = 0, n = 0;
    for (uint64_t a : callstack(info)) n++, in += a >= base && a < base + size;
    char buf[96];
    snprintf(buf, sizeof buf, " Shrouded Engine Tweaks in the crash callstack: %s (%d of %d frames).", in ? "Yes" : "No",
             in, n);
    return s + buf;
}

std::string rep::redact(std::string text, const std::string& home) {
    if (home.size() < 4) return text;  // a drive root would redact every path
    std::string alt = home;
    std::replace(alt.begin(), alt.end(), '\\', '/');
    replaceCI(text, home, "%USERPROFILE%");
    replaceCI(text, alt, "%USERPROFILE%");
    return text;
}

const rep::LastSession& rep::checkLast(const std::wstring& gameDir) {
    FILETIME t{};
    std::string log = readFile(gameDir + L"shrouded_engine_tweaks\\set.prev.log", &t);
    gLast = {};
    gLast.crashed = crashed(log);
    if (gLast.crashed) gLast.report = newestReport(gameDir, t);
    gReady = true;
    return gLast;
}

const rep::LastSession& rep::lastSession() {
    static const LastSession none;
    return gReady ? gLast : none;
}

std::string rep::logText(const std::wstring& gameDir, bool prev) {
    std::string text = readFile(gameDir + L"shrouded_engine_tweaks\\" + (prev ? L"set.prev.log" : L"set.log"));
    const LastSession& last = lastSession();
    if (prev && last.crashed && !text.empty())
        text = "The last session didn't close normally (a crash, unexpectedly, or from the outside). " +
               (last.report.empty() ? std::string("No game crash report was written.") : last.report) + "\r\n\r\n" +
               text;
    wchar_t home[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    return n && n < MAX_PATH ? redact(text, utf8(home)) : text;
}

bool rep::copyText(const std::string& s) {
    if (s.empty()) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(n) + 1) * sizeof(wchar_t));
    if (!g) return false;
    auto* w = static_cast<wchar_t*>(GlobalLock(g));
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w, n);
    w[n] = 0;
    GlobalUnlock(g);
    if (!OpenClipboard(nullptr)) {
        GlobalFree(g);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_UNICODETEXT, g) != nullptr;  // the clipboard owns g from here
    CloseClipboard();
    if (!ok) GlobalFree(g);
    return ok;
}
