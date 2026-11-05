# Building

## Requirements

- Windows 10 or 11, 64-bit.
- Visual Studio 2026 Build Tools, with the C++ workload and its x64 MASM assembler.
- CMake 3.20 or newer.
- A GPU with Vulkan, for `overlay_selftest` only.
- Python 3 with `pefile`, for `tools/reflect_dump.py` only.

Everything else (Lua, Dear ImGui, MinHook, Vulkan-Headers) is in [`third_party/`](../third_party).

## Build

From the repo root:

```
cmake -S runtime -B work/build/runtime -G "Visual Studio 18 2026" -A x64
cmake --build work/build/runtime --config Release
```

The DLL is `work/build/runtime/Release/powrprof.dll`. `work/` is ignored by git.

## Tests

After a game update, check every code lookup the runtime makes at startup against a copy of the new exe, before
starting the game. Each line says what was found; the exit code is 0 only if everything was:

```
work\build\runtime\Release\runtime_selftest.exe --patterns <copy of the new enshrouded.exe>
```

Two self-tests run without the game:

```
work\build\runtime\Release\runtime_selftest.exe <copy of enshrouded.exe>
work\build\runtime\Release\overlay_selftest.exe
```

- **`runtime_selftest`** checks the reflection reader, the code patterns and the system scan against a copy of the
  exe, then the ECS, event, resource, options and admin code against synthetic game memory, and the whole Lua API,
  sandbox included. Point it at a copy of the exe, never the one in the game folder. Its pattern addresses are pinned
  to build `1792148057`.
  Native observation tests install real MinHook detours on controlled functions inside the test process. They cover
  scalar/register/stack arguments, hidden return pointers, unchanged returns, nonvolatile registers, unwind metadata,
  copied payloads, faults, queues, reload generations and Lua lifecycle behavior. A child process checks retained
  target/registration limits and the 64 MiB budget. It can also be run directly with `--hooks-limits`.
  The printed microbenchmark compares unhooked calls, inactive detours, scalar and snapshot capture, and multiple
  subscribers. It excludes queue drains and Lua execution, and does not establish in-game performance.
- **`overlay_selftest`** opens a small window and drives a real Vulkan swapchain through the overlay layer, closed and
  open, and checks the export forwarding.

Offline tests alone don't cover the hook layer in the game. After changing it, repeat the in-game check: observe a
verified game target from a test mod, compare three repeatable frametime runs each for the known-good DLL, the new DLL
without observers and with observers, and test removal, reload during activity, overflow, world exit/re-entry and
shutdown. Keep a known-good DLL for rollback and replace the installed DLL only while the game is closed.

## Tools

- **`tools/reflect_dump.py`** dumps every type of the game's reflection table (fields, offsets, enums) from a copy of
  `enshrouded.exe` to JSON. The format is in [reflection-format.md](reflection-format.md).

  ```
  python -I tools/reflect_dump.py <copy of enshrouded.exe> types.json
  ```

## Layout

| Path | What |
|---|---|
| `runtime/main.cpp` | Loading, logging, the hooks |
| `runtime/game.cpp`, `game.h` | Game memory: reflection, patterns, ECS, events, resources, options, systems |
| `runtime/hooks.cpp`, `hooks.h`, `hooks_x64.asm` | Bounded native entry observation, queues and Windows x64 capture stub |
| `runtime/lua_api.cpp` | The Lua API, mod loading and the sandbox |
| `runtime/lua_imgui.cpp`, `lua_json.cpp` | ImGui and json for mods |
| `runtime/overlay_vk.cpp` | The overlay's Vulkan layer |
| `runtime/overlay_ui.cpp` | The overlay's windows and input |
| `runtime/exports.h` | The forwarded `powrprof.dll` exports |
| `runtime/selftest.cpp`, `overlay_test.cpp` | The two self-tests |
| `runtime/hooks_test.cpp`, `hooks_test_x64.asm` | Native observation, concurrency, limits and ABI test targets |
