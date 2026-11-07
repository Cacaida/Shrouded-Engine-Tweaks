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

Another mod also ships a `powrprof.dll`? See [Other DLL mods](#other-dll-mods).

That's it. Nothing outside the game folder is changed, and the game's own files stay untouched.

## First start

The first start creates `shrouded_engine_tweaks\` in the game folder, and the overlay opens with a small window in
the middle: press the key that should open and close it.

- The game gets no input until you pick one.
- Keys that type text or close menus are refused, and so is F10. That leaves `` ` ``, F1-F24 except F10, Insert,
  Delete, Home, End, PageUp, PageDown and Pause.
- A key pressed with Ctrl, Alt or Shift held is ignored, so another app's hotkey (a recorder's Alt+F9, say) can't
  become the overlay key by accident.
- The key is saved, and the overlay stays open, so pressing it again closes the overlay.
- If the game closes before you pick one, the window comes back at the next start.

At every start, the top-left corner shows which key opens the overlay for about 5 seconds. You can change the key
later in the overlay's **Settings** window.

## Files

```
<game>\powrprof.dll                              Shrouded Engine Tweaks itself
<game>\shrouded_engine_tweaks\
    mods\<name>\init.lua                         one folder per mod
    plugins\                                     other DLL mods, loaded with the game (see Other DLL mods)
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
| `overlayKey` | empty | The key that opens the overlay: `` ` ``, F1-F24, Insert, Home, End, Delete, PageUp, PageDown, Pause, or a virtual-key code like `0xC0`. Empty: the overlay asks for it at the next start |
| `fpsCap` | `0` | Frame rate cap: `0` is off, otherwise 10 and up |
| `antiAliasing` | `default` | `off` keeps the game's antialiasing at None (no FXAA, FSR3, DLSS or XeSS), which the game's menu doesn't offer |
| `asyncCompute` | `default` | `on` or `off` force the game's async compute; `default` lets the game pick. Applies at the next start |
| `console` | `0` | `1` also opens a separate console window. Its close button is disabled, because closing it would end the game |
| `historyLimit` | `100` | How many typed console lines Up and Down remember, 1 to 10000 |
| `saveHistory` | `1` | `1` keeps them across game starts in `console_history.txt`; `0` forgets them when the game closes |

## Other DLL mods

Some Enshrouded mods also install themselves as `powrprof.dll` in the game folder, so only one of them fits there.
Shrouded Engine Tweaks loads them for you instead:

1. Move the other mod's `powrprof.dll` into `shrouded_engine_tweaks\plugins\`. Any name works, so rename it after its
   mod (`auto_doors.dll`). Its other files (configs, folders) stay where that mod's instructions put them.
2. Put Shrouded Engine Tweaks' `powrprof.dll` in the game folder.

Every `.dll` in `plugins\` is loaded at game start, in name order, at the same point the game would have loaded it as
`powrprof.dll`. `set.log` and **Settings**, **Status** list each one, loaded or with the error that stopped it.

- **With Vortex:** let Shrouded Engine Tweaks win the `powrprof.dll` conflict, then copy the other mod's DLL from its
  download into `plugins\` by hand.
- If the other mod hooks the same game code Shrouded Engine Tweaks uses, both still work: Shrouded Engine Tweaks
  finds the code by its original bytes, and the two hooks run one after the other.

## Update

Close the game and replace `powrprof.dll`. Your mods, settings and bindings stay.

## Uninstall

Delete `powrprof.dll` and the `shrouded_engine_tweaks` folder.

## Troubleshooting

- **No key window at the first start, no hint in the corner:** Shrouded Engine Tweaks isn't running. Check that
  `shrouded_engine_tweaks\set.log` exists, and that the game folder's `powrprof.dll` is this one (right-click it,
  **Properties**, **Details**: the description says Shrouded Engine Tweaks). Another mod's `powrprof.dll` may have
  replaced it: see [Other DLL mods](#other-dll-mods).
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
