# Changelog

Each release needs a `## <version>` section here matching `VERSION`. The release workflow publishes that section as the GitHub release notes and fails if it is missing.

## Unreleased

- Fixed: jumping at high frame rates. The first frame of a jump lifts the player by only speed x frame time, which is too little for the contact test of platforms (the pallet at the start of Episode VI "Speeder Showdown"), so the jump was cancelled and the fallen tree could not be cleared. The jump impulse now gives the same first-frame lift and the same apex as at 60 fps, at any frame rate. Jump height used to be up to 4 % lower at 120 fps and above even on the ground.
- Fixed: grass and foliage ran about 14x too fast at high frame rates (constant wobble when standing still). The wind and sway phase, the wobble springs, the grass chains and the effect timeline are written for one step per 1/60 s; they are now stepped 60 times per second of real time at any frame rate.
- Fixed: slow turning and stalled heading changes at high frame rates (angle steps were truncated to whole units every frame), a missed 0.001 contact push-out, and the wall-push timer, which fired after 4 frames (5 ms at 850 fps) instead of 67 ms.
- Fixed: more per-frame gameplay rates found by a whole-loop code audit: 28 `(int)(rate x dt)` truncations that lost their fraction at high frame rates (turning, spinning and sliding entities stalled), the lightning arc's velocity kick on the hero, the rope damping, several per-call rotation steps and timers, hero knockback and object pushes, particle emitter lifetimes, and two call-counted state counters. They can be disabled with `FixGameplayRates=0`.
- Fixed: hero physics that decayed per frame instead of per second at high frame rates (velocity on the step-up over the fallen tree in Speeder Showdown, platform and speed-boost terms, slow turning).
- Fixed (untested on the device): the ledge probe of the step-up (`sub_68EE40`) used tolerances scaled for `move x dt` vectors on a fixed 0.2 long vector, so at 850 fps it stopped 0.014 above the ledge instead of 0.001. That is the size of the gap by which the jump over the fallen tree in Speeder Showdown stopped short. Part of `FixGameplayRates`.
- Fixed (tested on the Deck: 10 of 10 jumps over the fallen tree at 850 fps, speed into the wall within 1 % of 60 fps early in the jump): the contact velocity blend (`FixContactVelocity`) is corrected for the walk easing, two more push-out loads (`sub_679310`, an editor handler) are scaled with the step.
- Fixed: **FPS Limit** values below the display's refresh rate did nothing, and the others overshot (90 gave 97 fps). Every value of the Tweaks list now holds exactly.
- Fixed: the audio of a skipped cutscene kept playing over the level.
- New: **VSync** and **Window Mode** (fullscreen / windowed / borderless windowed) rows in Video Settings; `VSync` and `WindowMode` in the ini. Changes apply at once.

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
