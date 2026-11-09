# Architecture

How Shrouded Engine Tweaks gets into the game and what it relies on. Offsets are for the tested builds; everything
address-based is found again at every start, and a part that can't be found turns itself off.

## Threads at a glance

| Thread | What runs there |
|---|---|
| Loader (`DllMain`) | Overlay layer setup, settings, the async compute launch flag |
| Init thread | Logs, reflection, the resource index, waiting for the game window, the hooks, English names |
| Main thread | The game's per-frame options update: game options, and the mods outside a world |
| Server thread | The server world tick: the mods in a world you host, and the entity inspector's reads and edits |
| Render thread | The Vulkan present: the overlay and mods' Draw callbacks |
| Worker threads | The game's systems, runner profiling, the read point check, and bounded native argument capture; no Lua |
| Log watcher | Tails `enshrouded.log` for the leave and shutdown lines |

One mutex keeps all Lua apart, so only one thread runs Lua at a time. The render thread only tries it: while a tick
holds it, the present shows the previous overlay frame instead of waiting. A Tick or Draw callback over 50 ms gets a
yellow line naming its mod, at most once per 10 s.

## Loading

- `enshrouded.exe` imports `POWRPROF.dll` (two functions). It isn't a Windows KnownDLL, so a copy in the game folder
  loads first.
- The DLL forwards all 126 of the system `powrprof.dll`'s exports to
  `\\.\GLOBALROOT\SystemRoot\System32\powrprof.dll`, which works wherever Windows is installed (`runtime/exports.h`).
