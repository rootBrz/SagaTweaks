# Changelog

Each release needs a `## <version>` section here matching `VERSION`. The release workflow publishes that section as the GitHub release notes and fails if it is missing.

## 1.0.1

- No functional changes. First release published to Nexus Mods; the license, release workflow and Nexus upload were added.

## 1.0.0

First public release.

- Startup: skips the boot logos and the "Press any button" title screen.
- Cutscenes: Start skips a cutscene and every following part straight to gameplay.
- Display: no 85 fps cap, runs at the desktop refresh rate, render scale 50-300 % (native row in Video Settings), FSAA row removed (render scale replaces it).
- Frame-rate independence: clock counters, blink timers and jump physics behave as at 60 fps.
- Input: player 1 hot-switches between keyboard and gamepad; extra gamepads join as player 2 with Start; lost pads drop out cleanly and never lock the pause menu.
- Prompts: Xbox or PlayStation button glyphs, selectable per player.
- Tweaks page in the main and pause menu; Back (B / Esc) leaves it like the stock options pages.
- Fixed: characters disappearing after changing the render scale while the game is running.
