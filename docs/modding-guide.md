# Modding guide

A mod is a folder with an `init.lua`, in `<game>\shrouded_engine_tweaks\mods\`. Mods are plain Lua 5.4. The full list
of functions is in [api-reference.md](api-reference.md).

## Your first mod

`mods\myheal\init.lua`:

```lua
local player, hp, maxHp, healNow = nil, 0, 0, false

-- The player picks the key in the overlay's Bindings window.
Keys.hotkey("heal", "Heal to full", function() healNow = true end)

-- Every server tick in a world you host (every frame at the main menu, where there's no player): read and change the
-- game here.
Tick.add(function()
    player = player or Ecs.player()
    local h = player and Ecs.get(player, "Health")
    if not h then player = nil return end
    if healNow then
        -- slot 0 = current HP, slot 2 = max HP before gear and buffs: only ever raise HP to it
        h.dataStorage[0] = math.max(h.dataStorage[0], h.dataStorage[2])
        healNow = false
    end
    hp, maxHp = h.dataStorage[0], h.dataStorage[2]
end)

-- Every frame while the overlay is open: show what Tick read. Never touch the game here.
Draw.add(function()
    if not Overlay.isOpen() then return end
    if ImGui.Begin("My heal") then
        ImGui.Text(("HP: %d / %d"):format(hp, maxHp))
        if ImGui.Button("Heal") then healNow = true end
    end
    ImGui.End()
end)
```

Open the overlay: the console shows `Mod loaded: myheal`. Bind **Heal to full** in **Bindings**. After editing the
file, type `reload()` or press **Reload Mods**.

## Tick and Draw

Two places run your code, on different threads:

| | `Tick.add` / `onUpdate` | `Draw.add` / `onDraw` |
|---|---|---|
| When | every server tick in a world you host, every frame elsewhere | every frame, on the render thread |
| Game access | yes | no: `Ecs.*` and component views raise an error |
| ImGui | no | yes |

So read the game in Tick, keep what you need in variables, and show those in Draw. A button sets a flag that the next
Tick acts on. `Admin.*` is the exception: it only queues, so a button can call it directly. `Res`, `GameOptions`,
`Types` and `Systems` work in both.

## When mods load, and what works where

Mods load once, at the main menu, as soon as the game's resources are in, and stay loaded until the game closes.
Entering and leaving worlds doesn't reload them.

| | Main menu and loading | In a world you host |
|---|---|---|
| `Tick.add`, `onUpdate` | every frame, on the main thread | every server tick |
| Console, `Keys`, hotkeys, `Draw`, overlay events | yes | yes |
| `Res`, `GameOptions`, `Types`, `Systems`, `io`, `json`, `GetMod`, `reload()` | yes | yes |
| `Ecs.player`, `Ecs.get`, `Ecs.each` | nil, or an empty loop | yes |
| `Events.post`, `Admin.*` | an error: "not in a world you host" | yes |
| `Observe` handlers | registered, not called | after each server tick |
| `Hooks` handlers | before Tick when observations are queued | before Tick when observations are queued |

- A world someone else hosts counts as the main menu: only worlds you host are reachable.
- So a tick that uses the game looks for the player first: `local p = Ecs.player() if not p then return end`.

## Lifecycle events

```lua
registerForEvent("onInit", function() other = GetMod("other") end)   -- every mod's init.lua has run
registerForEvent("onWorldEnter", function() inWorld = true end)      -- first tick of a world you host
registerForEvent("onWorldLeave", function() inWorld = false end)     -- you left; the world is going away
registerForEvent("onShutdown", function() --[[ save files ]] end)    -- reload() or the game closing
```

| Event | When |
|---|---|
| `onInit` | Once every mod's `init.lua` has run: at the main menu at startup, and after `reload()` |
| `onUpdate(dt)` | Like `Tick.add`; `dt` is seconds since the previous one |
| `onDraw` | Like `Draw.add` |
| `onOverlayOpen`, `onOverlayClose` | At the first tick after the overlay opens or closes |
| `onWorldEnter` | The first tick of a world you host (a respawn doesn't count), and after `reload()` in a world. The player may take a few ticks to exist |
| `onWorldLeave` | The moment you leave. `Ecs.*` and `Events.post` raise "the world is closing"; `Res` and `GameOptions` still work |
| `onShutdown` | Right before your mod is unloaded. When the game closes, only files are left: save from what you have in memory |

- Register events at the top of `init.lua`. Other names raise an error.
- On a crash or a killed process, `onShutdown` doesn't run: save when your data changes, and use `onShutdown` as a
  last save.

## Mods side by side

- **Each mod has its own globals.** A global you set stays in your mod; reads fall back to the shared API.
- **The shared API is read-only:** `Ecs.player = myFunction` raises an error, so one mod can't break another. Keep
  your own functions in your own table.
- **`require("util")`** loads `util.lua` (or `util\init.lua`) from your mod's folder, once per mod.
- **`GetMod("other")`** gives what `mods\other\init.lua` returned. Mods load in folder order, so call it from
  `onInit`.
- **Errors** are printed with your file and line (`myheal/init.lua:12: ...`). A callback that errors is removed, and
  the line names your mod.

## Finding the game's data

Everything comes from the game's own type data, so the console can tell you what exists:

```lua
Ecs.types("player")        -- search type names
Ecs.fields("Health")       -- a type's fields with offsets, base classes included
Ecs.get(Ecs.player(), "Health").dataStorage   -- arrays print their values
Res.types("item")          -- resource types, with how many the game has
local g, logs = Res.find("ItemInfo", "Wood Logs") print(g, logs.debugName, logs.maxStackSize)
print(Res.text(logs.description))   -- the English text of a name or description
for _, n in ipairs(Systems.list("stamina")) do print(n) end
```

The overlay's **Resources** window shows the same resources with every field, and **Systems** every system.

## Observing native functions

Hooks tell your mod **that** a game function ran and **with which arguments**. You react on the next tick, with
everything else the API offers. They watch; they don't change the call.

| You can | You can't |
|---|---|
| React to game actions that have no event or readable component, e.g. "a craft was attempted", failed ones included | Stop, change or replace what the function does |
| Read its arguments: integers, floats, bools, entity ids, strings, copies of reflected structs up to 1 KB | See its return value, so whether the call succeeded |
| Act on it at the next tick: `Ecs.get`, `Admin.*`, `Res`, `Systems`, your own state and UI | Run code at the moment of the call; delivery is always one tick later |
| Count calls, see who made them, and measure the capture cost in the **Hooks** window | Keep a pattern working across game updates without checking it again |

A function only tells you what its entry sees. A crafting function that also handles failed attempts reports those
too: filter in your handler (by entity, for example), or pick a target that only runs on the outcome you want. If the
game already posts an event for that outcome, `Observe` is simpler and survives game updates better: finished crafts,
for example, arrive as `CraftRecipeSuccessEvent` and pickups as `PickupEvent` ([Events](api-reference.md#events)).

`Hooks.observe` registers an entry observer from your mod's initialization or normal tick work. Declare a unique name,
a verified entry pattern, argument types, a bounded queue capacity and a handler. See the
[native hooks reference](api-reference.md#native-hooks) for the complete shape and supported types. You cannot
register from Draw, the console or shutdown. Keep UI actions in flags that Tick processes.

Find and verify a target's calling convention and argument semantics before writing its declaration. Reflection only
describes data layouts. Ordinary Windows x64 calls support up to eight declared positions, including implicit object
and hidden return pointers. Skip unused positions explicitly. A readable snapshot may still be inconsistent or refer
to the wrong object. Copied entity ids provide no lifetime or current-world guarantee.

The original function runs immediately after capture. Your handler receives independent Lua-owned copies later,
before ordinary Tick callbacks, and can retain or edit them. It cannot override the native call. Keep handlers short:
queues are bounded, and overflow drops events. Check `ev.dropped` and the overlay's **Hooks** window.

Removal and handler errors stop delivery immediately. Reload deactivates old handlers before teardown and only
reactivates matching registrations when the new mod registers them. World transitions discard pending observations.
Inactive detours stay installed until restart. Keeping the same name, target, arguments and capacity reuses storage;
changing declarations consumes retained slots. Faulted targets require a restart.

## Saving your mod's files

```lua
local f = io.open("settings.json", "w")   -- mods\<your mod>\settings.json
f:write(json.encode({volume = 0.5}))
f:close()

local f = io.open("settings.json")        -- nil if it doesn't exist
if f then
    local ok, data = pcall(json.decode, f:read("a"))
    f:close()
end
```

`io.open` and `io.lines` work inside your mod's folder only. Paths that could leave it (absolute paths, drives,
`..`) raise an error.

## The sandbox

Mods are treated as third-party code:

- **Available:** base Lua, `string`, `table`, `math`, `utf8`, `coroutine`, `json`, `os.time`, `os.clock`, `os.date`,
  and `io.open` / `io.lines` in your own folder.
- **Source text only:** mods, `require` and `load` refuse precompiled bytecode.
- **Not available:** the rest of `io`, `debug`, `dofile`, `loadfile`, `string.dump`, `os.execute`, `os.remove`,
  `os.rename`, `os.exit`, `os.getenv`, native DLLs.

## Limits

- **Hosted worlds only** for the game itself (see the table above).
- **Native hooks observe only:** no inline Lua overrides, return capture or custom register conventions. Patterns
  belong to one game build: check them again after every game update.
- **Resource edits live in memory:** make them in `onInit`, so they're in place before any world loads. Strings and
  array lengths can't change size; resources can't be created, cloned or deleted.
- **Game option changes aren't saved,** and the game's settings menu doesn't show them.
- **Replication:** writes skip the game's dirty-marking. Some fields only reach other players when the server next
  touches them.
- **Cost:** `Ecs.player()`, `Ecs.each`, `Res.find` and `Res.each` walk a whole map per call. Keep their results, and
  run loops on a key press or a timer, not every tick.
- **Tick and Draw share one Lua state,** so they wait for each other: keep both short.
- **Keys** are virtual-key codes, so `` ` `` is a different physical key on non-US layouts.
- **Paths are ANSI:** characters outside your Windows code page in the game folder's path or a file name keep mods
  from loading.
- **English names only** for now.