- In any other process it does nothing.
- **Plugins:** `DllMain` loads every `.dll` in `shrouded_engine_tweaks\plugins\` in name order, so other mods that
  ship as `powrprof.dll` still run. Their `DllMain` runs under the loader lock, as it would have as the game's import.
  The exe only calls SET's forwarded exports, and a plugin that loads `powrprof.dll` by name gets SET, which forwards
  to System32 too. Results are logged once set.log is open.
- The exe has no anti-cheat and no packer. A SteamStub wrapper covers only the entry point, so the init thread waits
  for the game window before patching any code.
- Hooks use MinHook.

## Finding code

- **Byte patterns:** the world tick, the admin consumer, the options update, the camera push and the system runner
  are each found by a prologue pattern that must match exactly once in `.text`.
- **Clean code:** patterns match `.text` as the exe file holds it, read once (20 MB) with the loader's relocations
  applied, not the live code. A detour another mod already put on a target (a plugin's MinHook) doesn't hide it, and
  MinHook chains SET's detour in front of that one. If the file isn't the running build, the live code is scanned.
- **System names:** the game's system descriptors in `.rdata` are `{char* name, u64 length, fn}`. A system's
  function is found by its name (`inventory_actions`), which survives patches that move code.
- **The game build** is the exe's link time stamp. Builds that were tested are listed in `game.h`; on any other the
  build line and its Status row are yellow, and each feature still checks itself.
- **Every feature is optional:** one not found turns off with a log line and a Status row; the rest keeps working.
- **Output levels:** each runtime line and Status row is green (as expected), yellow (works, but unexpected or
  untested) or red (not found, failed, an error). Prints and dumps stay plain. A line carries its level as a leading
  marker byte (`level.h`); the sink strips it, colors the overlay and the console window, and tags set.log lines
  `[W]` (yellow) and `[E]` (red). A line marked `LV_LOG` goes to set.log only: debug lines (`ov::debug`), entering
  and leaving a world, the overlay opening and closing.
- **Wording:** every line SET writes reads `Label: Value`, the label in Title Case and the first word after each colon
  capitalized; identifiers, file names and code keep their case. Lua errors raised to mods keep Lua's own style.
  `rep::crashed` accepts the old and the new spelling of the lines it looks for, since the first start after an
  update reads a log the previous version wrote.

## Reflection

The exe carries a static table of all its data types in `.rdata` (12,658 on build 1793791052): names, sizes, fields
with offsets, base classes, enums, and for some types (1,444 on build 1792148057) their default values. A type's id
is the FNV-1a hash of its qualified name. It has no function information. The format is in
[reflection-format.md](reflection-format.md).

Everything Lua sees by name comes from this table, read again at every start, so layouts follow the game through
patches.

## ECS world and the tick

- **The world tick** is hooked. A world's kind byte at `world + 8` is 0 for the server and 2 for the client. Lua runs
  only for the server world: right before its tick (queued lines, `Tick`, keys, admin commands) and right after it
  (`Observe`).
- **Components:** the world's schema (`*world`) maps a type hash to a component index (`+0x88`). The entity map at
  `world + 0x148` is a hash map from entity id to a record; the record's archetype chunk holds each component at
  `chunk + offset[idx] + row * stride[idx]`.
- **Views re-resolve** on every access, so an entity that moved chunk or lost a component can't leave a stale
  pointer.
- **Sessions:** a new world, or ticks resuming after a pause with the sim time started again, is a new session:
  `onWorldEnter`.

### The read point

Lua and the inspector read and write a world's components right before and after its tick, on the server thread.
That is safe only because no system of that world runs then. Build 1793791052:

- **Statically:** the system runner has three callers. Two (`exe+9acf40`, `exe+9ade50`) are called only by the world
  tick; the third is the job entry (`exe+9eebb0`) that only `9ade50` hands to the task system. The tick's other
  parallel path (`exe+9af3d0`, also called only by the tick) runs each stage's jobs through the same task system. Both
  wait in `exe+8f0b30` before returning, and that returns only once the task system has nothing queued (`+0x1a0`)
  or running (`+0x1c0`).
- **At runtime:** the runner hook checks it on every system call. `rt::tick` and `rt::afterTick` make a generation
  counter odd while they hold the world; a system of that world that starts while it's odd, or sees it change before
  it ends, is an overlap. An overlap logs one red line and marks the Read Point Status row. None were seen in play.

### Entity ids

- **An id is not a lifetime.** Entities spawned at runtime get ids from counters in the world (`world+0x4a0`, and
  `+0x498` for creations that bring no id), only ever incremented and restored when a save loads, so those don't come
  back. Placed world entities have fixed ids (a block prefix in the high half, an index in the low half) and return
  under the same id, as new objects, when their area streams back in.
- **The record** (the entity map's value) holds the id at `+0x10`. The dword at `+0x14` is shared by many entities and
  doesn't change when an id returns; no field in the record's first 0x40 bytes marks a lifetime.
- **So identity is presence continuity:** the inspector checks its selected and pinned ids at every tick, and one that
  is missing once is gone for good. What it can't see is an entity removed and re-created under the same id inside a
  single tick.

## Events

- **Where:** the schema's event map (hash to event index) and the world's streams move between game builds, so they
  are read at startup from a function of the game that loads both (`ecs::findEvents`). Build 1793791052: map at
  `schema + 0xe16db8`, streams at `world + 0x7b9e80`, pending events at `+ 0x70` from there.
- **Reading:** each event type has a stream, holding this tick's events in linked pages until the session clears them
  after the tick. `Observe` handlers run in between.
- **Posting:** `Events.post` appends a record to the session's pending-event buffer, which the
  game distributes at the start of the next tick. A type hash the world doesn't know would crash the distributor, so
  it is checked first.
- **Time:** sim time is the tick length times the tick index (`world + 0x4c0`, `+ 0x4dc`).

## Admin commands

- The game's `admin_player` system runs a command when the player's `ServerConsumedPlayerInput` counter differs from
  the command's version in `PlayerInput.fromAdminClient`, then sets the counter to the version.
- The game rebuilds `PlayerInput` inside the tick, so the hook right before the consumer writes queued commands then.
  It moves the counter, not the version, so the next rebuild leaves the pair equal and the command fires once.
- Giving items runs through a second system, `inventory_actions`, hooked the same way.
- **World settings:** the consumer's query holds the `ecs::GameSettings` resource (slot 13). A change is merged into
  the current settings before the game applies it.

## Resources

- **The index** comes from `enshrouded.kfc` (KFC4), read at startup: GUID, type hash and part to an offset and size.
- **The data:** the game loads all its resources (91,909 on build 1793791052) into one block at a fixed address,
  `0x20000000000` (the `GF_Resource` block in `enshrouded.log`), and keeps it until it exits.
- **Layout:** resources are blobs: variable-size values sit after the struct, reached through offsets relative to
  the field, all inside the resource.
- **Writes:** the game makes the pages read-only after loading, so each write lifts the protection for its own bytes
  and puts it back.

## English names

- The index's content map points at the `enshrouded_<n>.dat` file and offset holding the En_Us text table of the
  `LocaTagCollectionResource`. It's read once, about 1.7 MB.
- A text reference's id is FNV-1a over its 16 GUID bytes.
- The game streams resource data in for about 1.5 s after its window shows, so the table is looked for again for up
  to 30 s.

## Game options

- **The hook:** the game's per-frame options update on the main thread. The options object sits at
  `app + 0x128`: machine settings at `+0x9c0`, user settings in 16 slots. The slots grow with the settings, so their
  start, size and has-value byte are read at startup from the game's user-slot getter (`opts::findLayout`); build
  1793791052: from `+0xa70`, `0x310` each.
- **Changes:** the game reads these every frame, and its own menu applies a change by copying it in, so a write
  applies the same way. The game never saves them.
- **Field of view:** the camera keeps its own copy, refreshed only by the menu's camera push. A changed value is
  handed over through that same push.
- **Antialiasing off** keeps the machine setting at None, which the menu doesn't offer.
- **Defaults** come from each settings type's twin outside `keen::ds` in the reflection table.
- The same hook runs the mods' frame outside a world: they load here at the main menu.

## Systems

- The game's 861 systems all run through one program runner, called with `(world, job, index, slot)` on whichever
  thread the job system picks; the system's update is at `*(job + 0x10) + 0x28`.
- The runner hook skips a job whose system is switched off. With nothing off, the check is one atomic load.
- The profiler times each call with the performance counter, per system and world kind.
- Systems run side by side on worker threads, so Lua can't run inside one.

## Native observation

`hooks.cpp` and `hooks_x64.asm` implement entry observation for ordinary Windows x64 calls, with eight argument
positions including implicit arguments. A target-specific thunk supplies the context in RAX. The common stub uses
a fixed aligned frame with MASM unwind metadata, saves integer and XMM argument registers, captures, restores them,
and tail-jumps to MinHook's original trampoline. It does not preserve arbitrary register-input conventions. Native
returns remain the original function's responsibility; observation does not intercept them.

Registration is serialized. Context, declarations and queue buffers are initialized before the detour is enabled.
SET reserves its own five detour targets before mods load. Each observation target has one permanent detour, shared
by subscribers. Removal only deactivates a subscriber; detours, trampolines and contexts remain until process exit.
Pattern scanning uses the clean copy of `.text` (see Finding code), so retained patches, SET's or another mod's, do
not break reload lookup. Only if the file can't be used does it fall back to the live code with SET's saved original
entry bytes overlaid.

Capture briefly locks metadata to copy active declarations and generations, then decodes their compatible union once
into bounded stack storage. Pointer and stack reads run under small SEH guards outside queue locks. Capture performs
no heap allocation, logging or Lua calls. An invalid read atomically disables the target, and control-thread work
discards pending observations and reports the fault. The original still runs.

Each subscriber owns two preallocated event/payload buffers. Producers append and the single consumer swaps buffers
under the same SRWLock. The consumer copies its sealed buffer and sorts by sequence outside the lock. Generation
checks at publication and immediately before invocation reject work from before removal, reload or a world transition.
All Lua batches are frozen before the first handler runs, then delivered before ordinary Tick callbacks. Delivery is
counted only when invoking a handler; overflow and lifecycle discards have separate counters.

Stable mod/name keys reuse identical target, argument and queue declarations after reload. Changed declarations use
new retained slots. Limits are 64 targets, eight registrations per target, eight arguments, 256-byte strings, 1 KiB
snapshots and queue capacity 1..4096. The 64 MiB budget accounts for both native buffers and one delivery copy per
registration. Lua-owned copies retained by mods are separate. Diagnostics retain at most 32 recent refusals/faults.
Optional capture timing is off initially and uses a separate timed-call count.

Readable snapshots can still race with game writes or describe the wrong object. Reflection validates copy layout,
not function semantics. Entity ids carry no world or lifetime proof. Queue capacity deliberately allows event loss.

## Interception rules

`rules.cpp` applies rules; `hooks.cpp` stores them on the same detour as observers. The common stub calls
`hk_entry`, which applies the target's rules to the saved frame, then captures for observers. Normally the stub
then restores registers and jumps to the original. If a rule blocked the call, `hk_entry` leaves the return value in
the saved context slot (RAX) or the saved XMM0, and the stub loads those two, frees its frame and returns to the
caller itself: it touched only volatile registers and its own frame, and an x64 callee pops no arguments, so the
caller sees an ordinary return.

If a matching rule changes the return value, `hk_entry` records the entry's rule order and which of its rules
matched in the frame. The stub then copies stack arguments 5 to 8 into its own outgoing area (slots past the real
arity only read the caller's frame), restores the registers and `call`s the original instead of jumping to it.
When the original returns, `hk_after` applies those rules' changes to RAX or XMM0, and the stub returns to the
caller. The stub's frame is fixed (0xE8 bytes, described by its unwind data), so a C++ exception or an access
violation inside the original unwinds through it to the caller's handlers. The original sees the stub as its return
address and runs about 0x100 bytes deeper, which the catalog entry's RE has to rule out as a problem.

- **Thread contract:** rules run on the calling thread, take no lock, allocate nothing and call no Lua.
- **Fixed shape:** a rule's conditions and changes are fixed at registration. Params are 64-bit atomics; the id
  list sits behind a sequence counter, and a call reading it mid-update treats `in` as not matching. Nothing a call
  can be reading is ever replaced.
- **Order:** a target's active rules sit in one 64-bit word (15 four-bit slot indexes and a count), sorted by mod
  load order, then registration. Registering or removing a rule stores a new word, so a call sees the old order or
  the new one.
- **Stage, then commit:** the declared positions are copied from the saved registers (XMM for float positions) and
  the caller's stack slots. Conditions read the original copy and the original memory. Changes apply in order to a
  second copy of the arguments and to staged copies of the fields (each staged with the value memory held before any
  rule ran). After every rule ran, the changed fields are written; if a write faults, the ones before it get their
  old values back. Only then are the changed positions written back to the frame. Those writes can't fault, so a
  call gets all of its changes or none.
- **Field writes** exist only on catalog targets, for the fields the entry lists. A field marked as on the caller's
  stack is written only if, at that call, its bytes lie between the calling thread's `StackLimit` and `StackBase`
  (two loads from the TEB). Otherwise that rule doesn't apply to the call at all and counts as refused. A field the RE
  found owned by the calling thread but not on its stack skips that check; an entry lists at most one such field, so
  no call changes two fields another thread could see.
- **Return changes** exist only on catalog targets whose entry allows `after` (never `void` or a hidden return
  pointer). A rule's storage is never freed, so a rule removed while a call is inside the original still finishes
  that call.
- **Blocking** exists only on catalog targets whose entry allows it; the entry's return kind decides the value
  (integers sign- or zero-extended to RAX, `f32`/`f64` in XMM0, nothing for `void`; a hidden return pointer is never
  blockable). The first matching blocking rule ends the call before anything is committed.
- **Faults:** a faulting field read or write turns that rule off; the control thread logs it and drops it from the
  order.
- **Retention:** rule slots, like observer registrations, are kept until exit. Removal clears the slot's active flag
  and republishes the order; an identical rule registered again reuses its slot.
- **Catalog** (`contracts.cpp`): functions the runtime verified itself, each with its full signature, return kind,
  allowed capabilities, its fields (writable, or read-only for conditions) and, per build, a pattern plus anchors
  (byte checks inside the function). Each entry is checked at startup; one that fails is off and the rest keep
  working. `runtime_selftest --patterns` also reports which listed build's contract still matches a new exe. Fields
  are described by the RE (offset and number kind), not by reflection: the structs these functions take often have no
  reflected type.
- **`combat_xp`:** the combat XP one player gets for one kill,
  `u32 (u32 playerLevel, Optional<u32>* victimLevel, f32 xpMod, u32 players, context*)`; `xpMod` is the victim's
  `ExperienceSource.xpMod`. The levels and the player count are zero-extended into 64-bit converts, so the catalog
  declares them `u32`: a negative level would read as about 4.29 billion and hit the XP cap. A leaf: no calls,
  locks or writes. Its only caller, `combat_experience_source`, passes argument 2 as `lea rdx, [rsp+40h]`, a slot of
  its own frame that it rewrites before every call and never reads after one, so a change lasts for that call only.
  The function reads it at `[rdx]` (the level) and `[rdx+4]` (whether it's set; if not, the player's level is used).
  Anchors: those two reads and the final `ret`. Blocking is allowed: nothing else happens inside the function, and
  the caller only adds the returned `eax` to the player's XP and posts it in an `XPGainedEvent` (the function returns
  `rax`, but only `eax` is kept, so the catalog declares a `u32` return and rules stop at 0 and 4294967295). Return
  changes are allowed: the function makes no calls, so nothing in it reads its return address or depends on stack
  depth.
- **`hit`:** one hit landing, `void (bool* out, attributes* target, context*, PendingHit* hit)`. Its only caller,
  `impact_hit_conclusion`, copies each pending hit into its own frame and passes it as `lea r9, [rbp+240h]` (all three
  call sites, every listed build). The first 0xF8 bytes of that copy are a `HitEvent`: `targetId` +0x94,
  `rootTargetId` +0xA0, `sourceId` +0xA4, `rootSourceId` +0x90, `healthChange` +0xBC (`i32`, negative is damage).
  The function reads `healthChange` once, adds it to the target's health attribute (`[attr] += delta`) and only then
  copies it into the `HitEvent` it posts. A change reaches the HP and the event alike. The entity ids are read-only:
  the caller looked the target up before the call, and nothing shows what changing them would do. Anchors: the
  `healthChange` read, the `HitEvent` post (hash, size) and the copy into it. No block or return changes: the
  function also applies the stamina change and posts the event.
- **Why not a rule on `HitEvent` itself:** the event is written after the damage is applied, so changing it changes
  what observers see but not the HP lost. `impact_hit_conclusion` posts it; `impact_system_prepare` re-posts
  HitEvents it decodes from a stream.

## Leaving and closing

A thread tails `enshrouded.log`:
- `[game_server] Switching state from Run to Shutdown`: you left the world, `onWorldLeave`.
- `[GameFramework] Shutdown '`: the game is closing, at least 2 s before it frees its options and resources, so
  `onShutdown` runs with files only.

