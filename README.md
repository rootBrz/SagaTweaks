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
| Frame-rate fixes | Clock counters, blink timers and jump physics behave as at 60 fps. |
| Render scale | 50-300 % internal resolution, native row in Video Settings. Effects and the HUD pass are depth-tested at the scaled size. |
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

The common options are on the in-game **Tweaks** page (render scale is in Video Settings); all of
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
| `RenderScale` | 1.0 | Internal resolution multiplier. |
| `FixFrameCounters` | 1 | Frame-rate independent clocks. |
| `FixJumpPhysics` | 1 | 60 Hz vertical physics steps. |

Debug-only keys (`LogFPS`, `TraceRT` (also enables `<ini>.scale` run-time scale changes and depth diagnostics), `TracePasses`, `SkipPass`, `SkipScaledPass`,
`DumpTextures`) exist for reverse engineering; see `docs/REVERSING.md`.

## Build

Needs `i686-w64-mingw32-gcc`.

```
make        # build/SagaTweaks.asi
make dist   # build/SagaTweaks.zip
```

## Compatibility

Patches are bound to fixed addresses of the Steam executable and wait until SteamStub has
decrypted the code. Other builds of the game will not match and are left untouched.
Single-player only changes; no online component. Use at your own risk.

## Source layout

`src/main.c` (entry, per-frame dispatch) and one module per feature: `startup`, `cutscene`,
`framerate`, `render`, `prompts`, `input`, `menus`, `config`, plus `util` (patch primitives) and
`texdump` (debug). Game addresses and signatures live in `src/game.h`. Each feature module exposes
`<Name>Ready()` (are the expected game bytes present?), `<Name>Install()` and `<Name>Frame()`.
