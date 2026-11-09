# Using the overlay

The overlay opens and closes with your overlay key, pressed alone (picked on a first start, see
[installation.md](installation.md)). With Ctrl, Shift or Alt held, the key goes to the game and other mods instead.

- **While open,** mouse and keyboard go to the overlay only. The game sees none of it, and mods' key bindings
  stay silent. Gamepad still controls the game.
- **Mod windows** can also show while the overlay is closed. They take no input.

## Toolbar

The buttons at the top open and close the built-in windows: **Console**, **Bindings**, **Resources**, **Entities**,
**Systems**, **Hooks**, **Game Log**, **Settings** and **ImGui Debug**. An open window's button is highlighted.
**Mod Settings** and **Compatibility** open from the Settings window.

- The console starts open, the others closed. Positions & sizes are remembered.
- **Reload Mods** asks first, in the middle of the screen. Then it puts back what the mods changed in resources,
  options and systems, and restarts every mod from its `init.lua`, without restarting the game. It works at the main
  menu too.
- **(!)** means something is off or failed (red), or works but is unexpected (yellow): see **Settings**, **Status**.

## Console

- **Type Lua** and press Enter. It runs at the game's next tick (the next frame at the main menu). A value prints
  itself: `Ecs.player()` prints your player's ID.
- **Up and Down** walk the lines you typed, kept across game starts by default.
- **Output** is everything SET and the mod(s) print: The last 2000 lines, all of it also in `set.log`.
  - **Colors:** green is as expected, yellow works but is unexpected, red failed (Lua errors included). What mods
    print stays plain. In `set.log`, yellow lines start with `[W]` and red ones with `[E]`.
  - **Clear** empties the window; the log keeps everything.
  - **Auto-Scroll** follows new lines while you're at the bottom.
  - **Ctrl+F** keeps only the lines that match. It ignores case; `a,b` shows lines with either word, `-a` hides lines
    with `a`. Esc or **x** closes it.
- **Mod loading** prints one line per mod: `Mod Loaded: <name>`, `Mod Failed: <name>` after its error, or
  `Mod Skipped: <name> (no init.lua)`, then a total.
- **Only in `set.log`:** debug lines, entering and leaving a world, and the overlay opening and closing. They help
  with bug reports without filling the console.

## Bindings

Every hotkey and input that mods registered, in one collapsible section per mod. Nothing is bound until you bind it.

- **Bind:** click the button next to an entry, then press the key, optionally with Ctrl, Shift or Alt. Esc cancels,
  **x** unbinds. Keys show as your keyboard labels them: `Ü`, `ß` or `#` on a German keyboard, `;` or `]` on a US
  one, and by name for keys without a symbol (`Space`, `Enter`, `Num5`).
- A binding fires only with exactly its modifiers: Ctrl+F8 doesn't fire an F8 binding.
- Mouse buttons, modifiers alone and the overlay key can't be bound.
- Changes apply at once and are saved in `bindings.ini`.
- **Remove dead bindings** deletes saved keys that no loaded mod registers.
- The game still sees the keys you bind, so pick keys the game doesn't use.

## Resources

The game's own data: Items, recipes, perks, enemy templates, loot tables and the rest, about 92,000 resources of 146
types (91,909 on build 1793791052). It works from the main menu.

- **Left:** pick a type, then filter. Both search boxes keep entries that hold every word you type, in any order and
  case: `wood log` finds `Material_T1_Wood_Logs`. Items show their English name next to their own.
- **Group by** sorts the list into groups by one of the type's fields: items by `category` (Weapons, Materials...)
  or `equipment.slot`, enemy and object templates by `family` (Npc, Animal, Plant...). Groups open while you filter.
- **Right:** the selected resource's fields, structs and arrays as a tree.
  - **Edit** in place: numbers apply on Enter, bools are checkboxes, enums a list. The change goes into the game's
    memory and lasts until the game closes.
  - **Copy GUID** puts the GUID on the clipboard, for `Res.get`, `Admin.spawn` or `Admin.give`.
  - A reference to another resource has **Go**, which jumps to it. A text reference shows its English text.
- Long arrays show their first 500 elements.

## Entities

The entities of a world you host: the player, enemies, animals, placed objects. It reads the world between game ticks,
and only while the window is open or you watch a value.

- **Left:** The 200 entities nearest to you, refreshed twice a second, with their distance, health and faction. Type a
  component name (`Health`, `Faction`...) to list only entities that have it, or an id to look one up, then **Show**.
- **Right:** The selected entity's components. Expand one to see its fields, refreshed ten times a second.
  - **Edit** like in Resources: numbers on Enter, bools as checkboxes, enums as a list. The change is made at the next
    game tick, and only if the value is still the one you see; otherwise the line under the entity says why not.
  - A field that holds another entity's id has **Select**, which jumps to that entity.
  - Right-click a value and pick **Watch this value** to add it to the **Watchlist** below, which keeps updating while
    you play, up to 64 values.