## The overlay

- **A Vulkan layer in the same DLL.** At load it writes a layer manifest and sets `VK_ADD_LAYER_PATH` and
  `VK_LOADER_LAYERS_ENABLE` for this process only, so the loader puts it into the game's Vulkan chain. Nothing is
  installed system-wide.
- **Drawing:** at each present it records an ImGui frame into the presented image with its own render pass and
  submits it after the game's work. A frame with nothing to draw passes through untouched. Mods' Draw callbacks run
  first, under the Lua lock, then the built-in windows without it. If the lock is taken, no frame is built and the
  last one is drawn again.
- **Input:** a subclass of the game window. While the overlay is open, keyboard and mouse go to ImGui and never reach
  the game's window procedure.
- **Overlay key:** asked for at a start while `overlayKey` in settings.ini is empty, so a start that ended before a
  key was picked asks again. A start hint names the key for 5 s, drawing frames while the overlay is closed.
- **Frame rate cap:** each present waits for its slot.
- **Async compute:** the exe's `GetCommandLineA/W` imports are pointed at copies with the game's own launch option
  added, before the game's code runs.

## Write log

`runtime/writelog.cpp`. What mods change in resources, game options and systems, so it can be undone when they stop.

- **What's logged:** `put()` in `lua_api.cpp` (every Lua value write) logs a resource ref (`raw && gen == 0 && !plain`)
  and an options ref (`raw && gen == 0 && plain && !owned`); `Systems.enable` logs the system's on/off. Event and admin
  scratch buffers, Lua-owned values and components aren't.
