# Architecture

How Shrouded Engine Tweaks gets into the game and what it relies on. Offsets are for the tested builds; everything
address-based is found again at every start, and a part that can't be found turns itself off.

## Threads at a glance

| Thread | What runs there |
|---|---|
| Loader (`DllMain`) | Overlay layer setup, settings, the async compute launch flag |
| Init thread | Logs, reflection, the resource index, waiting for the game window, the hooks, English names |
| Main thread | The game's per-frame options update: game options, and the mods outside a world |
| Server thread | The server world tick: the mods in a world you host |
| Render thread | The Vulkan present: the overlay and mods' Draw callbacks |
| Worker threads | The game's systems, runner profiling and bounded native argument capture; no Lua |
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
  `[W]` (yellow) and `[E]` (red).

## Reflection

The exe carries a static table of all 12,646 of its data types in `.rdata`: names, sizes, fields with offsets, base
classes, enums, and for 1,444 types their default values. A type's id is the FNV-1a hash of its qualified name. It has
no function information. The format is in [reflection-format.md](reflection-format.md).

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
- **The data:** the game loads all 91,812 resources into one block at a fixed address, `0x20000000000` (the
  `GF_Resource` block in `enshrouded.log`), and keeps it until it exits.
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
These hooks are observation only: Lua cannot change arguments, suppress the original call or override its return.

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

## Crash safety

- An access violation in Lua work is caught inside the Lua lock: the mods turn off for good, the game keeps running.
- An access violation in a hook (admin commands, giving, game options) turns that feature off.
- An access violation while drawing turns the overlay off.
- Lua errors are ordinary errors: printed with the mod's file and line, and the failing callback is removed.