- **Gone:** An entity that leaves the world (killed, or its area unloaded when you travel away) is gone for good in this
  window, and so are its watched values. If it comes back when you return, the game made a new one: pick it again
  from the list.
- Edits made here are yours, like console edits: Reload Mods doesn't undo them.

## Systems

All 861 of the game's systems: The pieces of code it runs every tick, such as stamina drain, durability loss, fall
damage and spawners.

- **Untick one** and the game stops running it, in every world, until you tick it again or the game closes. Some are
  needed: if the world breaks, tick them again or reload the world.
- The filter keeps names that hold every word you type: `fall damage` finds `fall_damage_infliction`.
- **Off Only** lists what's off; **Turn All On** switches everything back.
- `exe+...` is the system's function in the game's exe, for reverse engineering.
- **Profile** times every system. The list is then sorted by the last second's CPU time, with calls per second and the
  world it ran in. **Log Top 30** writes the costliest to the console and `set.log`.

## Hooks

For modders: The game functions mods hook ([native hooks](api-reference.md#native-hooks)), one section per function,
with how often it's called.

- **Observers:** How many calls each one handled, how many it **dropped** because its queue was full, and how many
  were **discarded** because of a reload or a world change.
- **Rules:** On or off, **hits** (calls they changed), **refused** (calls skipped because a field wasn't where it's
  safe to change), faults, and the other mod's rule when two change the same value.
- **Timing** measures what the hooks cost per call. It's off at first, because measuring costs a little too.
- A function whose memory couldn't be read stays off until the game restarts: fix the mod first. Problems also go to
  the log, and to **Settings**, **Status**, **Function Hooks**.

## Game Log

The game's own log, `<game>\enshrouded.log`, followed live.

- Warnings are yellow, errors red.
- Empty lines and the engine's state machine traces are left out; the file keeps everything.
- The same Clear, Auto-Scroll, wrapping and Ctrl+F as the console. It keeps the last 5000 lines.

## Settings

- The fields of `settings.ini` (see [installation.md](installation.md#settings)): **Separate Console Window**,
  **Overlay Key**, **FPS Cap**, **Async Compute**, **Antialiasing Off**, **Console History**, **Keep Console
  History** and **Debug Logs** (on by default).
  - **Save** writes the file. A new overlay key, FPS cap, antialiasing, console history and debug logs apply at once;
    the console window and async compute on restart.
  - **Load** re-reads the file; **Default** fills in the defaults (Save keeps them).
- **Dump Game Options** prints every game option, with the game's default next to any value you changed.
- **Mod Settings** and **Compatibility**, on the right of that row, open those windows (below).
- **Status:** What was found at startup, and any problem since. The first thing to check when something doesn't
  work. **Catalog** says how many rule targets work on this game build. **Read Point** should always be green: red
  means a safety check failed, so please report it.
- **Bug Report:** **Copy to Clipboard** copies this session's log, **Copy last session's logs** the one before.
  **Report on GitHub** and **Report on Nexus** open the bug report page ([reporting.md](reporting.md)).

### Mod Settings

The options mods offer (`Settings.define`), in one collapsible section per mod: Checkboxes, sliders, number fields,
lists and text fields.

- Changes apply at once: The mod hears of them at its next tick.
- They're saved in the mod's `settings.json` within a second, and kept across game starts.
- Hover over an option to see its key, the name the mod's code and its `settings.json` use.

### Compatibility

What works on this game build, and what works right now: one row per feature and per rule target, with its ID (what
mods read with `Features()`) and why it's off.

- **Green = Works.**
- **Yellow = Works on this build, but not right now:** Admin commands outside a world you host, for example. The game
  build's own row is yellow when the build is untested; every other row still checks itself.
- **Red = Off on this build:** Not found (game updated?), or a rule target that didn't check out.

## ImGui Debug

- **Style editor:** Colors, sizes and presets. **Save Style** keeps your style across starts, **Load Style** goes back
  to the saved one, **Default Style** to the default.
- **Metrics** and **ID Stack Tool**: ImGui's own debugging windows, for when a mod window misbehaves.

## If something goes wrong

- If drawing ever fails, the overlay turns itself off and says so in the log. The game and the mods keep running.
- After a session that didn't close normally, a **Last Session** window opens by itself at the next start: what the
  game's crash report says, and **Copy to Clipboard** for that session's log. Dismissing it closes the
  overlay again. Ending the game from Task Manager's Processes tab closes it normally, so no window follows.
- A mod error never reaches the game: it's printed with the mod's file and line, and that callback is removed.