- **Who wrote it:** the active mod. `call()` sets it from the function it calls, for init chunks and every callback.
  Each registration (`Tick.add`, `Draw.add`, `registerForEvent`, `Keys.*`, `Observe`, `Hooks.observe`) records the mod
  running when it was made in a weak table, so a function built with `load()` (source "load") still belongs to its mod,
  and a failing mod's cleanup (`dropModFns`) finds it. The console runs as no mod and isn't logged.
- **One record per value, not per mod:** the game's original bytes (saved before the first logged write, never
  replaced), the latest logged bytes, and the mods that wrote it. A write overlapping records merges them into one.
  Records are kept by address in a map and never overlap.
- **Undo** when the last writer leaves: current bytes equal to the latest logged ones get the original back
  (resources through `res::write`); equal to the original, nothing; anything else was changed outside the log and stays.
  Options are checked to still be the machine or the current user's settings first.
- **When:** in the reload branch after `onShutdown` (all mods leave), in `unloadMod` (a mod failed while loading;
  values other loaded mods wrote stay until they leave), and on the access-violation path, C++ only. Never from
  `closeState` at game exit.
- **Budget:** 16 MiB of saved bytes. A write that needs a new record past it fails with a Lua error instead of
  happening unlogged.
- Everything runs under the Lua lock.

