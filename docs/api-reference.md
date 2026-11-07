# API reference

Everything a mod can call. For how mods are put together, see [modding-guide.md](modding-guide.md).

| Area | Functions |
|---|---|
| [Entities](#entities-ecs) | `Ecs.player()`, `Ecs.get(id, "Component")`, `Ecs.each("Component", ...)`, `Ecs.fields(type)`, `Ecs.types(search)` |
| [Events](#events) | `Observe("EventType", fn)`, `Events.post("EventType", {fields})` |
| [Native hooks](#native-hooks) | `Hooks.observe{...}`, `Hooks.remove(id)` |
| [Resources](#resources-res) | `Res.get(guid [, type])`, `Res.each(type)`, `Res.find(type, name)`, `Res.text(guid)`, `Res.types(search)` |
| [Game options](#game-options) | `GameOptions.machine()`, `GameOptions.user()`, `GameOptions.dump([prefix])` |
| [Systems](#systems) | `Systems.list([search])`, `Systems.enable(name, on)`, `Systems.enabled(name)`, `Systems.profile(on)`, `Systems.stats()` |
| [Types](#types-and-values) | `Types.fields(name)`, `Types.enum(name)`, `Types.new(name [, values])`, `Types.copy(view)` |
| [Ticks and lifecycle](#ticks-and-lifecycle) | `Tick.add(fn(dt))`, `registerForEvent(name, fn)`, `reload()` |
| [Keys](#keys) | `Keys.on(key, fn)`, `Keys.hotkey(id, label, fn)`, `Keys.input(id, label, fn(down))` |
| [UI](#ui-imgui) | `Draw.add(fn)`, `ImGui.*`, `Overlay.isOpen()` |
| [Admin](#admin-commands) | `Admin.setTime`, `teleport`, `spawn`, `give`, `setLevel`, `setFlameLevel`, `settings()`, `changeSettings{...}`, `send` |
| [Mods](#mods-and-files) | `GetMod(name)`, `GetVersion()`, `GetGameBuild()`, `require(name)`, `io.open`, `io.lines`, `json.encode`, `json.decode`, `print` |

## Entities (Ecs)

```lua
local p  = Ecs.player()            -- the host player's entity id, or nil
local hp = Ecs.get(p, "Health")    -- a component by short ("Health") or full ("keen::ecs::Health") name; nil if absent
print(hp.dataStorage[0])           -- read a field; arrays are 0-based, like the game's memory
hp.dataStorage[0] = 100            -- write; checked against the field's type and range
for id, hp, f in Ecs.each("Health", "Faction") do end   -- every loaded entity with all of these components
Ecs.fields("Health")               -- print a type's fields with offsets, base classes included
Ecs.types("player")                -- search type names (case-insensitive, first 100)
```

- **Views hold no pointer.** Every access looks the component up again, so a view whose entity is gone raises an
  error the next time it's used.
- **`Ecs.each(name, ...)`** takes 1 to 8 names and gives the id, then one view per name. The entity list is taken
  when the loop starts; `break` works. It covers loaded entities only, the world around you.
- **Health:** `dataStorage` slot 0 is current HP, slot 2 max HP before gear and buffs, so with bonuses current HP can
  be above it: never lower HP to slot 2.
- **Wrapper types read as plain values:** a struct whose only field is `value`, `id` or `index` (`EntityId {id}`,
  `Time {value}`), so `e.deadEntityId` is a number you can compare with `Ecs.player()`.
- **Writes are checked:** a value must fit its field, and an enum field takes only its enum's values, as a number or
  by name (`"None"`, any case). The same holds for resources, options, `Events.post` and `Admin.*`.
- **Whole structs and arrays** can be assigned: a table of fields (`g.windowSize = {x = 1920, y = 1080}` changes only
  those), or another value of exactly the same type. Fixed-size data only: a struct holding strings or variable-size
  lists raises an error.
- **Variable-size game values** (some options' strings and lists) read as `<TypeName>`, never as data.

## Events

```lua
Observe("EntityDiedEvent", function(e)   -- every event of the type, right after each server tick
    print(e.deadEntityId, e.timeStamp)   -- read-only; valid only inside the handler
end)
Events.post("TestEvent1", {data = 42})   -- through the game's own queue; timeStamp is set for you
```

- **`Events.post`** accepts event types the world has a stream for, and fields that exist and fit; anything else
  raises an error and posts nothing. The game handles the event at the start of the next tick. It returns false if
  the game's queue is full.
- **Finished crafts:** `CraftRecipeSuccessEvent` arrives once per successful craft, from the backpack
  (`workshopId` 0) and at a station alike, with the crafted `recipeId`. Failed attempts post nothing, so this needs
  no native hook.

- **Pickups:** `PickupEvent` arrives once per pickup, from ground loot and from harvesting plants alike, with
  `itemId`, `itemCount` (the whole stack), `pickedUpByEntityId` and `pickedUpFromEntityId`.

```lua
Observe("CraftRecipeSuccessEvent", function(e)
    print(e.workshopId == 0 and "backpack" or "station", e.recipeId)
end)
Observe("PickupEvent", function(e)
    print(e.pickedUpByEntityId, e.itemId, e.itemCount)
end)
```

## Native hooks

`Hooks.observe` observes function entry and returns a subscription id. The original function always runs. Lua runs
later, on the normal mod tick before `Tick` callbacks. There are no inline overrides or return-value captures.

```lua
-- verifiedPattern must identify the entry of a function whose signature you have verified.
local id = Hooks.observe{
    name = "craft done",
    pattern = verifiedPattern,
    args = {"entity", "u32", "skip", "f32", "keen::ecs::Health*"},
    queue = 256,
    fn = function(ev)
        print(ev.seq, ev.time, ev.dropped, ev.args[1])
    end,
}
Hooks.remove(id)
```

The declaration above illustrates the API; it is not a verified crafting signature. `pattern` takes whitespace-separated
hexadecimal byte pairs and `??` full-byte wildcards, up to 256 bytes, with at least one exact byte. It must match exactly
once in the executable's `.text`. Malformed, missing and ambiguous patterns raise distinct errors. SET reconstructs
its own patched entry bytes in a private scan copy, so the same pattern can register again after reload.
A pattern and its argument declaration belong to one game build. A game update can move, change or duplicate the
function: the pattern then fails to register, or worse, still matches with a different signature. Verify both again
after every update. Wildcarding call and jump displacements keeps a pattern working longer, never indefinitely. Keep
one pattern per verified build and pick it with `GetGameBuild()`; see
[Game versions](modding-guide.md#game-versions) for `mod.json`.

Only ordinary Windows x64 calls are supported, with at most eight declared argument positions. Count implicit object
and hidden aggregate-return pointers as positions too; use `skip` for unwanted positions. Vectorcall, custom register
conventions, variadic calls and by-value aggregates are not supported. Signature declarations remain the mod author's
responsibility: reflection describes data, not the function's signature.

| Argument declaration | Captured value |
|---|---|
| `i8`, `i16`, `i32`, `i64` | Signed integer of that width |
| `u8`, `u16`, `u32`, `u64` | Unsigned integer bits; `u64` uses Lua's signed 64-bit representation, so values above `2^63-1` appear negative |
| `ptrsize` | Alias for `u64`; an integer only, with no dereferencing capability |
| `bool` | Boolean from the low byte |
| `f32`, `f64` | Floating-point number |
| `entity`, `entity*` | Copied 32-bit entity id, passed by value or through a pointer |
| `cstr` | Copied bytes up to NUL or 256 bytes, with no encoding conversion |
| `TypeName*` | Owned reflected value copied from a fixed-size struct or array supported by `Types.new`, at most 1 KiB |
| `skip` | No read; the corresponding table position is nil |

Pointer declarations produce nil for a null pointer. A non-null unreadable pointer disables observation for the
entire target until restart and discards its pending events. Readable memory can still be inconsistent, from the
wrong lifetime, or semantically wrong. Snapshots are not atomic transactions. An entity id is a copied number, not a
lifetime guarantee or proof it belongs to the current world; only verified target semantics establish that.

Each handler receives an ordinary Lua table with `args`, `seq`, `time` and `dropped`. Argument indexes retain their
declared positions; skipped or null entries are nil, so do not use `ipairs` or `#` to infer the declaration's length.
Tables, strings and owned reflected snapshots may be retained or modified without changing game memory or another
subscriber's copy. `seq` identifies a capture attempt, not gameplay causality. `time` is seconds from a fixed QPC
origin established when observation first initializes, independent of world simulation time.

Each subscriber has its own queue. Capacity defaults to 256 and accepts 1 through 4096. A full queue drops new events.
The first event in the next nonempty delivered batch reports accumulated overflow losses in `dropped`; other events
report zero. Empty drains retain that count. Total drops stay visible in Hooks diagnostics. Removal, reload, faults
and world transitions discard pending work separately from overflow. Each drained batch is sorted by sequence, with
no ordering guarantee across batches or subscribers. All current batches are sealed before callbacks start: events
and registrations created inside a handler wait until a later tick.

Registration is bound to the owning mod's environment. It is rejected from Draw, the console (including exported mod
functions called by the console), and shutdown. Names must be nonempty, at most 128 bytes, without slashes, and unique
among that mod's active hooks. A mod cannot remove another mod's subscription; repeating removal of its own id is
harmless. Removal immediately stops further delivery. A handler error removes only that handler and names the mod and
hook in the log. All subscriptions deactivate before reload/shutdown and when an access violation makes Lua unusable.

One detour is shared by subscribers of the same target. Active declarations at each non-skipped position must agree.
SET reserves its world tick, admin consumer, inventory actions, options update and system runner. Those targets cannot
be observed through this API. Installed detours and their storage remain until process exit, even when inactive.
Identical mod/name, target, argument and capacity declarations reuse their registration after reload. Changed declarations
consume another retained registration. Limits are 64 targets, eight retained registrations per target and a 64 MiB budget
for native queue and delivery storage. Lua copies retained by mods are outside that budget. Exhaustion raises an error;
restart reclaims storage. Pending observations are discarded at world transitions, but registration remains active.

## Resources (Res)

The game's data: every item, recipe, perk, enemy template, loot table and setting it ships, 91,812 resources of 146
types. The game keeps them all in memory from startup to exit, and `Res` reads them in place.

```lua
local item = Res.get("1a2b3c4d-....", "ItemInfo")      -- by GUID and type; nil if none
local any  = Res.get(guid)                             -- the first resource with that GUID
for guid, item, part in Res.each("ItemInfo") do end    -- every resource of a type
local guid, t = Res.find("TemplateResource", "<name>") -- by debugName or name, ignoring case
local guid, logs = Res.find("ItemInfo", "wood logs")   -- or by English name
print(Res.text(logs.name), Res.text(logs.description)) -- English text, or nil
Res.types("item")                                      -- print matching resource types, with counts
```

- **Fields** read like components. Strings are Lua strings, arrays are 0-based with a length (`#item.tags`), an
  optional reads as its value or nil, a variant as its stored type. A reference to another resource reads as its GUID
  string; follow it with `Res.get`.
- **Names:** `debugName` or `name` are the developers' names. The names you see in the game are text references;
  `Res.text(guid)` gives their English text.
- **Editing:**

  ```lua
  local _, logs = Res.find("ItemInfo", "Material_T1_Wood_Logs")
  logs.maxStackSize = 999            -- numbers, bools and enums, checked
  logs.name = other.name             -- references take a GUID string
  logs.exchangeOnBroken = nil        -- nil clears a reference
  ```

  - An edit changes the game's copy in memory and lasts until the game closes. Make it in `onInit` to have it every
    time.
  - A value the game reads when it needs it changes at once. A value it copied at world load changes at the next
    world load.
  - Strings and array lengths can't change. Resources can't be created, cloned or deleted: the game builds its own
    lookup tables at startup, and a world saved with an unknown item drops it.
  - On your own multiplayer server, joined players keep their own unedited copy.
- **Before the game finishes loading,** `Res` raises "resources are not available (yet)".
- **Anywhere:** Draw callbacks included.

## Game options

The game's own settings, live:
- **`GameOptions.machine()`:** this PC's settings (`local_settings.json`): window, graphics, render quality, sound.
- **`GameOptions.user()`:** your user settings: field of view, volumes, camera, gameplay and HUD options.

```lua
local m = GameOptions.machine()
print(m.graphics.render.antiAliasing)            -- enums read as numbers
m.graphics.render.antiAliasing = "None"          -- and take names or numbers
GameOptions.user().gameplay.hideHUD = true
GameOptions.user().graphics.fov = 100            -- applies within a frame, beyond the menu's range too
GameOptions.dump("user.gameplay")                -- print every option under this path; dump() prints all
```

- **A change works the way the game's settings menu applies one.** It lasts until the game closes, or until you
  apply the game's menu. Nothing is saved, and the menu doesn't show it. Make it in `onInit` to keep it.
- **`dump`** prints `machine.graphics.render.antiAliasing = Fsr3 (3) (default Dlss (4))`: the game's default follows
  any value you changed.
- **Never printed:** hosting passwords and banned players.
- **Before the game has its settings,** both raise "game options not found yet". `user()` also raises while no
  user is signed in.
- **Anywhere:** Draw callbacks included.

## Systems

The game runs its gameplay as 861 named systems: `stamina_depletion_sprint`, `durability_loss`,
`fall_damage_infliction`, `random_spawner`...

```lua
for _, n in ipairs(Systems.list("stamina")) do print(n) end   -- names holding the search, sorted
Systems.enable("stamina_depletion_sprint", false)              -- sprinting costs no stamina
print(Systems.enabled("stamina_depletion_sprint"))             -- false
Systems.profile(true)                                          -- start timing, from zero
local list, secs = Systems.stats()                             -- {{name, ms, calls, world}, ...}, costliest first
```

- **Off means skipped entirely,** in every world, for every player and entity, until switched on again or the game
  closes. `reload()` doesn't change it.
- **Some are needed:** switching off network, collider or `*_init` systems can break the world.
- **Only off:** "half the durability loss" needs a data change instead.
- **The profiler** measures CPU time summed over every thread, so it isn't frame time. `ms / secs` is milliseconds
  per second; `world` is `"server"`, `"client"` or `"both"`.
- **Anywhere:** Draw callbacks included.

## Types and values

The game describes all 12,646 of its data types. `Types` gives that to Lua as data, and makes values Lua keeps.

```lua
for _, f in ipairs(Types.fields("ItemInfo")) do print(f.name, f.type, f.offset) end
local aa = Types.enum("RenderAntiAliasingSetting")              -- {None = 0, Fxaa = 1, ...}
local m = Types.new("keen::g38_options::pc::machine::Settings") -- starts from the game's defaults
local saved = Types.copy(GameOptions.machine().graphics.renderPreferences)
GameOptions.machine().graphics.renderPreferences = saved        -- put it back later
```

- **`Types.new(name [, values])`:** a value Lua owns, starting from the game's default for the type (else zeros), then
  `values`.
- **`Types.copy(view)`:** a copy Lua owns of a struct or array.
- Owned values read and write like any view, never go stale, and can be assigned to a field of the same type.
  Fixed-size types only.
- **Names:** a short name works when one type has it; otherwise use the full one.

## Ticks and lifecycle

```lua
Tick.add(function(dt) end)                    -- every server tick in a world you host, every frame elsewhere
registerForEvent("onInit", function() end)    -- see the modding guide for every event
reload()                                      -- restart every mod with a fresh Lua state
```

Events: `onInit`, `onUpdate(dt)`, `onDraw`, `onOverlayOpen`, `onOverlayClose`, `onWorldEnter`, `onWorldLeave`,
`onShutdown`. When each runs: [modding-guide.md](modding-guide.md#lifecycle-events). A callback that errors is removed.

## Keys

```lua
Keys.on("F6", function() end)                        -- a fixed key
Keys.hotkey("heal", "Heal to full", function() end)  -- the player binds the key in the Bindings window
Keys.input("sprint", "Sprint while held", function(down) end)   -- true on press, false on release
```

- **Key names:** F1-F24, A-Z, 0-9, `` ` ``, Insert, Home, End, Delete, PageUp, PageDown, Pause, or `0xNN`.
- **Hotkeys and inputs** exist only inside mods. `id` takes letters, digits and `_` and is unique in your mod;
  `label` is what the Bindings window shows. An input's release also comes when the game window loses focus or the
  overlay opens. `registerHotkey` and `registerInput` are the same functions.
- Keys fire only while the game window has focus and the overlay is closed.

## UI (ImGui)

```lua
Draw.add(function()                          -- every frame, on the render thread
    if not Overlay.isOpen() then return end  -- leave this out to show the window over the game (no input then)
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
- **Scrolling regions:** `BeginChild(id [, w, h [, border [, flags]]])` (0 = fill the space left), then always
  `EndChild()`.
- **Text and layout:** `Text`, `TextColored(r, g, b, a, text)`, `Separator`, `SameLine`, `Spacing`.
- **Widgets:** `Button(label [, w, h])`, `Checkbox`, `SliderFloat`, `SliderInt`, `InputFloat`, `InputInt`,
  `InputText(label, text, maxLen)`.
- **Tooltips:** `IsItemHovered`, `SetTooltip`.
- **Constants:** `ImGuiCond.{Always, Once, FirstUseEver, Appearing}`, `ImGuiWindowFlags.{None, NoTitleBar, NoResize,
  NoMove, NoScrollbar, NoCollapse, AlwaysAutoResize, NoBackground, NoSavedSettings, NoInputs}`.
- `ImGui.*` works only inside Draw, and Draw can't touch the game (see the modding guide).

## Admin commands

The game's own admin commands, run as you, the host:

```lua
Admin.setTime(12)                          -- time of day in hours, 0 to below 24; 12.5 = 12:30
Admin.teleport(x, y, z)                    -- meters; the game picks a safe spot near the target
Admin.teleport(x, y, z, true)              -- exactly there
Admin.setLevel(20)                         -- your character level, 1 to 255
Admin.setFlameLevel(3)                     -- the world's flame level, saved with the world
Admin.spawn("Enemy_Wildbeast_wolf_earlygame")   -- a template by name or GUID, 5 m from you; optional count, level
Admin.give("Wood Logs", 75)                -- items into your inventory; optional level and rarity
Admin.changeSettings({enemyDamageFactor = 0.5}) -- world settings, see below
Admin.send("adminSetDaytime", {syncDaytime = 6 * 3600 * 1000000000})   -- raw: any command in the table
```

- **Calls queue,** and run at the next server tick. A second call for the same command before then replaces the
  first (game settings merge instead).
- **Where:** in a world you host, from anywhere, Draw included. `Admin.settings()` and `Admin.spawn` read the game,
  so not in Draw. Outside a world they raise "not in a world you host".
- **Your position:** fixed point, meters × 2³², y up:
  `local pos = Ecs.get(Ecs.player(), "CurrentTransform").transform.position print(pos.x / 2^32)`.
- **`Admin.give(item [, count [, level [, rarity]]])`:** an `ItemInfo` by GUID, `debugName` or English name. Count 1
  to 999, level 1 to 255, rarity a name (`"Legendary"`) or 0 to 4. The game makes one item or one full stack per
  command, so the runtime sends one per tick: 75 logs at stack size 50 take 26 ticks, about a second.
- **`Admin.spawn(template [, count [, level]])`:** a `TemplateResource` by GUID or name (enemies are `Enemy_...`).
  Count 1 to 255, level 1 to 65535.
- **Raw commands:** `Admin.send(command, fields)` fills the game's command by field name, checked like
  `Events.post`. Fields left out are 0. A raw teleport needs `orientation = {w = 1}`.
- **A command the game didn't run** logs `admin command <name> was not run by the game`.

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

Not available: kill faction (release builds don't run the system that reads it), destroy item and perk (they need
inventory slot ids).

### World settings

```lua
local s = Admin.settings()                 -- the current settings as a table; nil until the world's first tick
Admin.changeSettings({enemyDamageFactor = 0.5, weatherFrequency = 1})   -- only these change
```

- Only what you name changes; the rest keeps its current value.
- Saved with the world, like a change in the game's own menu, for everyone on the server.
- Times are nanoseconds: 30 minutes is `30 * 60 * 10^9`.
- `miningDamageFactor` is stored but the game doesn't use it.

| Setting | Type |
|---|---|
| `playerHealthFactor`, `playerManaFactor`, `playerStaminaFactor`, `playerBodyHeatFactor`, `playerDivingTimeFactor` | float |
| `enableDurability`, `enableStarvingDebuff`, `enableGliderTurbulences`, `pacifyAllEnemies` | bool |
| `foodBuffDurationFactor`, `shroudTimeFactor`, `miningDamageFactor`, `plantGrowthSpeedFactor` | float |
| `resourceDropStackAmountFactor`, `factoryProductionSpeedFactor`, `perkUpgradeRecyclingFactor`, `perkCostFactor` | float |
| `experienceCombatFactor`, `experienceMiningFactor`, `experienceExplorationQuestsFactor` | float |
| `enemyDamageFactor`, `enemyHealthFactor`, `enemyStaminaFactor`, `enemyPerceptionRangeFactor` | float |
| `bossDamageFactor`, `bossHealthFactor`, `threatBonus` | float |
| `fromHungerToStarving`, `dayTimeDuration`, `nightTimeDuration` | time (ns) |
| `flameLevelFogRemoval` | integer 0-255 |
| `tombstoneMode` | 0 AddBackpackMaterials, 1 Everything, 2 NoTombstone |
| `weatherFrequency` | 0 Disabled, 1 Rare, 2 Normal, 3 Often |
| `fishingDifficulty` | 0 VeryEasy, 1 Easy, 2 Normal, 3 Hard, 4 VeryHard |
| `randomSpawnerAmount` | 0 Few, 1 Normal, 2 Many, 3 Extreme, 4 Automatic |
| `aggroPoolAmount` | 0 Few, 1 Normal, 2 Many, 3 Extreme |
| `tamingStartleRepercussion` | 0 KeepProgress, 1 LoseSomeProgress, 2 LoseAllProgress |
| `curseModifier` | 0 Easy, 1 Normal, 2 Hard |

## Mods and files

```lua
GetMod("other")                     -- what mods\other\init.lua returned; true if nothing, nil if not loaded
print(GetVersion())                 -- "0.1.1"
print(GetGameBuild())               -- 1793791052: the running game build, as Status shows it
local util = require("util")        -- util.lua or util\init.lua from your mod's folder, once per mod
local f = io.open("save.json", "w") -- files inside your mod's folder
for line in io.lines("notes.txt") do end
json.encode({a = 1}) json.decode('{"a": 1}')
print("hello")                      -- to the console and set.log
```

- **`require` cycles** (`a` requires `b`, which requires `a`) raise "require cycle: 'a' is still loading" instead of
  recursing. Move the shared part into a third module.
- **`json.encode`:** tables with keys 1..n become arrays (`{}` is `[]`), string keys become objects with sorted keys.
  Functions, cycles, NaN and mixed tables raise an error.
- **`json.decode`:** `null` becomes nil; bad text raises an error giving the position.
- `load(chunk)` runs in the shared globals; pass `_ENV` (`load(chunk, name, "t", _ENV)`) to keep a chunk in your mod.
