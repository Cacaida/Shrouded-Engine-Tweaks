// The write log: what mods changed in the game's resources, options and systems, so it can be undone when they stop
// running (docs/architecture.md, Write log). One record per value, whichever mods wrote it: the game's original bytes,
// the latest logged bytes and the mods that wrote it. When the last of them leaves, the original comes back, unless
// something outside the log (the console, the overlay, the game) changed the value since. Lua's threads only, under
// the Lua lock.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace wlog {

enum Kind : uint8_t { Res, Opt, Sys };
using Sink = void (*)(const char* line);

// Before mod writes n bytes at p, inside the value at base: a resource's data, an options struct, or a sys::System
// (p == base, n == 1: on or off). Saves the game's original of every byte not logged yet and adds mod as a writer.
// False if that needs a new record and the budget is full: the write must not happen.
bool reserve(Kind k, const void* base, const uint8_t* p, uint32_t n, const std::string& mod);
void wrote(const uint8_t* p, const void* src, uint32_t n);  // the write reserve allowed happened: its bytes are the latest
void leave(const std::string& mod, Sink sink);              // mod stopped while the others go on (it failed while loading)
void leaveAll(Sink sink);                                   // every mod stopped: reload, or Lua off after an access violation
size_t records();
void setBudget(size_t bytes);  // runtime_selftest only

}  // namespace wlog