## Mod settings

`runtime/modsettings.cpp` (`mset`) holds the values, `settingsDefine` and friends in `lua_api.cpp` the Lua side.

- **The values live in C++,** one page per mod under a mutex, so the Mod Settings window reads and edits them from the
  render thread without the Lua lock. The table `Settings.define` returns is empty; its metatable reads and writes the
  store.
- **Overlay edits** queue (mod, key) once; `tickMods` hands them to the mod's `onChange` through `call()`, so what it
  changes counts for that mod in the write log.
- **Reading the file:** with the mods' `json.decode`, in `define`. Keys the page doesn't define are kept as JSON text
  (`json.encode`) and written back.
- **Saving:** pages are marked changed. The end of `tickMods` saves at most once a second; `closeState` (reload and game
  exit, after `onShutdown`) and `unloadMod` save at once and drop the pages; the access-violation path saves too, C++
  only. The text is built under the mutex and written outside it, to `settings.json.tmp`, then swapped in with
  `MoveFileEx`. Only the Lua side writes, under the Lua lock, so two saves never race. A failed save stays marked and
  is reported once.
- **Write refusal:** the mods' `io.open` refuses write modes on `settings.json` and its `.tmp` at the top of the mod's
  folder, after dropping trailing dots and spaces as Windows does.

