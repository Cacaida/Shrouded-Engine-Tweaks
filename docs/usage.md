# Using the overlay

The overlay opens and closes with your overlay key (picked at the first start, see
[installation.md](installation.md)).

- **While it's open,** mouse and keyboard go to the overlay only. The game sees none of it, and mods' key bindings
  stay silent. A gamepad still controls the game.
- **Mod windows** can also show while the overlay is closed. They take no input then.

## Toolbar

The buttons at the top open and close the built-in windows: **Console**, **Bindings**, **Resources**, **Systems**, **Hooks**,
**Game Log**, **Settings** and **ImGui Debug**. An open window's button is lit.

- The console starts open, the others closed. Positions and sizes are remembered.
- **Reload Mods** asks first, then restarts every mod from its `init.lua`, without restarting the game. It works at
  the main menu too.
- **(!)** means a feature is unavailable or reported a fault/refusal (red), or something is unexpected but works
  (yellow): see **Settings**, **Status**.

## Console

- **Type Lua** and press Enter. It runs at the next tick: the server tick in a world you host, the next frame
  elsewhere. Expressions print their value: `Ecs.player()` prints the player's entity id.
- **Up and Down** walk the lines you typed, kept across game starts by default.
- **Output** is everything Shrouded Engine Tweaks and the mods print: the last 2000 lines, all of it also in `set.log`.
  - **Colors:** green is as expected, yellow works but is unexpected, red failed (Lua errors included). What mods
    print stays plain. In `set.log`, yellow lines start with `[W]` and red ones with `[E]`.
  - **Clear** empties the window; the log keeps everything.
  - **Auto-Scroll** follows new lines while you're at the bottom.
  - **Ctrl+F** keeps only the lines that match. It ignores case; `a,b` shows lines with either word, `-a` hides lines
    with `a`. Esc or **x** closes it.
- **Mod loading** prints one line per mod: `Mod loaded: <name>`, `Mod failed: <name>` after its error, or
  `Mod skipped: <name> (no init.lua)`, then a total.

## Bindings

Every hotkey and input that mods registered, in one collapsible section per mod. Nothing is bound until you bind it.

- **Bind:** click the button next to an entry, then press the key, optionally with Ctrl, Shift or Alt. Esc cancels,
  **x** unbinds.
- A binding fires only with exactly its modifiers: Ctrl+F8 doesn't fire an F8 binding.
- Mouse buttons, modifiers alone and the overlay key can't be bound.
- Changes apply at once and are saved in `bindings.ini`.
- **Remove dead bindings** deletes saved keys that no loaded mod registers.
- The game still sees the keys you bind, so pick keys the game doesn't use.

## Resources

The game's own data: items, recipes, perks, enemy templates, loot tables and the rest, 91,812 resources of 146
types. It works from the main menu on.

- **Left:** pick a type, then filter. Both search boxes keep entries that hold every word you type, in any order and
  case: `wood log` finds `Material_T1_Wood_Logs`. Items show their English name next to their own.
- **Group by** splits the list into collapsible groups by one of the type's enum fields, exactly as the game stores
  it: items by `category` (Weapons, Materials, Ammunition...) or `equipment.slot`, templates by `family` (Npc,
  Animal, Plant...). Group names are the game's own. A resource too small to hold the field is listed under
  **Unreadable**, and extra parts of a resource under **Extra parts**. Groups open while you filter.
- **Right:** the selected resource's fields, structs and arrays as a tree.
  - **Edit** in place: numbers apply on Enter, bools are checkboxes, enums a list. The change goes into the game's
    memory and lasts until the game closes.
  - **Copy GUID** puts the GUID on the clipboard, for `Res.get`, `Admin.spawn` or `Admin.give`.
  - A reference to another resource has **Go**, which jumps to it. A text reference shows its English text.
- Long arrays show their first 500 elements.

## Systems

All 861 of the game's systems: the pieces of code it runs every tick, such as stamina drain, durability loss, fall
damage and spawners.

- **Untick one** and the game stops running it, in every world, until you tick it again or the game closes. Some are
  needed: if the world breaks, tick them again or reload the world.
- The filter keeps names that hold every word you type: `fall damage` finds `fall_damage_infliction`.
- **Off Only** lists what's off; **Turn All On** switches everything back.
- `exe+...` is the system's function in the game's exe, for reverse engineering.
- **Profile** times every system. The list is then sorted by the last second's CPU time, with calls per second and the
  world it ran in. **Log Top 30** writes the costliest to the console and `set.log`.

## Hooks

Native function observations registered by mods, grouped by target address.

- Each target shows its absolute address, executable offset and observed calls per second while subscribers are active.
- Rows show the mod/hook name, active or inactive state, invoked handlers, overflow drops, lifecycle discards,
  queue high-water mark/capacity and queue lock contention. Faults are shown at the target level.
- **Capture timing** is off initially. Turning it on records capture cost; averages use only timed calls. It adds
  measurement overhead and does not measure Lua handler time or total game impact.
- The storage total includes retained native queue and delivery capacity. Inactive registrations still occupy it.
- Recent refusals and capture faults are bounded to 32 diagnostic records and also written to the log. **Settings**,
  **Status**, **Function Hooks** shows whether any have occurred this session.
- Removal and reload deactivate handlers without removing detours. A target disabled by an invalid argument read
  stays disabled until restart. Fix the mod's declaration before trying it again.

Overflow means events were lost. Lifecycle discards mean pending work was invalidated by removal, reload, a world
transition or a fault; these are separate counts. A larger queue uses more memory and cannot fix an incorrect signature.

## Game Log

The game's own log, `<game>\enshrouded.log`, followed live.

- Warnings are yellow, errors red.
- Empty lines and the engine's state machine traces are left out; the file keeps everything.
- The same Clear, Auto-Scroll, wrapping and Ctrl+F as the console. It keeps the last 5000 lines.

## Settings

- The fields of `settings.ini` (see [installation.md](installation.md#settings)).
  - **Save** writes the file. A new overlay key, frame rate cap, antialiasing and console history apply at once; the
    console window and async compute at the next start.
  - **Load** re-reads the file; **Default** fills in the defaults (Save keeps them).
- **Dump Game Options** prints every game option, with the game's default next to any value you changed.
- **Status:** startup capabilities and runtime hook refusals/faults. The first thing to check when something doesn't work.

## ImGui Debug

- **Style editor:** colors, sizes and presets. **Save Style** keeps your style across starts, **Load Style** goes back
  to the saved one, **Default Style** to the default.
- **Metrics** and **ID Stack Tool**: ImGui's own debugging windows, for when a mod window misbehaves.

## If something goes wrong

- If drawing ever fails, the overlay turns itself off and says so in the log. The game and the mods keep running.
- A mod error never reaches the game: it's printed with the mod's file and line, and that callback is removed.
