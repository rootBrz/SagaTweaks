# Changelog

Each release needs a `## <version>` section here matching `VERSION`. The release workflow publishes that section as the GitHub release notes and fails if it is missing.

## 1.2.0

- Fixed: Episode IV "Death Star Escape": the row of five buttons you light with the cart could not be completed at high frame rates (the buttons seemed to stay lit for a shorter time the higher the frame rate was). The game is balanced on a button letting go two game updates after its hold timer ends, about 33 ms at 60 fps but 3 ms at 600 fps, and the cart crosses the row only a few milliseconds faster than the buttons stay lit. The button's release now takes 33 ms at every frame rate (nothing changes at 60 fps).
- Fixed: the zipline swung wrong at high frame rates (the rope hung far too low). Part of the rope's per-update physics had been scaled and part had not.
- Fixed: at high frame rates the player's own steering after a fall came out about twice as strong as at 60 fps. A fixed halving of the target speed had been treated as a decay and scaled with the frame rate by mistake.
- Fixed: loose LEGO pieces on the ground twitched many times more often at high frame rates, and animated objects jittered at the frame rate instead of 60 times a second.
- Fixed: the running speed boost no longer restarts every time the ground contact blinks for a single update, and the push between two clashing lightsabers no longer grows with the frame rate.
- Changed: the game's smoothing curves (turning, easing of velocity) follow the response a 60 fps game has per second exactly, also for fast rates, instead of the continuous approximation.
- Changed: the log no longer records input state changes unless `LogInput=1`; the debug keys are all off by default. New debug keys: `LogInput`, `ProbeButtons`, `ProbeLever`, `ProbeAnim`; `LiveIni` now logs each value it changes. The log starts with the ini values in use.
- Developer: `docs/AUDIT.md` section 15 describes how the button row and the zipline were found and the rules learned (a `x *= c` is a decay only if `x` survives to the next call; a decay patch needs its source term patched too).

## 1.1.7

- Fixed: with two players, dropping the second player from the pause menu's "drop out" row left the Tweaks and Quit rows swapped: pressing Tweaks opened the Quit confirmation and Quit opened Tweaks. The Tweaks row is now located from the rows that are actually drawn, and the menu's cursor range follows them.

## 1.1.6

- Fixed: skipping a cutscene in the middle of a level skipped what comes after it. In "Invasion of Naboo" Jar Jar did not join the party after the skipped cutscene, and the same cutscene could be triggered again after going to the next part of the level and back. A skipped cutscene now ends the way a played one does: Start moves it to just before its end and the game ends it itself.
- Changed: skipping a cutscene now uses the game's own "Star Wars" screen wipe, instead of a fade to black. The cutscene's last picture stays on screen while the cutscene (and any parts that follow, including level changes) is skipped, then sweeps away over the gameplay. The black fade is gone.
- Fixed: the game always opened in exclusive fullscreen and only switched to the saved window mode on the main menu; on Windows the borderless mode then came out as a framed window while Video Settings said Borderless. A borderless mode is now set up before the first device is created, and the window style is restored if Direct3D overwrote it.
- Fixed: the keyboard did not hot-switch to a gamepad when only its analog sticks or triggers were used. They count as input now.
- Changed: the render scale is a backbuffer size (the video mode's size times the scale) instead of hooked render targets. This fixes the heat distortion, doubled shadows and the doubled HUD on the pause screen with a scale above 100 %, and does not depend on DXVK. Fullscreen is not offered while the scale is not 100 %.

## 1.1.5

- Fixed: after pausing at a render scale other than 100 %, setting the scale back to 100 % and pausing again showed the frame from the earlier pause behind the menu.

## 1.1.4

- Fixed: the lightsaber sparks were 14 times rarer at 850 fps than at 60 fps (and 2 times at 120 fps). The random roll that starts a spark burst used the frame time, and the burst itself also lasts one frame. The roll now uses 1/60 s at most, like at 60 fps. `FixGameplayRates=0` turns it off.
- Fixed: with a render scale above 100 % the HUD showed twice (a blurred second copy) on the pause screen. The game copies the backbuffer for the pause background; it got the previous frame, HUD included, instead of the scaled scene. The pause background is now a copy at the scaled resolution, so it stays as sharp as the live picture.

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