## Entity inspector

`runtime/inspect.cpp` (`insp`) feeds the Entities window; the window draws only its copies.

- **When:** right after `rt::tick` in the world-tick hook, at the read point, for the world Lua had that tick. It has
  its own access-violation guard: a fault turns the inspector off, not the mods.
- **Every tick:** the selected and pinned ids are looked up in the entity map. One that is missing is gone for good
  (see Entity ids). A new session marks everything gone.
- **While the window is open:** the list is one walk of the entity map, a slice per tick (0.25 ms of work, every
  0.5 s), keeping the 200 nearest to the player. The selected entity is refreshed every 100 ms: the components it has
  come from its archetype bitmask, their types from the schema's component map (read once per session), and only
  expanded ones are copied, within 256 KiB per refresh. The copying starts at a different component each time, so
  one past the budget keeps its last copy and is refreshed later. Pins are read every 100 ms even while it's closed.
- **Publishing:** a new `View` behind a `shared_ptr`, swapped under a mutex that is never held while game memory is
  read.
- **Edits** are requests: the selection they were made on, component type, offset, a number, bool or enum type, the
  value the window showed and the new one. The next tick applies one only if the selection is still that one and was
  present at every tick since, the entity still has the component, and the field still holds the shown value. The
  offset comes from the reflection walk of that same component type, so the type check covers the field. Edits are
  the user's own, like console edits: not in the write log.
- **Field trees:** the Resources and Entities windows share one tree (`drawValue`): a resource is read and written in
  place, a component copy sends edit requests.

## Crash safety

- An access violation in Lua work is caught inside the Lua lock: the mods turn off for good, the game keeps running.
- An access violation in a hook (admin commands, giving, game options) turns that feature off.
- An access violation while drawing turns the overlay off.
- Lua errors are ordinary errors: printed with the mod's file and line, and the failing callback is removed.
