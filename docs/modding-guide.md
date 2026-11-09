# Modding guide

A mod is a folder with an `init.lua` file in `<game>\shrouded_engine_tweaks\mods\`. Mods are written in Lua 5.4; if
Lua is new to you, [Programming in Lua](https://www.lua.org/pil/) is a good start. Every function is listed in the
[API reference](api-reference.md).

## Words you'll see

| Word | Meaning |
|---|---|
| **Entity** | Anything that exists in a world: you, an enemy, a chest, a plant. Each has a number, its id. |
| **Component** | A piece of data an entity has, like `Health` or `Faction`. You read and change entities through them. |
| **Resource** | The game's fixed data, the same in every world: item stats, recipes, perks, enemy types, loot tables. |
| **System** | One piece of game logic that runs over and over, like stamina drain or fall damage. |
| **Tick** | One step of the game world. The game runs many per second. |
| **A world you host** | A world running on your PC: single-player, or the server your game hosts. Only these can be changed. |
| **Hook, rule** | Advanced: watching or changing one of the game's own functions as it runs. |

## Your first mod

Create `mods\hello\init.lua`:

```lua
print("Hello from my first mod!")
Keys.on("F6", function() print("You pressed F6") end)
```

Start the game and open the overlay: the console shows your line and `Mod Loaded: hello`. Close the overlay, press F6,
and open it again to see the second line. After editing the file, press **Reload Mods** (or type `reload()`) instead
of restarting the game.

Now one that changes the game: a key that heals you.

```lua
local hp, maxHp, healNow = 0, 0, false

-- You pick the key in the overlay's Bindings window.
Keys.hotkey("heal", "Heal to full", function() healNow = true end)

-- Runs every tick. This is where you read and change the game.
Tick.add(function()
    local player = Ecs.player()
    local h = player and Ecs.get(player, "Health")
    if not h then return end              -- no player: at the main menu, or still loading
    if healNow then
        -- slot 0 is current HP, slot 2 max HP without gear and buffs: only ever raise HP to it
        h.dataStorage[0] = math.max(h.dataStorage[0], h.dataStorage[2])
        healNow = false
    end
    hp, maxHp = h.dataStorage[0], h.dataStorage[2]
end)

-- Runs every frame. This is where you draw windows; it can't touch the game.
Draw.add(function()
    if not Overlay.isOpen() then return end
    if ImGui.Begin("My heal") then
        ImGui.Text(("HP: %d / %d"):format(hp, maxHp))
        if ImGui.Button("Heal") then healNow = true end
    end
    ImGui.End()
end)
```

## Tick and Draw

Your code runs in two places, at different times:

| | `Tick.add` | `Draw.add` |
|---|---|---|
| When | every tick in a world you host, every frame elsewhere | every frame |
| Can touch the game (`Ecs`) | yes | no, it raises an error |
| Can draw windows (`ImGui`) | no | yes |

So read the game in Tick, keep what you need in variables, and show those in Draw. A button sets a flag (like
`healNow` above), and the next Tick acts on it. `Res`, `GameOptions`, `Types` and `Systems` work in both, and so do
most `Admin` commands.

## What works where

Mods load once, at the main menu, and stay loaded until the game closes. Entering or leaving a world doesn't reload
them.

| | Main menu | In a world you host |
|---|---|---|
| `Tick.add` | every frame | every tick |
| Console, keys, `Draw`, `Res`, `GameOptions`, `Types`, `Systems`, files | yes | yes |
| `Ecs.player`, `Ecs.get`, `Ecs.each` | nil, or nothing to loop over | yes |
| `Events.post`, `Admin.*` | error: "not in a world you host" | yes |
| `Observe` handlers | not called | after each tick |

A world someone else hosts counts as the main menu. That's why a Tick that uses the game checks for the player first:
`local p = Ecs.player() if not p then return end`.

## Lifecycle events

```lua
registerForEvent("onInit", function() end)        -- every mod's init.lua has run
registerForEvent("onWorldEnter", function() end)  -- you entered a world you host
registerForEvent("onWorldLeave", function() end)  -- you're leaving it
registerForEvent("onShutdown", function() end)    -- your mod is about to stop: save your files here
```

| Event | When |
|---|---|
| `onInit` | Once every mod's `init.lua` has run, at startup and after a reload |
| `onUpdate(dt)` | Like `Tick.add`; `dt` is the seconds since the last one |
| `onDraw` | Like `Draw.add` |
| `onOverlayOpen`, `onOverlayClose` | The first tick after the overlay opens or closes |
| `onWorldEnter` | The first tick in a world you host, and after a reload in one. The player can take a few ticks to appear |
| `onWorldLeave` | The moment you leave. `Ecs` and `Events.post` stop working here; `Res` and `GameOptions` still work |
| `onShutdown` | Right before your mod stops (a reload, or the game closing). At game close only files still work |

Register events at the top of `init.lua`. After a crash, `onShutdown` doesn't run, so save when your data changes too.

## Several mods together

- **Each mod has its own globals,** so two mods can use the same variable names.
- **The shared API can't be changed:** `Ecs.player = something` raises an error, so one mod can't break another.
- **`require("util")`** loads `util.lua` from your mod's folder.
- **`GetMod("other")`** gives what another mod's `init.lua` returned. Call it in `onInit`, when every mod has loaded.
- **Errors** show your file and line (`hello/init.lua:12: ...`), and the function that failed is removed. The game
  keeps running.

## When your mod stops, its changes go back

What your mod changes in resources, game options and systems goes back to the game's value when your mod stops:

- on **Reload Mods** or `reload()`, after your `onShutdown` (so your own clean-up runs first);
- when your mod **fails while loading**.

Not when the game closes: there's nothing to put back then. Changes you make yourself in the console or the overlay
aren't undone. If two mods change the same value, it goes back when the last of them stops. Entity data isn't undone:
entities come and go with the world anyway.

## Finding the game's data

The game describes all of its own data, so the console can tell you what exists:

```lua
Ecs.types("player")        -- search component and type names
Ecs.fields("Health")       -- the fields of a type
Ecs.get(Ecs.player(), "Health").dataStorage   -- print a value
Res.types("item")          -- resource types, and how many of each
local g, logs = Res.find("ItemInfo", "Wood Logs") print(g, logs.maxStackSize)
print(Res.text(logs.description))   -- the English text of a name or description
for _, n in ipairs(Systems.list("stamina")) do print(n) end
```

The overlay's **Resources**, **Entities** and **Systems** windows show the same data, with every field.

## Options and files

**Options** the player can change go through `Settings.define`. They're saved for you, and get a page in the
overlay's **Mod Settings** window ([reference](api-reference.md#settings)):

```lua
local s = Settings.define{
    {key = "volume", type = "number", default = 0.5, min = 0, max = 1, label = "Volume"},
    onChange = function(key, value) print(key, "is now", value) end,
}
print(s.volume)
```

**Anything else** goes in your own files, inside your mod's folder:

```lua
local f = io.open("save.json", "w")       -- mods\<your mod>\save.json
f:write(json.encode({visited = {"a", "b"}}))
f:close()

