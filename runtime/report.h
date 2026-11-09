// Bug report helpers: whether the last session crashed, what the game's own crash report says about it, and the logs
// as text for the clipboard. docs/reporting.md has the user side.
#pragma once
#include <cstdint>
#include <string>

namespace rep {

struct LastSession {
    bool crashed = false;
    std::string report;  // a line from the game's crash report, empty if it wrote none
};

// The session in this log text started SET fully but never logged the game's shutdown. False when it can't tell.
bool crashed(const std::string& log);
// One line from the text of a game crash report (enshrouded.info): the game's reason, and how many callstack frames
// are inside the module whose file path ends with dll (forward slashes, any case).
std::string crashSummary(const std::string& info, const std::string& dll);
std::string redact(std::string text, const std::string& home);  // home (a user folder) becomes %USERPROFILE%

// Reads the rotated set.prev.log and the game's newest crash report, once at startup; lastSession() keeps the result.
const LastSession& checkLast(const std::wstring& gameDir);
const LastSession& lastSession();
// set.log, or set.prev.log with a crash note on top, with this user's folder redacted.
std::string logText(const std::wstring& gameDir, bool prev);
bool copyText(const std::string& utf8);  // to the Windows clipboard, from any thread

}  // namespace rep
