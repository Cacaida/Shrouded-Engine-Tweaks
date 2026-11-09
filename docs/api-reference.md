# API reference

Everything a mod can call. New to modding? Start with the [modding guide](modding-guide.md).

| Area | Functions |
|---|---|
| [Entities](#entities-ecs) | `Ecs.player()`, `Ecs.get(id, "Component")`, `Ecs.each("Component", ...)`, `Ecs.fields(type)`, `Ecs.types(search)` |
| [Events](#events) | `Observe("EventType", fn)`, `Events.post("EventType", {fields})` |
| [Resources](#resources-res) | `Res.get(guid [, type])`, `Res.each(type)`, `Res.find(type, name)`, `Res.text(guid)`, `Res.types(search)` |
| [Game options](#game-options) | `GameOptions.machine()`, `GameOptions.user()`, `GameOptions.dump([prefix])` |
| [Systems](#systems) | `Systems.list([search])`, `Systems.enable(name, on)`, `Systems.enabled(name)`, `Systems.profile(on)`, `Systems.stats()` |
| [Types](#types-and-values) | `Types.fields(name)`, `Types.enum(name)`, `Types.new(name [, values])`, `Types.copy(view)` |
| [Ticks and lifecycle](#ticks-and-lifecycle) | `Tick.add(fn(dt))`, `registerForEvent(name, fn)`, `reload()` |
| [Keys](#keys) | `Keys.on(key, fn)`, `Keys.hotkey(id, label, fn)`, `Keys.input(id, label, fn(down))` |
| [UI](#ui-imgui) | `Draw.add(fn)`, `ImGui.*`, `Overlay.isOpen()` |
| [Admin](#admin-commands) | `Admin.setTime`, `teleport`, `spawn`, `give`, `setLevel`, `setFlameLevel`, `settings()`, `changeSettings{...}`, `send` |
| [Settings](#settings) | `Settings.define{...}` |
| [Features](#features) | `Features()` |
| [Mods and files](#mods-and-files) | `GetMod(name)`, `GetVersion()`, `GetGameBuild()`, `require(name)`, `io.open`, `io.lines`, `json.encode`, `json.decode`, `print` |
| [Native hooks](#native-hooks) (advanced) | `Hooks.rule{...}`, `Hooks.param(id, name, value)`, `Hooks.list(id, ids)`, `Hooks.observe{...}`, `Hooks.remove(id)` |

## Entities (Ecs)

```lua
local p  = Ecs.player()            -- your player's entity id, or nil
local hp = Ecs.get(p, "Health")    -- a component by name ("Health" or "keen::ecs::Health"); nil if it has none
print(hp.dataStorage[0])           -- read a field; lists start at 0, like in the game
hp.dataStorage[0] = 100            -- change it
for id, hp, f in Ecs.each("Health", "Faction") do end   -- every loaded entity that has all of these
Ecs.fields("Health")               -- print a type's fields
Ecs.types("player")                -- search type names (first 100)
```

- **Always current:** a component you got reads the game again on every use, so once its entity is gone, using it
  raises an error.
- **`Ecs.each`** takes 1 to 8 component names and gives the id, then one component per name. It only sees loaded
  entities: the world around you.
- **Health:** `dataStorage` slot 0 is current HP, slot 2 max HP without gear and buffs. With bonuses, current HP can be
  higher than slot 2, so never lower HP to it.
- **Simple wrappers read as plain values:** an `EntityId` or a `Time` reads as a number, so `e.deadEntityId` can be
  compared with `Ecs.player()` directly.
- **Changes are checked:** a value must fit its field, and an enum field takes only its own values, as a number or by
  name (`"None"`, any case). The same goes for resources, options, `Events.post` and `Admin`.
- **Whole structs** can be set from a table (`g.windowSize = {x = 1920, y = 1080}` changes just those fields) or from a
  value of the same type. Structs holding text or variable-length lists can't be set whole.
- **Variable-length game values** read as `<TypeName>`, not as data.

## Events

```lua
Observe("EntityDiedEvent", function(e)   -- every event of this type, right after each tick
    print(e.deadEntityId, e.timeStamp)   -- read-only, and only valid inside the function
end)
Events.post("TestEvent1", {data = 42})   -- send an event through the game's own queue
```

- **`Events.post`** checks the event type and its fields; anything wrong raises an error and sends nothing. The game
  handles it at the next tick. It returns false if the game's queue is full.
- **Finished crafts:** `CraftRecipeSuccessEvent`, once per craft, from the backpack (`workshopId` 0) or a station,
  with the `recipeId`.
- **Pickups:** `PickupEvent`, once per pickup (loot and harvested plants alike), with `itemId`, `itemCount`,
  `pickedUpByEntityId` and `pickedUpFromEntityId`.

```lua
Observe("CraftRecipeSuccessEvent", function(e)
    print(e.workshopId == 0 and "backpack" or "station", e.recipeId)
end)
Observe("PickupEvent", function(e)
    print(e.pickedUpByEntityId, e.itemId, e.itemCount)
end)
```

## Resources (Res)

The game's fixed data: every item, recipe, perk, enemy type, loot table and setting it ships. About 92,000 resources
of 146 types (91,909 on build 1793791052), always in memory.

```lua
local item = Res.get("1a2b3c4d-....", "ItemInfo")      -- by GUID (its unique id) and type; nil if none
local any  = Res.get(guid)                             -- the first resource with that GUID
for guid, item, part in Res.each("ItemInfo") do end    -- every resource of a type
local guid, t = Res.find("TemplateResource", "<name>") -- by its internal name, any case
local guid, logs = Res.find("ItemInfo", "wood logs")   -- or by its English name
print(Res.text(logs.name), Res.text(logs.description)) -- English text, or nil
Res.types("item")                                      -- print matching resource types, with counts
```

- **Fields** read like components. Text is a Lua string, lists start at 0 and have a length (`#item.tags`), and a
  link to another resource reads as its GUID: follow it with `Res.get`.
- **Names:** `debugName` and `name` are the developers' names. The names players see are text links: `Res.text` gives
  their English text.
- **Changing:**

  ```lua
  local _, logs = Res.find("ItemInfo", "Material_T1_Wood_Logs")
  logs.maxStackSize = 999            -- numbers, true/false and enums
  logs.name = other.name             -- links take a GUID
  logs.exchangeOnBroken = nil        -- nil clears a link
  ```

  - A change lasts until your mod stops: then the game's value
    [goes back](modding-guide.md#when-your-mod-stops-its-changes-go-back). Make it in `init.lua` or `onInit` to have
    it every time.
  - Some values apply at once; ones the game copies at world load apply at the next world load.
  - Text and list lengths can't change, and resources can't be added, copied or deleted.
  - On your own multiplayer server, joined players keep their unchanged copy.
- **Before the game has loaded its data,** `Res` raises "resources are not available (yet)". It works everywhere,
  Draw included.

## Game options

The game's own settings, changed live:

- **`GameOptions.machine()`:** this PC's settings: window, graphics, sound.
- **`GameOptions.user()`:** your settings: field of view, volumes, camera, gameplay and HUD.

```lua
local m = GameOptions.machine()
print(m.graphics.render.antiAliasing)            -- enums read as numbers
m.graphics.render.antiAliasing = "None"          -- and take names or numbers
GameOptions.user().gameplay.hideHUD = true
GameOptions.user().graphics.fov = 100            -- applies at once, even beyond the menu's range
GameOptions.dump("user.gameplay")                -- print every option under this path; dump() prints all
```

- **A change applies like one made in the game's menu.** It lasts until your mod stops or you change that option in
  the menu. It isn't saved, and the menu doesn't show it. Make it in `onInit` to keep it.
- **`dump`** shows the game's default next to every value that differs from it.
- **Never printed:** hosting passwords and banned players.
- **Before the game has its settings,** both raise "game options not found yet". It works everywhere, Draw included.

## Systems

The game runs its logic as 861 named systems: `stamina_depletion_sprint`, `durability_loss`,
`fall_damage_infliction`, `random_spawner`...

```lua
for _, n in ipairs(Systems.list("stamina")) do print(n) end   -- names containing the search
Systems.enable("stamina_depletion_sprint", false)              -- sprinting costs no stamina
print(Systems.enabled("stamina_depletion_sprint"))             -- false
Systems.profile(true)                                          -- start measuring
local list, secs = Systems.stats()                             -- {{name, ms, calls, world}, ...}, costliest first
```

- **Off means off completely,** for everyone, in every world, until it's switched back on, your mod stops, or the game
  closes.
- **Some are needed:** switching off network, collider or `*_init` systems can break the world.
- **Only on or off:** for "half the durability loss", change the data instead.
- **Profiling** measures CPU time over all threads, not frame time: `ms / secs` is milliseconds per second.

## Types and values

The game describes all of its data types, about 12,650. `Types` reads those descriptions, and makes values your mod
keeps.

```lua
for _, f in ipairs(Types.fields("ItemInfo")) do print(f.name, f.type, f.offset) end
local aa = Types.enum("RenderAntiAliasingSetting")              -- {None = 0, Fxaa = 1, ...}
local m = Types.new("keen::g38_options::pc::machine::Settings") -- starts from the game's defaults
local saved = Types.copy(GameOptions.machine().graphics.renderPreferences)
GameOptions.machine().graphics.renderPreferences = saved        -- put it back later
```

- **`Types.new(name [, values])`** makes a value of that type, starting from the game's defaults, then `values`.
- **`Types.copy(view)`** copies a struct or list out of the game.
- Both read and write like the game's own data, never go out of date, and can be assigned to a field of the same type.
- **Names:** a short name works when only one type has it; otherwise use the full one.

## Ticks and lifecycle

```lua
Tick.add(function(dt) end)                    -- every tick in a world you host, every frame elsewhere
registerForEvent("onInit", function() end)    -- see the modding guide for every event
reload()                                      -- restart every mod
```

Events: `onInit`, `onUpdate(dt)`, `onDraw`, `onOverlayOpen`, `onOverlayClose`, `onWorldEnter`, `onWorldLeave`,
`onShutdown`. When each runs: [modding guide](modding-guide.md#lifecycle-events).

## Keys

```lua
Keys.on("F6", function() end)                        -- a fixed key
Keys.hotkey("heal", "Heal to full", function() end)  -- the player picks the key in the Bindings window
Keys.input("sprint", "Sprint while held", function(down) end)   -- true when pressed, false when released
```

- **Key names:** F1-F24, A-Z, 0-9, `` ` ``, Insert, Home, End, Delete, PageUp, PageDown, Pause, Space, Tab, Enter,
  Backspace, CapsLock, Left, Up, Right, Down, PrintScreen, ScrollLock, NumLock, Num0-Num9, NumMultiply, NumAdd,
  NumSubtract, NumDecimal, NumDivide, Semicolon, Equals, Comma, Minus, Period, Slash, LeftBracket, Backslash,
  RightBracket, Quote, `<>` (the extra key next to left Shift on many non-US keyboards), or `0xNN`. Punctuation is
  named after where it sits on a US keyboard, so a name means the same key on every layout: `Semicolon` is the key
  that types Ü on a German keyboard. The Bindings window shows the player the symbol their own layout puts there.
- **`id`** uses letters, digits and `_`, once per mod. **`label`** is what the Bindings window shows.
- Keys only fire while the game window is active and the overlay is closed. A held input is released when you
  switch away or open the overlay.

## UI (ImGui)

```lua
Draw.add(function()                          -- every frame
    if not Overlay.isOpen() then return end  -- leave this out to show the window over the game (no clicks then)
    if ImGui.Begin("My mod") then            -- also Begin(name, flags) and Begin(name, open [, flags]) -> open, draw
        ImGui.Text("hello")
        if ImGui.Button("Do it") then doIt = true end
        enabled, pressed = ImGui.Checkbox("Enabled", enabled)
        speed, used = ImGui.SliderFloat("Speed", speed, 0, 10)
    end
    ImGui.End()                              -- always, even when Begin returned false
end)
```

- **Windows:** `Begin`, `End`, `CollapsingHeader`, `SetNextWindowPos(x, y [, cond])`, `SetNextWindowSize(w, h [, cond])`.
- **Scrolling areas:** `BeginChild(id [, w, h [, border [, flags]]])` (0 fills the space), then always `EndChild()`.
- **Text and layout:** `Text`, `TextColored(r, g, b, a, text)`, `Separator`, `SameLine`, `Spacing`.
- **Widgets:** `Button(label [, w, h])`, `Checkbox`, `SliderFloat`, `SliderInt`, `InputFloat`, `InputInt`,
  `InputText(label, text, maxLen)`.
- **Tooltips:** `IsItemHovered`, `SetTooltip`.
- **Constants:** `ImGuiCond.{Always, Once, FirstUseEver, Appearing}`, `ImGuiWindowFlags.{None, NoTitleBar, NoResize,
  NoMove, NoScrollbar, NoCollapse, AlwaysAutoResize, NoBackground, NoSavedSettings, NoInputs}`.
- `ImGui` works only inside Draw.

## Admin commands

The game's own admin commands, run as you, the host:

```lua
Admin.setTime(12)                          -- time of day in hours, 0 to below 24; 12.5 = 12:30
Admin.teleport(x, y, z)                    -- in meters; the game picks a safe spot nearby
Admin.teleport(x, y, z, true)              -- exactly there
Admin.setLevel(20)                         -- your character level, 1 to 255
Admin.setFlameLevel(3)                     -- the world's flame level, saved with the world
Admin.spawn("Enemy_Wildbeast_wolf_earlygame")   -- an enemy or object by name or GUID, 5 m from you
Admin.give("Wood Logs", 75)                -- items into your inventory
Admin.changeSettings({enemyDamageFactor = 0.5}) -- world settings, see below
Admin.send("adminSetDaytime", {syncDaytime = 6 * 3600 * 1000000000})   -- any command from the table, by hand
```

- **Commands run at the next tick.** Calling the same one twice before then keeps only the second (world settings
  are merged instead).
- **Where:** only in a world you host, else they raise "not in a world you host". `Admin.settings()` and
  `Admin.spawn` read the game, so they can't run in Draw; the rest can.
- **Your position** is stored as meters × 2³², y up:
  `local pos = Ecs.get(Ecs.player(), "CurrentTransform").transform.position print(pos.x / 2^32)`.
- **`Admin.give(item [, count [, level [, rarity]]])`:** an item by GUID, internal name or English name. Count 1 to
  999, level 1 to 255, rarity a name (`"Legendary"`) or 0 to 4. The game gives one stack per tick, so 75 logs at stack
  size 50 take about a second.
- **`Admin.spawn(template [, count [, level]])`:** by GUID or name (enemies start with `Enemy_`). Count 1 to 255,
  level 1 to 65535.
- **`Admin.send(command, fields)`** fills a command's fields by name, checked like `Events.post`. Missing fields are 0.
  A teleport sent this way needs `orientation = {w = 1}`.
- A command the game didn't run logs `Admin Command <name>: Not run by the game`.

| Command | Helper | Fields type | Tested in game |
|---|---|---|---|
| `adminSetDaytime` | `setTime` | `AdminSetDayTimeAction` | yes |
| `adminTeleport` | `teleport` | `AdminTeleportAction` | yes |
| `adminSetFlameLevel` | `setFlameLevel` | `AdminSetFlameLevel` | yes |
| `adminSetCharacterLevel` | `setLevel` | `AdminSetCharacterLevel` | yes |
| `adminChangeGameSettingsAction` | `changeSettings` | `AdminChangeGameSettingsAction` | yes |
| `adminSpawnCommand` | `spawn` | `AdminSpawnCommand` | yes |
| `adminInventoryCreateAction` | `give` | `AdminInventoryCreateAction` | yes |
| `adminRepairAllItemsAction` | | `RepairAllItemsAction` (`{}`) | no |
| `adminRefillAllFuelAction` | | `RefillAllFuelAction` (`{}`) | no |
| `adminApplyInventory` | | `AdminApplyInventoryCommand` | no |

Not available: kill faction (the game doesn't run it in released builds), destroy item and perk.

### World settings

```lua
local s = Admin.settings()                 -- the current settings as a table; nil until the world's first tick
Admin.changeSettings({enemyDamageFactor = 0.5, weatherFrequency = 1})   -- only these change
```

- Only what you name changes. It's saved with the world, like a change in the game's own menu, for everyone on it.
- Times are in nanoseconds: 30 minutes is `30 * 60 * 10^9`.
- `miningDamageFactor` is saved, but the game doesn't use it.

| Setting | Type |
|---|---|
| `playerHealthFactor`, `playerManaFactor`, `playerStaminaFactor`, `playerBodyHeatFactor`, `playerDivingTimeFactor` | number |
| `enableDurability`, `enableStarvingDebuff`, `enableGliderTurbulences`, `pacifyAllEnemies` | true/false |
| `foodBuffDurationFactor`, `shroudTimeFactor`, `miningDamageFactor`, `plantGrowthSpeedFactor` | number |
| `resourceDropStackAmountFactor`, `factoryProductionSpeedFactor`, `perkUpgradeRecyclingFactor`, `perkCostFactor` | number |
| `experienceCombatFactor`, `experienceMiningFactor`, `experienceExplorationQuestsFactor` | number |
| `enemyDamageFactor`, `enemyHealthFactor`, `enemyStaminaFactor`, `enemyPerceptionRangeFactor` | number |
| `bossDamageFactor`, `bossHealthFactor`, `threatBonus` | number |
| `fromHungerToStarving`, `dayTimeDuration`, `nightTimeDuration` | time (ns) |
| `flameLevelFogRemoval` | whole number 0-255 |
| `tombstoneMode` | 0 AddBackpackMaterials, 1 Everything, 2 NoTombstone |
| `weatherFrequency` | 0 Disabled, 1 Rare, 2 Normal, 3 Often |
| `fishingDifficulty` | 0 VeryEasy, 1 Easy, 2 Normal, 3 Hard, 4 VeryHard |
| `randomSpawnerAmount` | 0 Few, 1 Normal, 2 Many, 3 Extreme, 4 Automatic |
| `aggroPoolAmount` | 0 Few, 1 Normal, 2 Many, 3 Extreme |
| `tamingStartleRepercussion` | 0 KeepProgress, 1 LoseSomeProgress, 2 LoseAllProgress |
| `curseModifier` | 0 Easy, 1 Normal, 2 Hard |

## Settings

Your mod's options: saved for you, and shown as a page in the overlay's **Mod Settings** window.

```lua
local s = Settings.define{
    {key = "god",   type = "bool",   default = false, label = "God mode"},
    {key = "regen", type = "number", default = 0, min = 0, max = 100, label = "Health regen (/s)"},
    {key = "count", type = "int",    default = 1, min = 1, max = 10},
    {key = "mode",  type = "choice", values = {"off", "slow", "fast"}, default = "off"},
    {key = "name",  type = "text",   default = "", max = 64},
    onChange = function(key, value) end,   -- optional
}
if s.god then end
s.regen = 5   -- kept within 0..100, and saved
```

- **Once per mod,** usually in `init.lua`. A reload reads the saved values back.
- **Each entry:** `key` (letters, digits and `_`), `type`, and optionally `default`, `label` (shown in the window,
  else the key), `min` and `max` (for numbers), `values` (for a choice), `max` (for text: its length, 256 if left out).
  Without a default: `false`, `0` (moved into the range), the first choice, or `""`.
- **Values:** a bool is `true`/`false`, a number any number, an int a whole number, a choice one of its strings, a
  text a string.
  - Numbers are kept within their range; text that's too long is cut.
  - The wrong type, `math.huge`, or a choice that isn't listed raises an error, and so does a key you didn't define.
- **`onChange(key, value)`** runs at your mod's next tick after the player changes a value in Mod Settings. Not for
  your own changes.
- **Saved** in `mods\<your mod>\settings.json` within a second, and at once on a reload or when the game closes.
  - Keys in the file your mod doesn't use are kept, so going back to an older version of your mod loses nothing.
  - A value of the wrong type in the file goes back to its default, with a yellow line.
  - A broken file is renamed to `settings.json.bad`, and the defaults are used.
- Your mod can read `settings.json`, but not write it.

## Features

What works on this game build, and what works right now, so your mod can turn its own options off instead of failing:

```lua
local f = Features()
if not f.admin.compatible then print("no admin commands on this build: " .. f.admin.reason) end
if f.hit.available then Hooks.rule{name = "tough skin", target = "hit", ...} end
```

- **`compatible`:** works on this game build.
- **`available`:** usable right now. Admin commands, for example, are compatible at the main menu, but only available
  in a world you host.
- **`reason`:** why one of the two is false, or `""`.
- **`label`:** the name the Compatibility window (in Settings) shows. It can change between versions, so don't
  compare it.
- The ids below never change. A feature that's ever removed keeps its id, with the reason "Removed in x.y".

| Id | What | Unavailable when |
|---|---|---|
| `game_build` | this game build was tested (`GetGameBuild()` gives the number) | an untested build: every other feature still checks itself |
| `resources` | `Res`, the Resources window | not found |
| `english_names` | `Res.text`, English names | not found |
| `game_options` | `GameOptions` | not found, or not yet (they come in after the main menu shows) |
| `field_of_view` | field of view changes apply at once | not found: they need the game's settings menu |
| `systems` | `Systems` | not found |
| `events` | `Observe`, `Events.post` | not found, or not in a world you host |
| `admin` | `Admin.*` | not found, or not in a world you host |
| `admin_give` | `Admin.give` | not found, or not in a world you host |
| `admin_settings` | `Admin.settings`, `Admin.changeSettings` | not found, or not in a world you host |
| `overlay` | `Draw`, `ImGui`, the overlay | the overlay didn't load (administrator, Linux, or a bug: see set.log) |
| `combat_xp`, `hit` | the [catalog targets](#catalog-targets) of the same name | the target didn't check out on this build |

## Mods and files

```lua
GetMod("other")                     -- what mods\other\init.lua returned; true if nothing, nil if not loaded
print(GetVersion())                 -- "0.2.0"
print(GetGameBuild())               -- 1793791052: the running game build
local util = require("util")        -- util.lua or util\init.lua from your mod's folder, loaded once
local f = io.open("save.json", "w") -- files inside your mod's folder only
for line in io.lines("notes.txt") do end
json.encode({a = 1}) json.decode('{"a": 1}')
print("hello")                      -- to the console and set.log
```

- **`require` loops** (`a` needs `b`, which needs `a`) raise an error. Move the shared part into a third file.
- **`json.encode`:** a list becomes an array (`{}` is `[]`), string keys an object. Functions, `math.huge`, NaN,
  tables that contain themselves and mixed tables raise an error.
- **`json.decode`:** `null` becomes nil; broken text raises an error with the position.
- **`load(chunk)`** runs in the shared globals; pass `_ENV` (`load(chunk, name, "t", _ENV)`) to keep a chunk in your
  mod.
- **"write log full"** means your mod changed far more values than any mod should (megabytes of them); the change
  isn't made.

## Native hooks

Advanced. Hooks reach the game's own functions:

- **Rules** (`Hooks.rule`) change a call while it runs: its arguments, its result, or whether it runs at all.
- **Observers** (`Hooks.observe`) tell your mod a function ran, with which values, at the next tick.

The easy way in is a rule on a **catalog target**: a function Shrouded Engine Tweaks studied and checks on every game
build. Anything else needs a **pattern** you found yourself through reverse engineering.

### Catalog targets

Name the target with `target = "<name>"`:

| Target | Arguments | Fields | What changing it does |
|---|---|---|---|
| `combat_xp` | 1 `u32` your level, 2 the victim's level, 3 `f32` the victim's XP modifier (0.8 on the enemies tried), 4 `u32` players sharing the kill | on 2: `level` (`u32`), `isSet` (`bool`; if false, your level counts) | The combat XP you get for one kill: more for a victim above your level, up to a cap. `level` makes the victim count as stronger or weaker (it stops at 0); `mul` on 3 scales the XP; `block = n` gives exactly n (0 to 4294967295); `after = {{mul = 2}}` doubles what the game worked out. |
| `hit` | 4 the hit about to land | on 4: `healthChange` (`i32`, negative is damage); read-only: `targetId`, `rootTargetId`, `sourceId`, `rootSourceId` | One hit, just before the target loses HP. Changing `healthChange` changes the HP lost and the `HitEvent` the game sends about it. Damage is negative, so `max = -50` caps a hit at 50 damage. Halving rounds away from zero: a 1-damage hit stays 1. |

```lua
Hooks.rule{name = "tougher foes", target = "combat_xp",
    when = {{arg = 2, field = "isSet", eq = true}},
    set = {{arg = 2, field = "level", add = 5}}}
Hooks.rule{name = "flat xp", target = "combat_xp", params = {xp = 50}, block = "$xp"}
Hooks.rule{name = "double xp", target = "combat_xp", after = {{mul = 2}, {max = 5000}}}
Hooks.rule{name = "tough skin", target = "hit",
    when = {{arg = 4, field = "targetId", eq = "player"}},
    set = {{arg = 4, field = "healthChange", mul = 0.5}}}
```

A target that didn't check out on this game build makes `Hooks.rule` return nil, with a yellow line; the
Compatibility window and `Features()` say which.

### Rules

```lua
local id = Hooks.rule{
    name = "tough skin",                   -- unique in your mod
    target = "hit",                        -- or pattern = ..., builds = {...}, args = {...} (below)
    params = {k = 0.5},                    -- values you can change later with Hooks.param
    when = {{arg = 4, field = "targetId", eq = "player"}},   -- all must hold
    set = {{arg = 4, field = "healthChange", mul = "$k"}},   -- applied in order
}
Hooks.param(id, "k", 0.25)                 -- the next calls use 0.25
Hooks.remove(id)                           -- the next calls are untouched
```

- **`when`** (conditions): `{arg = n, <op> = value}` with `eq`, `ne`, `lt`, `le`, `gt` or `ge`, or `in = {ids}` (one
  per rule, up to 64 ids; change them later with `Hooks.list(id, ids)`). `field = "name"` checks a field instead of
  the argument itself. A missing value never matches.
- **`set`** (changes): `{arg = n, <op> = value}` with `set`, `add`, `mul`, `min` (at least) or `max` (at most).
- **`block = value`:** the call doesn't run, and returns `value` (`true` for a function that returns nothing). The
  first matching rule that blocks wins; the changes of rules before it are dropped.
- **`after = {{<op> = value}, ...}`:** the call runs, then its result is changed. Conditions are still checked when
  the call starts.
- **Values:** a number, `true`/`false`, `"player"` (your player's entity, so it never matches outside a world you
  host) or `"$name"` (a param). Up to 8 conditions, 8 changes and 8 params.
- **Numbers** are rounded and kept within their type's range: an `i8` given 300 becomes 127.
- **Several rules on one function** run in mod load order, then in the order they were made. Every condition sees the
  original values, and each change works on what the one before left: ×0.5 then +20 gives `x * 0.5 + 20`. Two mods
  changing the same value get a yellow line, and the Hooks window shows it.
- **All or nothing:** a call gets all of its rules' changes or none. If a rule can't read or write its memory, that
  call stays unchanged, the rule turns off with a red line, and only a restart brings it back.
- **A field marked as on the stack** (the catalog says which) is only changed when it really is; otherwise the call is
  skipped and counted under Refused in the Hooks window.
- **Observers** of the same function see the arguments after the rules changed them, and see blocked calls too.
- **Limits:** up to 15 rules per function. Rules can't be made in Draw, the console or `onShutdown`, and a mod can't
  change another mod's rule. A reload, or your mod failing while loading, removes all its rules; the same rule made
  again reuses its slot.

### Your own patterns

A pattern finds a function in the game's exe by its first bytes. You need to find it, and work out its arguments,
yourself: that takes reverse engineering, and has to be checked again after every game update.

```lua
Hooks.rule{name = "more xp", pattern = "48 89 5C 24 ?? 57 48 83 EC 20 ...",
    builds = {1793791052},                 -- the game builds you checked it on; required
    args = {"i32", "skip", "f32", "i32"},  -- every argument up to the last one you use
    set = {{arg = 3, mul = 2}}}
```

- **Pattern:** hex bytes separated by spaces, `??` for any byte, up to 256 bytes. It must match exactly once.
- **`builds`:** on any other build, `Hooks.rule` returns nil with a yellow line, and your mod keeps running. Keep one
  pattern per build and pick it with `GetGameBuild()` ([game updates](modding-guide.md#game-updates)).
- **What your own patterns allow:** observers, and rules that change number arguments. Fields, `block` and `after`
  are for catalog targets only.
- **Arguments:** normal Windows x64 functions, up to 8 arguments, counting a hidden `this` or result pointer. Use
  `skip` for ones you don't need.

| Argument type | You get |
|---|---|
| `i8`, `i16`, `i32`, `i64` | a whole number |
| `u8`, `u16`, `u32`, `u64` | a whole number that can't be negative (a `u64` above 2^63 still reads as negative in Lua) |
| `ptrsize` | same as `u64`: a number, never followed as a pointer |
| `bool` | `true`/`false` |
| `f32`, `f64` | a number with decimals |
| `entity`, `entity*` | an entity id, given directly or through a pointer |
| `cstr` | text, up to 256 bytes |
| `TypeName*` | a copy of a game struct (any fixed-size type `Types.new` can make), up to 1 KiB |
| `skip` | nothing: that position is nil |

### Observers

```lua
local id = Hooks.observe{
    name = "craft attempt",
    pattern = verifiedPattern,
    args = {"entity", "u32", "skip", "f32", "keen::ecs::Health*"},
    queue = 256,                           -- how many calls can wait for the next tick
    fn = function(ev)
        print(ev.seq, ev.time, ev.dropped, ev.args[1])
    end,
}
Hooks.remove(id)
```

- **When:** the game's function runs at once; your `fn` gets a copy of the values at the next tick, before `Tick`
  callbacks.
- **`ev`:** `args` (by position; skipped ones are nil, so don't use `#` or `ipairs`), `seq` (a running number),
  `time` (seconds) and `dropped` (calls lost since the last batch because the queue was full).
- **Queue:** 1 to 4096 calls, 256 if left out. A full queue drops calls; keep your function short. Leaving a world
  throws away calls not handled yet.
- **Copies can be wrong:** values are copied as the function sees them, and an entity id is only a number. A pointer
  that can't be read turns that function's observers off until restart.
- **Rules for registering** are the same as for rules: unique names, not from Draw, the console or `onShutdown`.
- **Limits:** 64 functions in total, 8 observers per function, 64 MiB of queues. A few functions Shrouded Engine
  Tweaks uses itself can't be hooked. Hooks stay installed until the game closes, even when removed; a restart frees
  everything.
