# SagaTweaks

Quality-of-life ASI mod for **LEGO Star Wars: The Complete Saga** (Steam, `LEGOStarWarsSaga.exe`,
5,472,256 bytes). Built for Proton on a handheld (Steam Deck / ROG Ally) but nothing in it is
Linux specific.

## Features

| Area | What it does |
|---|---|
| Startup | Skips boot logos and the "Press any button" title screen (straight to the main menu). |
| Cutscenes | **Start skips a cutscene** and every following part of the chapter intro, straight to gameplay. |
| Frame rate | Removes the 85 fps cap, runs at the desktop refresh rate (shown in Video Settings). |
| Frame-rate fixes | Clock counters, blink timers, grass and foliage animation, collision response and jumps behave as at 60 fps (same jump height, jumps from platforms and over the fallen tree work, at 60 and 1000 fps). |
| Display | **VSync** and **Window Mode** (fullscreen / windowed / borderless) rows in Video Settings. |
| Render scale | 50-300 % internal resolution, native row in Video Settings. The game itself runs at the larger size (windowed or borderless; the Fullscreen choice is skipped while the scale is not 100 %) and the picture is scaled to the window on present. |
| FSAA | Row removed from Effects Settings: render scale supersamples the whole frame and replaces it. |
| Input | Player 1 hot-switches keyboard / gamepad. A second gamepad joins as player 2 with **Start**; lost pads drop out cleanly and never lock the pause menu. |
| Prompts | Xbox or PlayStation button glyphs, chosen per player (e.g. Xbox for P1, DualShock for P2). |
| Menus | A **Tweaks** page in the main and pause menu holds the everyday options; render scale is in Video Settings. |

## Install

1. Install an ASI loader: [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
   (use the 32-bit `dinput8.dll` build next to `LEGOStarWarsSaga.exe`).
2. Copy `SagaTweaks.asi` into the game folder (or its `scripts\` / `plugins\` subfolder).
3. Proton / Wine: make the proxy DLL load. Steam launch option:
   `WINEDLLOVERRIDES="dinput8=n,b" %command%`
4. Start the game. `SagaTweaks.ini` and `SagaTweaks.log` appear next to the exe.

## Configuration

The common options are on the in-game **Tweaks** page (render scale, window mode and VSync are in Video Settings); all of
them live in `SagaTweaks.ini`, section `[SagaTweaks]`:

| Key | Default | Meaning |
|---|---|---|
| `SkipLogos` | 1 | Skip boot logos (restart needed). |
| `SkipTitle` | 1 | Skip the "Press any button" screen. |
| `SkipCutscenes` | 1 | Start skips cutscenes. |
| `HotSwitchInput` | 1 | P1 follows the last used device; extra gamepads join as P2 only with Start (like F2 on keyboard). |
| `StartButton` | 7 | Raw button code used as Start. |
| `ButtonPromptsP1` / `P2` | 0 | 0 auto, 1 Xbox, 2 PlayStation. `ButtonPrompts` sets both. |
| `PromptGlyphs` | 1 | 0 leaves the original prompt art untouched. |
| `MaxFPS` | 0 | FPS limit, 0 = unlimited (vsync still applies). |
| `RefreshRate` | 0 | Force a refresh rate, 0 = desktop. |
| `VSync` | 1 | 0 presents without waiting for the display (Video Settings row). |
| `WindowMode` | 0 | 0 fullscreen, 1 windowed, 2 borderless windowed (Video Settings row). |
| `RenderScale` | 1.0 | Internal resolution multiplier. |
| `FixFrameCounters` | 1 | Frame-rate independent clocks. |
| `FixGameplayRates` | 1 | Fixes the other per-frame rates found by the code audit (see `docs/AUDIT.md`). |
| `FixContactVelocity` | 1 | Keeps part of the old velocity after a contact at short steps. |
| `LogicHz` | 0 | Most game logic updates per second (0 = every frame). |
| `FixJumpPhysics` | 1 | Jump impulse corrected to the 60 fps lift and height. |

Debug-only keys, all off by default: `LogFPS` (frame rate in the log), `LogInput` (log every change of the input / player state), `LiveIni` (rereads `LogicHz`, `MaxFPS` and the fix switches once a second and logs the changes), `ProbeSweep` (logs the hero's collision sweeps), `ProbeHero` (one log line per logic update of player 1 near the log of Speeder Showdown: position, velocity, flags, input; read with `tools/audit/trace_jumps.py`), `ProbeButtons` (a log line whenever a floor button changes: triggered flag, animation state, hold timer), `ProbeLever` (a line per frame with the crank angle while a lever is held), `ProbeAnim` (per second: how often each animation list is stepped and the sum of the step), `ProbeConsts`, `DumpTextures`. They exist for reverse engineering; see `docs/REVERSING.md`.

## Build

Needs `i686-w64-mingw32-gcc`.

```
make        # build/SagaTweaks.asi
make dist   # build/SagaTweaks.7z
```

## Known issues

* **Climbing the fallen log in Speeder Showdown at very high frame rates** is not bit-identical to 60 fps. In
  the last 0.15 s of the jump the speed into the log is 3-6 % lower than at 60 fps; the jump still succeeds in
  all test runs (see `docs/AUDIT.md`, section 11).

## Compatibility

Patches are bound to fixed addresses of the Steam executable and wait until SteamStub has
decrypted the code. Other builds of the game will not match and are left untouched.
Single-player only changes; no online component. Use at your own risk.

## Source layout

`src/main.c` (entry, per-frame dispatch) and one module per feature: `startup`, `cutscene`,
`framerate`, `window`, `render`, `prompts`, `input`, `menus`, `config`, plus `util` (patch primitives) and
`texdump` (debug). Game addresses and signatures live in `src/game.h`. Each feature module exposes
`<Name>Ready()` (are the expected game bytes present?), `<Name>Install()` and `<Name>Frame()`.

## Releasing

The version lives in `VERSION` and is embedded in the build (the log starts with `SagaTweaks <version>`).
To release: add a `## <version>` section to `CHANGELOG.md`, bump `VERSION`, push to `main`. The
`release` workflow builds `SagaTweaks.7z` and publishes it with that changelog section as the notes
(a manual run of the workflow is a dry run). If the `NEXUSMODS_API_KEY` secret and the `NEXUSMODS_MOD_ID` / `NEXUSMODS_FILE_ID` variables are
set, the same archive is also uploaded to Nexus Mods.

## License

[GNU GPL version 3 only](LICENSE).
