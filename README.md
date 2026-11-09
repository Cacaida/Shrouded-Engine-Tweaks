# Shrouded Engine Tweaks

[![License](https://img.shields.io/badge/License-MIT-blue)](LICENSE)
![Version](https://img.shields.io/badge/Version-0.2.0-blue)
![Windows](https://img.shields.io/badge/Windows-10%20%7C%2011-blue)
[![Nexus Mods](https://img.shields.io/badge/Nexus%20Mods-Download-orange)](https://www.nexusmods.com/enshrouded/mods/303)

A Lua scripting runtime and mod framework for **Enshrouded**. It loads with the game, adds an in-game overlay with a
console and tool windows, and gives Lua mods live access to the game's entities, data, systems, options and admin
commands, all by the names the game itself uses.

> Unofficial, not affiliated with Keen Games. Works in **worlds you host** (single-player, or the server your own game
> runs). Tested on game builds `1792148057`, `1792507660` and `1793791052`.

## What it can do

- **Lua mods:** one folder per mod. They load at the main menu, and reload without restarting the game.
- **Entities:** read and change anything in a world by name: your health, an enemy's faction, any of the game's ~690
  kinds of entity data (`Ecs.get(player, "Health")`).
- **Game data:** all ~92,000 of the game's items, recipes, perks, enemies and loot tables, read and changed live,
  found by their English names too.
- **Systems:** switch off any of the game's 861 pieces of game logic by name (stamina drain, fall damage, durability
  loss...), and see what each costs.
- **Game options:** graphics, camera, HUD and gameplay settings changed live, field of view beyond the menu's range
  included.
- **Admin commands:** time of day, teleport, spawn, give items, character and flame level, world settings.
- **Events:** react to the game's events (deaths, XP, pickups), and send your own.
- **Hooks (advanced):** watch the game's own functions, or change what they do as they run: the damage of a hit, the
  XP of a kill, or skip the call.
- **Overlay:** console, key bindings, a resource browser and editor, an entity inspector with live edits and a
  watchlist, a systems window, the game log, and settings with an FPS cap and antialiasing off.
- **Mod settings:** `Settings.define` declares typed settings. They're saved per mod and get a page in the overlay's
  Mod Settings window (in Settings).
- **Clean reloads:** what a mod changed in resources, options and systems goes back when it stops or Reload Mods
  runs.
- **Compatibility:** `Features()` and the Compatibility window (in Settings) say what works on this game build, and
  what works right now.
- **Mod UI:** mods draw their own windows with ImGui.
- **Safe by design:** each mod has its own globals and a read-only shared API, file access only in its own folder, no
  access outside the game, and errors that never reach the game.

## Status

**Works,** tested in game: everything above.

**Untested:** the raw `Admin.send` commands for repair all, refill fuel and apply inventory.

**On hold:**
- giving items to every player with a friend joined (`createForAllPlayers`).

**Not done yet:**
- joined players on your own server: commands go to one player, which may not be you;
- game option changes aren't saved, and the game's menu doesn't show them;
- no new, copied or deleted resources;
- English names only;
- hooks change a game function's data or result, or skip it, only on the built-in targets (combat XP, hits); with
  their own patterns, mods can watch functions and change number arguments.

## Requirements

- **To play:** Windows 10 or 11 (64-bit), Enshrouded, a Vulkan GPU. Start Steam and the game normally: run as
  administrator, the in-game overlay can't load.
- **Linux / Steam Deck:** not supported yet, the in-game overlay can't load under Proton. Untested: with
  `WINEDLLOVERRIDES="powrprof=n,b" %command%` in the game's Steam **Launch Options**, mods and the separate console
  window may work without the overlay.
- **To build:** Visual Studio 2026 Build Tools (C++) and CMake 3.20 or newer. See [building.md](docs/building.md).

## Quick Start

1. Download the zip from [Nexus Mods](https://www.nexusmods.com/enshrouded/mods/303) or
   [Releases](https://github.com/Cacaida/Shrouded-Engine-Tweaks/releases/latest).
2. Copy `powrprof.dll` and the `shrouded_engine_tweaks` folder next to `enshrouded.exe`, or install it via Vortex.
3. Start the game & press the key you want for the overlay.
4. Put mods in `shrouded_engine_tweaks\mods\<name>\init.lua`.

Another mod that also installs a `powrprof.dll` goes into `shrouded_engine_tweaks\plugins\` instead, and loads with
the game ([how](docs/installation.md#other-dll-mods)).

## Reporting problems

Open the overlay, **Settings**, **Bug Report**, **Copy log to clipboard**, and paste the log into a
[GitHub bug report](https://github.com/Cacaida/Shrouded-Engine-Tweaks/issues/new/choose) or the
[Nexus Bugs tab](https://www.nexusmods.com/enshrouded/mods/303?tab=bugs). After a crash, a window with the last
session's log opens by itself at the next start. More in [Reporting a bug](docs/reporting.md).

## Guides

| Guide | For |
|---|---|
| [Installation](docs/installation.md) | Installing, settings, updating, troubleshooting |
| [Usage](docs/usage.md) | The overlay and its windows |
| [Reporting a bug](docs/reporting.md) | Getting the log, where to report, what to write |
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
| 6 | Mod platform: per-mod globals and `require`, bindable hotkeys, mod files and `json` |
| 7 | Entity loop and lifecycle events |
| 8 | Resources: all 91,812 read and edited live, the Resources window, spawning |
| 9 | Game options live, antialiasing off, an options dump with the game's defaults |
| 10 | Types as data, values Lua owns, console history, Reload Mods |
| 11 | Mods load at the main menu and stay loaded across worlds |
| 12 | Systems: all 861 switched off by name, with a profiler |
| 13 | Observe-only native hooks: bounded argument capture, Lua delivery and diagnostics |
| 14 | 0.1.1: a plugins folder for other DLL mods, hooks that coexist with theirs, `GetGameBuild()` and `mod.json`, and fixes from a stress test (the overlay never stalls the game, require cycles, the overlay key prompt) |
| 15 | 0.2.0: native rules (`Hooks.rule` changes a game function's arguments, fields or result, or skips the call, on verified targets like `combat_xp` and `hit`), `Features()` and the Compatibility window, a mod's changes undone when it stops, typed mod settings, the Entities window (inspect, edit and watch entities live), and bug reports in one click (copy the log, a Last Session window after a crash, a message box when the overlay can't load) |

Current release: 0.2.0.

## Third-party code

- [Lua](https://www.lua.org) 5.4.9 (MIT)
- [Dear ImGui](https://github.com/ocornut/imgui) 1.92.9b (MIT)
- [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.4 (BSD-2-Clause)
- [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) (Apache-2.0 OR MIT)

Versions and sources are in [`third_party/README.md`](third_party/README.md).

## License

MIT: see [LICENSE](LICENSE). Mods are yours to license however you like.
