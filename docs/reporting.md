# Reporting a bug

Report on **GitHub** ([bug report form](https://github.com/Cacaida/Shrouded-Engine-Tweaks/issues/new/choose)) or on
**Nexus Mods** ([Bugs tab](https://www.nexusmods.com/enshrouded/mods/303?tab=bugs)). Both are read. A report with the
log in it can usually be answered; one without it usually can't.

## 1. Get the log

- **After a crash:** At the next start, a **Last Session** window opens by itself. It shows what the game's own crash
  report says, and **Copy to Clipboard** copies that session's log. **Dismiss** goes back to the game.
- **Any time:** Open the overlay, **Settings**, **Bug Report**. **Copy to Clipboard** copies this session's log,
  **Copy last session's logs** the one before.
- **The overlay doesn't load:** A Windows message box says why at start (and says so if the last session crashed).
  **Yes** copies the log, or both logs after a crash.
- **By hand:** `<game folder>\shrouded_engine_tweaks\set.log`. After a crash, the restart moved that session's log to
  `set.prev.log`.

The copied text shows your Windows user folder as `%USERPROFILE%`; the file itself is unchanged. The log already says
the Shrouded Engine Tweaks version, the game build, the Windows build, and your GPU & driver.

## 2. For overlay problems: The debug logs

**Debug Logs** in **Settings** (`debugLog=1` in `settings.ini`) is on by default: It adds lines about the overlay key
and drawing to `set.log`, not to the console. If you turned it off, turn it on again, **Save**, and do what went wrong
once more before copying the log.

## 3. Write the report

- What you did, what you expected, what happened instead.
- Windows, or Linux / Steam Deck.
- Whether Steam or the game runs as administrator.
- Other mods and overlays: Steam, Discord, RivaTuner / Afterburner, ReShade, DLLs in `shrouded_engine_tweaks\plugins\`.
- The log, pasted. On GitHub it goes in the **set.log** field. If it's too long for a Nexus report, post on GitHub,
  which takes the whole file.
