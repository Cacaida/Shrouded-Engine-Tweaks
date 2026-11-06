# Installation

## Requirements

- Windows 10 or 11, 64-bit.
- Enshrouded on Steam, on a tested build (see the README). Other builds usually work: each feature checks itself and
  turns off if it can't find what it needs.
- A graphics card with Vulkan, which the game needs anyway.

## Install

1. Download the zip from [Nexus Mods](https://www.nexusmods.com/enshrouded/mods/303) or
   [Releases](https://github.com/Cacaida/Shrouded-Engine-Tweaks/releases/latest).
2. Copy `powrprof.dll` and the `shrouded_engine_tweaks` folder into the game folder, next to `enshrouded.exe`.
   On Steam: right-click Enshrouded, **Manage**, **Browse local files**. Vortex installs it there too.
3. Start the game. A small window asks for the key that opens the overlay. Press it (see [First start](#first-start)).
4. Put mods in `shrouded_engine_tweaks\mods\`, one folder per mod.

That's it. Nothing outside the game folder is changed, and the game's own files stay untouched.

## First start

The first start creates `shrouded_engine_tweaks\` in the game folder, and the overlay opens with a small window in
the middle: press the key that should open and close it.

- The game gets no input until you pick one.
- Keys that type text or close menus are refused, and so is F10. That leaves `` ` ``, F1-F24 except F10, Insert,
  Delete, Home, End, PageUp, PageDown and Pause.
- The key is saved, and the overlay stays open, so pressing it again closes the overlay.

You can change the key later in the overlay's **Settings** window.

## Files

```
<game>\powrprof.dll                              Shrouded Engine Tweaks itself
<game>\shrouded_engine_tweaks\
    mods\<name>\init.lua                         one folder per mod
    settings.ini                                 the settings below
    bindings.ini                                 the keys you bound to mods' hotkeys
    set.log                                      everything printed this session, each line with its time
    set.prev.log                                 the previous session's log, so a crash's log survives the restart
    console_history.txt                          the lines you typed in the console (see saveHistory)
    imgui.ini                                    window positions and sizes
    style.dat                                    your overlay style, if you saved one
    layer\                                       the overlay's Vulkan layer manifest, rewritten at every start
```

## Settings

`settings.ini` is read when the game starts. Edit it, or use the overlay's **Settings** window.

| Setting | Default | Meaning |
|---|---|---|
| `overlayKey` | `` ` `` | The key that opens the overlay: `` ` ``, F1-F24, Insert, Home, End, Delete, PageUp, PageDown, Pause, or a virtual-key code like `0xC0` |
| `fpsCap` | `0` | Frame rate cap: `0` is off, otherwise 10 and up |
| `antiAliasing` | `default` | `off` keeps the game's antialiasing at None (no FXAA, FSR3, DLSS or XeSS), which the game's menu doesn't offer |
| `asyncCompute` | `default` | `on` or `off` force the game's async compute; `default` lets the game pick. Applies at the next start |
| `console` | `0` | `1` also opens a separate console window. Its close button is disabled, because closing it would end the game |
| `historyLimit` | `100` | How many typed console lines Up and Down remember, 1 to 10000 |
| `saveHistory` | `1` | `1` keeps them across game starts in `console_history.txt`; `0` forgets them when the game closes |

## Update

Close the game and replace `powrprof.dll`. Your mods, settings and bindings stay.

## Uninstall

Delete `powrprof.dll` and the `shrouded_engine_tweaks` folder.

## Troubleshooting

- **Check the Status first:** open the overlay, **Settings**, **Status**. It lists what was found at startup: the
  version, the game build, the world tick, admin commands, game options, systems, resources and English names.
  Green is as expected, yellow works but isn't (an untested game build, something still being looked for), red is
  off or failed and says what's off because of it. The toolbar shows **(!)** in the worst of them.
- **After a game update:** the game build shows yellow until it's tested. Each feature checks itself; a feature that
  can't find what it needs turns red and says so, and everything else keeps working.
- **Reporting a problem:** send `set.log` (or `set.prev.log` if the game crashed), and say what you did. In it,
  yellow lines start with `[W]` and red ones with `[E]`.
- **Mods do nothing:** most of the API needs a world you host. In someone else's game, mods stay at the main-menu
  level (see [modding-guide.md](modding-guide.md)).
- **Other mod loaders:** Shrouded Engine Tweaks works next to mod loaders that patch the game's data files. They load
  through a different DLL.
