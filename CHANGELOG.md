# Changelog

Each release needs a `## <version>` section here matching `VERSION`. The release workflow publishes that section as the GitHub release notes and fails if it is missing.

## 1.1.3

- New: skipping a cutscene fades to black (a quarter of a second, the cutscene keeps playing) and fades gameplay in from black, instead of cutting.
- Fixed: the player 2 join prompt ("Press F2 or START to start") wrapped to two lines and overlapped the stud counter. It now reads "F2 or START" on one line.
- Fixed: a dropped gamepad could be released immediately instead of after one second, and the window mode and button-prompt timers could fire at once. The elapsed-time checks added in 1.1.0 were wrong about every other millisecond.

## 1.1.2

- Fixed: the lightsaber trail disappeared for good after the first swing when the frame rate is unlocked. The saber added one trail segment per frame to a pool of 128, which ran dry within one swing at high frame rates. Segments are now added 60 times per second at any frame rate, like at 60 fps. `FixGameplayRates=0` turns it off.

## 1.1.1

- No functional changes. The release archive is a .7z instead of a .zip, because Nexus Mods quarantined the zip of 1.1.0.

## 1.1.0

- New: **VSync** and **Window Mode** (fullscreen / windowed / borderless windowed) rows in Video Settings. `VSync` and `WindowMode` in the ini. Changes apply at once.
- New: **FPS Limit** values below the display's refresh rate now work, and every value of the Tweaks list holds exactly (90 used to give 97 fps).
- Changed: `LogicHz` defaults to 0 (game logic runs every frame). The old cap of 120 is no longer needed.
- Fixed: the fallen tree in Episode VI "Speeder Showdown" can be climbed at any frame rate. At very high frame rates only about one jump in three worked; the jump now behaves like at 60 fps (10 of 10 jumps at 850 fps in tests). Causes: the ledge probe of the step-up used tolerances meant for one 60 fps step, and the velocity kept after a contact at short steps was too low.
- Fixed: jumps at high frame rates. The first frame of a jump lifted the player too little, so jumps from platforms were cancelled and jump height was up to 4 % lower. The jump now has the same first-frame lift and the same height as at 60 fps.
- Fixed: grass and foliage ran about 14 times too fast at high frame rates (constant wobble when standing still). Wind, sway, wobble, grass chains and the effect timeline are now stepped 60 times per second of real time.
- Fixed: many more per-frame gameplay rates found by a code audit of the whole game loop: slow or stalled turning, spinning and sliding entities, the lightning arc kick, rope damping, hero knockback and object pushes, particle lifetimes, the wall-push timer, and several hero physics terms that decayed per frame instead of per second. `FixGameplayRates=0` turns the audit fixes off.
- Fixed: the audio of a skipped cutscene kept playing over the level.
- Fixed: a dropped or unplugged gamepad was released after 60 frames, which is 60 ms at 1000 fps; it is now 1 second. The window mode is applied 0.5 s after start instead of after 30 frames.
- Fixed: the join prompt text could overwrite memory the game had reused. The prompt scan no longer runs while `HotSwitchInput` is off.
- Fixed: invalid ini values (`ButtonPrompts` outside 0-2, negative `MaxFPS` or `RefreshRate`) are clamped instead of crashing the Tweaks page; the window mode code checks the display mode table before using it.

## 1.0.2

- No functional changes. Fixes the release workflow's Nexus Mods upload (wrong zip path).

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