local f = io.open("save.json")            -- nil if it doesn't exist yet
if f then
    local ok, data = pcall(json.decode, f:read("a"))
    f:close()
end
```

Mods can only open files in their own folder, and can't write `settings.json` (that one belongs to
`Settings.define`).

## What mods can't do

Mods are treated as code from strangers, so they're limited to:

- **Lua:** base Lua, `string`, `table`, `math`, `utf8`, `coroutine`, `os.time`, `os.clock`, `os.date`, and `json`.
- **Files:** `io.open` and `io.lines`, inside your own folder only.
- **Not available:** the rest of `io` and `os`, `debug`, `dofile`, `loadfile`, `string.dump`, DLLs, and precompiled
  Lua.

## Game updates

A game update can break a mod, mostly ones that hook game functions. Mods that only use `Ecs`, `Res` and the other
data functions usually keep working.

**`mod.json`** (optional, next to `init.lua`) says what your mod was made for:

```json
{"requires": "0.2.0", "builds": [1793791052]}
```

- **`requires`:** the oldest Shrouded Engine Tweaks your mod needs. On an older one, your mod isn't loaded and the
  console says why.
- **`builds`:** the game builds you tested. On another build your mod still loads, with a yellow note in the console.
- **`Features()`** tells your mod what works on this game build ([reference](api-reference.md#features)), so it can
  turn its own options off instead of failing: `if not Features().admin.compatible then ... end`.
- **A mod that fails while loading is stopped completely,** so it never half-runs. Let what your mod can't work
  without fail at load, and use `pcall` only for optional extras.

## Hooking game functions (advanced)

Most mods never need this. Hooks reach the game's own code, below everything above:

- **Rules** change a game function's call while it runs: its arguments, its result, or whether it runs at all.
- **Observers** tell your mod that a function ran, and with which values, at the next tick.

The easy start is a rule on a **catalog target**: a game function Shrouded Engine Tweaks already studied and checks on
every game build. You just name it:

```lua
-- You take half damage from every hit.
Hooks.rule{name = "tough skin", target = "hit",
    when = {{arg = 4, field = "targetId", eq = "player"}},
    set = {{arg = 4, field = "healthChange", mul = 0.5}}}

-- Kills give double XP; a param changes the factor while the game runs.
local id = Hooks.rule{name = "xp boost", target = "combat_xp", params = {k = 2}, after = {{mul = "$k"}}}
Hooks.param(id, "k", 3)
```

A rule is data, not a function: the conditions (`when`) and changes (`set`, `after`, `block`) are applied for you,
instantly, without running Lua inside the game's code.

Any other function needs a **pattern**: bytes that find it in the game's exe. Finding one, and the function's
arguments, takes reverse engineering. A pattern belongs to one game build, so check it again after every update. Your
own patterns get observers and number-argument rules; changing fields, results or skipping calls works only on catalog
targets.

Everything else (the catalog list, every option, limits) is in the [API reference](api-reference.md#native-hooks). The
overlay's **Hooks** window shows what each hook does while you play.

## Good to know

- **Slow code slows the game:** a Tick holds up the game's tick. Anything over 50 ms gets a yellow line naming your
  mod.
- **`Ecs.player()`, `Ecs.each`, `Res.find` and `Res.each` search a lot:** keep their results, and loop on a key press or
  a timer, not every tick.
- **Resource edits** last until your mod stops. Make them in `onInit`, so they're in place before a world loads. Text
  and list lengths can't change, and resources can't be added or deleted.
- **Game option changes** aren't saved, and the game's menu doesn't show them.
- **Multiplayer:** joined players may not see some changes until the game itself next updates that value.
- **Keys** are physical keys from a US layout: `` ` `` may be a different key on yours.
- **Folder names:** characters your Windows language can't show, in the game's path or a file name, stop mods from
  loading.
- **English names only** for now.
