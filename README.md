# Shrouded Engine Tweaks

[![License](https://img.shields.io/badge/License-MIT-blue)](LICENSE)
![Version](https://img.shields.io/badge/Version-0.1.0-blue)
![Windows](https://img.shields.io/badge/Windows-10%20%7C%2011-blue)
[![Nexus Mods](https://img.shields.io/badge/Nexus%20Mods-Download-orange)](https://www.nexusmods.com/enshrouded/mods/303)

A Lua scripting runtime and mod framework for **Enshrouded**. It loads with the game, adds an in-game overlay with a
console and tool windows, and gives Lua mods live access to the game's entities, data, systems, options and admin
commands, all by the names the game itself uses.

> Unofficial, not affiliated with Keen Games. Works in **worlds you host** (single-player, or the server your own game
> runs). Tested on game builds `1792148057`, `1792507660` and `1793791052`.

## What it can do

- **Lua mods:** one folder per mod, loaded at the main menu and kept across worlds. Reload them without restarting
  the game.
- **Entities:** read and write any of the game's ~690 component types by name (`Ecs.get(player, "Health")`), and loop
  over every loaded entity that has a set of them.
- **Game data:** all 91,812 resources (items, recipes, perks, enemies, loot tables), read and edited live, found by
  their English names too.
- **Systems:** switch any of the game's 861 systems off by name (stamina drain, fall damage, durability loss...), and
  profile their CPU time.
- **Game options:** graphics, camera, HUD and gameplay options changed live, field of view included, beyond the
  menu's range.
- **Admin commands:** time of day, teleport, spawn, give items, character and flame level, world settings.
- **Events:** react to the game's events (deaths, XP), and post your own.
- **Overlay:** console, key bindings, a resource browser and editor, a systems window, the game log, settings with a
  frame rate cap and antialiasing off.
- **Mod UI:** mods draw their own windows with ImGui.
- **Safe by design:** each mod has its own globals and a read-only shared API, file access only in its own folder, no
  access outside the game, and errors that never reach the game.

## Status

**Works,** tested in game: everything above.

**Untested:**
- the raw `Admin.send` commands for repair all, refill fuel and apply inventory;
- a PC other than the one it was built on.

**On hold:**
- giving items to every player with a friend joined (`createForAllPlayers`).

**Not done yet:**
- joined players on your own server: commands go to one player, which may not be you;
- game option changes aren't saved, and the game's menu doesn't show them;
- no new, cloned or deleted resources;
- English names only;
- no inline Lua overrides or native return-value capture.

## Requirements

- **To play:** Windows 10 or 11 (64-bit), Enshrouded on Steam, a Vulkan GPU.
- **To build:** Visual Studio 2026 Build Tools (C++) and CMake 3.20 or newer. See [building.md](docs/building.md).

## Quick start

1. Download the zip from [Nexus Mods](https://www.nexusmods.com/enshrouded/mods/303) or
   [Releases](https://github.com/Cacaida/Shrouded-Engine-Tweaks/releases/latest).
2. Copy `powrprof.dll` and the `shrouded_engine_tweaks` folder next to `enshrouded.exe`, or install it with Vortex.
3. Start the game and press the key you want for the overlay.
4. Put mods in `shrouded_engine_tweaks\mods\<name>\init.lua`.

## Reporting problems

Open a [bug report](https://github.com/Cacaida/Shrouded-Engine-Tweaks/issues/new/choose) and attach
`shrouded_engine_tweaks\set.log` (`set.prev.log` if the game crashed). In the overlay, **Settings**, **Status** shows
what was found at startup: yellow and red rows are worth including.

## Guides

| Guide | For |
|---|---|
| [Installation](docs/installation.md) | Installing, settings, updating, troubleshooting |
| [Usage](docs/usage.md) | The overlay and its windows |
| [Modding guide](docs/modding-guide.md) | Writing your first mod, and how mods work |
| [API reference](docs/api-reference.md) | Every Lua function |
| [Architecture](docs/architecture.md) | How it works inside the game |
| [Building](docs/building.md) | Building from source and the self-tests |
| [Reflection format](docs/reflection-format.md) | The layout of the game's type table |

## Milestones

| # | Milestone |
|---|---|
| 1 | Loads as a proxy DLL; reads the game's 12,646 reflected types; components read and written by name |
| 2 | Lua runtime on the server tick, with a sandbox and an offline self-test |
| 3 | Events: `Observe` after each tick, `Events.post` through the game's queue |
| 4 | Admin commands through the game's own admin system, and world settings |
| 5 | In-game overlay: a Vulkan layer with a console and mod windows |
| 6 | Overlay tools: toolbar, game log, settings, style editor |
| 7 | Mod platform: per-mod globals and `require`, bindable hotkeys, mod files and `json` |
| 8 | Entity loop and lifecycle events |
| 9 | Resources: all 91,812 read and edited live, the Resources window, spawning |
| 10 | Giving items, English names, a leave-world event |
| 11 | First-start key window, frame rate cap, async compute, find in the console and log |
| 12 | Game options live, antialiasing off, an options dump with the game's defaults |
| 13 | Types as data, values Lua owns, console history, Reload Mods |
| 14 | Mods load at the main menu and stay loaded across worlds |
| 15 | Release groundwork: version, game build check, Status, timestamped logs |
| 16 | Field of view applies live |
| 17 | Systems: all 861 switched off by name, with a profiler |
| 18 | Review passes: crash safety inside the Lua lock, honest Status rows |
| 19 | Observe-only native hooks: bounded argument capture, Lua delivery and diagnostics |

Released as 0.1.0. Next: `GetGameBuild()` for mods, so Hooks patterns can be picked per game build.

## Third-party code

- [Lua](https://www.lua.org) 5.4.9 (MIT)
- [Dear ImGui](https://github.com/ocornut/imgui) 1.92.9b (MIT)
- [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.4 (BSD-2-Clause)
- [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) (Apache-2.0 OR MIT)

Versions and sources are in [`third_party/README.md`](third_party/README.md).

## License

MIT: see [LICENSE](LICENSE). Mods are yours to license however you like.
