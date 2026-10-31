# Auditing the whole game loop for frame-rate dependence

This is the method used to find code that assumes 60 calls per second, and the state of the one problem it has not
solved yet. Read it before starting another round, so the same dead ends are not walked twice.

## 1. Why a systematic audit

Symptom-driven debugging found the first causes (clock counters, jump lift, wind phase). Later symptoms (grass moving
too fast, the hero failing to climb the log in Episode VI "Speeder Showdown" at 850 fps but not at 60) had no single
cause. The game update, `sub_491D50`, is one function that calls about 1,600 others, and the 60 fps assumption is
spread across all of them. Looking for it one suspicion at a time kept missing things, so every function was read.

## 2. Where the data lives

Nothing decompiled is committed. Pseudocode of a commercial game does not belong in a public repository, and it can be
regenerated from the user's own exe in a few minutes. Everything below is in `orig/audit/` (gitignored, about 40 MB):

| Path | Content |
| --- | --- |
| `reach.txt` | Round 1 function list (1,628 functions reachable from `sub_491D50`): `addr size flag` |
| `pseudo/<addr>.c` | Hex-Rays text of those functions |
| `shards/compact/<addr>.c`, `shards/shard_NN.txt` | Same text without blank lines and local declarations; 77 shards of at most 1,300 lines; each shard line is `<file> <first line> <last line>` |
| `notes/shard_NN.md` | Round 1 findings, one line each |
| `notes2/J1..J8.md` | Round 2 findings (hero movement and collision, scenario-driven) |
| `dt_readers.txt` | Every function that reads the frame time `flt_925018`, marked `IN` or `NOT` in round 1 |
| `pseudo2/`, `shards2/`, `notes3/` | Round 3: the 228 dt readers round 1 missed, plus their callee closure (2,416 functions), 27 shards, findings |
| `traces/htrace*.txt` | Hero traces recorded on the Deck with `ProbeHero=1` |
| `briefs/` | The three reviewer briefs (also in `tools/audit/`) |

To rebuild, with a Python that can `import idapro` (the venv of the ida-pro-mcp plugin works):

    P=<venv>/bin/python
    $P tools/audit/ida_audit.py reach                                  # reach.txt
    $P tools/audit/ida_audit.py decompile --list reach --dst pseudo
    $P tools/audit/ida_audit.py shard --src pseudo --dst shards
    $P tools/audit/ida_audit.py dtreaders                              # round 3 seeds
    $P tools/audit/ida_audit.py decompile --list notin --closure --dst pseudo2
    $P tools/audit/ida_audit.py shard --src pseudo2 --dst shards2 --only notin

The regenerated `reach.txt` has about 1,590 entries, not 1,628: round 1 also contained functions added by hand. The
saved file is the reference. `tools/audit/scan_consts.py <lo> <hi> <value...>` lists instructions in an address range
that load a given float or double constant (used to find the patch site of a constant seen in pseudocode), and
`tools/audit/trace_jumps.py <trace>` summarises the jump attempts of a `ProbeHero` trace.

## 3. The review loop

1. Build the function list and the shards (above).
2. Write a brief (`tools/audit/brief*.md`): context, what to look for, what is already handled, exact output format
   (`sub_XXXXXX line N | H/M/L | quoted code | why it depends on the rate | per-second form`).
3. Start one cheap reviewer per shard (Haiku, `general-purpose` agent), at most 20 at once, each told to read every
   line of its shard and to write `notes*/shard_NN.md`. A full round of 77 shards takes about an hour.
4. Never trust a finding. Read the function yourself, check the units of every field it names, and look at the
   disassembly of the exact instruction before patching. Regex detectors were tried first and are useless: they flag
   every `+=` and say nothing about units.
5. Patch, test on the device, and write the verdict (fixed, false positive, open) into section 5.

Practical notes:
* The tool output viewer hides parts of long text ("N sections elided") and prints a handle `<<ccr:ccr_...>>`. The
  hidden text is recovered with the `caveman_retrieve` tool (pass the handle). Reviewers were told to do this; a
  reviewer that skipped it reviewed incomplete code.
* The limit is 20 concurrent agents; start the next shard when one finishes.
* Reviewers report plausible but wrong things about half of the time (see the false-positive classes below). They are
  good at reading volume, bad at units. Give them the field meanings you already know.

## 4. What the audit missed, and how that was found

Round 1 followed static call chains only. The game calls the per-character and per-state hero handlers through
function-pointer tables (`(*(*(hero+84)+24))(hero)` and similar), which the call graph does not show. The check that
exposed it is cheap: list every function that reads `flt_925018` (xrefs to `0x925018`) and compare with the reviewed
set. 351 functions read the frame time and 228 of them had never been reviewed. Do this check first next time, and
also look at callers of any suspicious field (`+472`, `+3524`, `+3864` ...) for functions outside the set.

## 5. Findings and verdicts

Fixed (all in `src/framerate.c`, described in `docs/REVERSING.md`):
* Collision tolerance fraction, seven absolute push-out constants plus `0.001` in `sub_68F940` (`0x68FE03`), velocity
  rebuild after the sweep, effect timeline, wind phase, and the grass, sway, wobble and chain systems (stepped 60 times
  per second of real time).
* Per-call decay `0.75` (`sub_68EE40`), `0.5` (`sub_4AB620`, `sub_4AB5A0`). (The `0.5` and `1 - blend` gains of `sub_4AC620` were scaled here until 1.1.7, wrongly, see section 15.4.)
* Int truncation of angle steps: `sub_550100` (call at `0x550121`) and `sub_550070` (whole function replaced); both
  keep the fraction on average by rounding up with probability equal to it.
* Wall-push timer of `sub_59BD40` (`0x59BE31`): gained `dt * (2/dt/30)` = 2/30 per call below 30 ms frames, so the
  wall-push state started after 4 calls (5 ms at 850 fps instead of 67 ms).

False positives that cost time (do not chase again):
* `% v113` update gate in `sub_434A20`: the counter `dword_925068` is already advanced at 60 per second.
* `flt_925018 < 1/60` branch in `sub_4ABAC0`: at 60 fps both branches give `diff * k * 0.333`.
* `sub_678410`, `sub_678580`: `+88` is the inverse ellipsoid radius, `+160` the sweep tolerance fraction.
* Constants applied to the collision work block fields `+168..176` look like velocity terms; `+168` is the contact normal
  or move direction, so multiplying it by a constant is a step proportional push (fine) or an absolute push (needs
  scaling) depending on whether the field is a direction or a displacement. Check before scaling.
* "Missing 24000 factor" in turn clamps (`sub_4102C0`, `sub_4D94F0`): decompiler artifact of a value held in an earlier
  variable.
* `sub_54FB40`, `sub_54FB80`, `sub_550070`, `sub_550100`: multiply by `flt_925018` inside (`sub_54FAD0` does not).
* Level-specific hazards flagged and left alone because they are not in the scenario: lightning arc kicks
  (`sub_4EAF30`), rope damping (`sub_4D6060`), ledge shimmy (`sub_56B8F0`), several UI and particle systems.

## 6. Open problem: climbing the fallen log at high fps

Scenario: start of Episode VI "Speeder Showdown", hero on the pallet, jump over the log. At 60 fps (and with
`LogicHz=60`) it works every time; at 850 fps with all fixes above about one attempt in three works. Measured with
`ProbeHero=1` (one trace line per logic frame near the log: time, dt, position, velocity, flags, input, hero state,
wall-push timer):
* The hero ends against the log face at x about 37.18 and is lifted by the step-up logic to y = 2.94, standing with
  horizontal speed about 0.14.
* A jump from there rises 0.50 and stops at y about 3.445, just under the log top (3.505). It succeeds only when the
  hero is over the rounded shoulder, with horizontal speed kept at the apex. In a trace of 11 jumps, the 4 successes all
  started at z <= -74.66 and all 7 failures at z >= -74.65, so where along the log the hero stands matters.
* The 60 fps reference (`LogicHz=60`, 10 jumps) was recorded after the fixes of section 9. Every jump starts at
  y 2.94 with horizontal speed about 0 (0.14 at 850 fps), reaches vy 2.30 and rises 0.54 (apex y 3.50, above the log
  top 3.505). The downward acceleration is -5.9 for the first 0.2 s, then falls to about 0 between 0.35 s and 0.5 s
  while the hero presses forward (vx 0.15 to 0.25): the hero is carried by the contact with the rounded shoulder.
* At 850 fps the jumps split in two: a short rise of about 0.50 (vy reaches 0 at 0.42 s, acceleration stays -5 to -7, vx
  drops back to 0) that fails, and a full rise of about 0.535 (0.51 s) that succeeds. Both start with the same vy 2.30
  and held jump input, so the difference is made by the contact in the last 0.1 s of the rise, not by the jump impulse.
* The contact-velocity blend in `VelRebuild` (`FixContactVelocity`) explains the standing speed of 0.14 at 850 fps.
  Switching it off gives a standing speed of 0 like 60 fps, but then all 15 jumps stop at apex 3.41 and none clears the
  log, so it stays on.
* `LogicHz=120` behaves like an uncapped logic rate.
* A fixed 60 Hz logic step with interpolated drawing would reproduce the original behaviour, but the picture was
  judged bad and the variable step already clears the log sometimes, so it was dropped (`docs/REVERSING.md`).

The remaining difference is in the contact handling of the sweep at small steps (the 0.005 contact gap, float
resolution at x = 37, the size of the response offsets). Next step if it is picked up again: log the sweep results of
`SweepProbe` (`ProbeSweep=1`) for one short and one full rise and compare the contact normals and push-outs frame by
frame.

## 7. Test setup (Steam Deck)

`LogicHz=0` (logic every frame), `VSync=0`, `RenderScale=0.5`, `LiveIni=1`, `ProbeHero=1`; load the level through
`dword_951B80` (see `docs/REVERSING.md`); scripted input through uinput. The recorded trace goes to `SagaTweaks.log`
(`H` lines); copy it with ssh and run `tools/audit/trace_jumps.py` on it.

## 8. Rounds 4-7 (callee layers) and where it stands

After round 3 the audit went "deeper" automatically: `ida_audit.py frontier --round N` lists every function that is
called by (or whose address sits in a data table next to) anything already reviewed (`orig/audit/reviewed.txt`) and
is not reviewed yet; those are decompiled, sharded and read like the earlier rounds (`tools/audit/brief4..7`).

| Round | Functions | Shards | Notes | Result |
| --- | --- | --- | --- | --- |
| 4 | 1,941 (1,379 table entries, 562 callees) | 50 | `notes4/` | complete |
| 5 | 976 | 30 | `notes5/` | complete |
| 6 | 667 | 22 | `notes6/` | complete |
| 7 | 407 | 15 | `notes7/` | complete (rerun after a usage-limit interruption) |
| 8 | 221 | 9 | `notes8/` | complete |
| 9 | 115 | 5 | `notes9/` | complete |
| 10 | 45 | 2 | `notes10/` | complete |
| 11 | 46 | 1 | `notes11/` | complete |
| 12 | 13 | 1 | `notes12/` | run last; the layer is almost empty, the audit has reached its fixpoint for practical purposes |

Layer sizes fell from 1,941 to 13 over nine rounds, so the call graph plus function-pointer tables of the logic loop has been covered (about 6,300 functions). Almost all of
rounds 4-6 is UI, audio, CRT, loaders and editor code. Real hero-relevant findings from them: the twin `+3140 * 0.5`
halving in `sub_4AB5A0` (fixed, `0x4AB5E1`), the wall-push timer in round 3 (fixed). Everything else flagged in those
rounds was a world-unit tolerance, a timer that is in fact decremented by dt (checked with a grep of all writers of
`+3368 +4240 +3500 +3416 +472`), a one-shot assignment, or code for other mechanics.

Conclusion so far: no further per-call increment or decay of the kind that was fixed remains in the hero path. The
log-climb difference between 60 and 850 fps therefore most likely comes from the numeric behaviour of the collision at
steps of 1e-4 (float resolution at x = 37, absolute push-out sizes) and needs the 60 fps reference trace
(section 6) rather than more reading.

## 9. Second fix pass from the audit (untested at the time of writing)

Mechanical fixes with byte-checked patch sites, all behind ini `FixGameplayRates` (default 1), in `src/framerate.c`:
* `k_trunc_sites`: 28 calls of `sub_6E6B90` whose argument is a product with the frame time (found by scanning for an
  access to `0x925018` within 40 bytes before the call). They call `ScaledDither`: if the value would have been below 1
  at a 1/60 s step the original truncation is kept (so intended zeros stay zero), otherwise the fraction is kept on
  average.
* `k_rate_sites`: constant operands redirected to `constant * min(1, dt*60)` (lightning kick `0x4EB408`, the +-16
  rotation steps `0x620911` / `0x620D11` with their conversion calls, the 33.33 ms overlay timer `0x631774`) or
  `constant ^ scale` (rope damping `0x4D6DBA`).

Third pass, hand-written stubs for amounts computed at run time (same switch `FixGameplayRates`):
* Knockback `sub_58E4E0` (`0x58E7D0`) and object push `sub_42C740` (`0x42CC1E`): the amount loaded from the stack is
  multiplied by `min(1, dt*60)`, so the velocity added while two objects overlap is the same per second.
* Call-counted byte counters: `sub_4802E0` `+4231` (`0x4803B5`, counts down from 50) subtracts the number of 60 Hz ticks
  of the frame (`g_steps`, 0 on most frames at high fps) instead of 1; `sub_54EA40` `+1972` (`0x54F355`, `0x54F3DD`,
  `0x54F40A`: -10, -20, +10 per call) uses `g_steps * delta`.

* Particle emitter `sub_410A10`: lifetime counters `+268` (`0x410A7B`) and `+280` (`0x410A82`) drop only on frames with a
  60 Hz tick; the path step `flt_7F15DC` at `0x410ED5` is scaled by the step.
* Hit-reaction gate `sub_438230` (`+4227`, accept every 3rd call): left alone. `sub_58E4E0` polls it every call during an
  overlap, but the saber path may call it once per hit, and a tick-gated counter would drop such hits on frames without a
  60 Hz tick. Unpatched, the only difference at high fps is a polled hit connecting after 3 calls (3 ms) instead of 50 ms.
* Camera `sub_40ADE0` re-read for the constant scan (0.5, 0.1, 0.95 hits): fine. Position and angle smoothing are
  `dt`-proportional (`rate * dt * flt_7F10D4`, `sub_550070` already patched), the ramp `flt_7F10D4` and the transition
  timers `+444/+452/+456/+95139C` add `dt`; `v238 * 0.5/0.75` and the two-player `* 0.5` act on locals rebuilt every
  call; `0.1/0.05` are shake amplitudes. Only the free-look camera (case 5, per-call ramp of `flt_7F1128`, `dt` truncated
  to int) is per-call, and it is entered only through `sub_40AA00(1, ...)`, which nothing calls: dead code.
* Ledge shimmy `sub_56B8F0` is fine: the value added to `dt * speed` is the length of the hero's displacement this frame
  (square root of the squared sum), which already scales with the frame time. Not a defect.

Checked on the Deck: all patches apply (log lines without `FAILED`) and the level runs at about 1,000 fps; the
physical effect is untested.

## 10. Candidate cause of the log-climb failure: step-up probe tolerance (untested)

Found by reading `sub_68EE40` in full and re-reading the old `ProbeHero` traces (`orig/audit/traces/htrace*.txt`):
* The traces misled in one way: the "successful" jumps of the 4 s window are second jumps (the hero falls back to
  y 2.45, is lifted to 2.94 again and jumps once more). In all three traces **every first jump failed**, apex 3.445-3.449,
  independent of z. The second jump that clears reaches y 3.4627 with vy 0.145 and then 3.49 (flag `f1`). So the margin
  is a height of about 0.015, not a position along the log.
* `sub_68EE40` is the step-up / ledge snap. It copies the hero ellipsoid, raises it by `flt_810B30` (0.1), sweeps it
  down by `-2 * 0.1 = -0.2` (a fixed vector, not `move * dt`) and, if `|hero y - landing y| <= 0.1 * 0.5` and the
  surface normal y >= 0.707 and the move this frame is not rising faster than half a step, moves the hero to the
  landing point. It is the only caller of this kind in `sub_68F940`, whose only caller is the hero mover `sub_416D60`.
* The sweep stops short of the surface by `work+160 * |vector|` (`sub_678F60`: `t = hit - work+160`), and `work+160`
  is the "very close" fraction that the mod raised to `0.005 / (dt * 60)` (0.07 at 850 fps) so that it stays 1e-4 world
  units for `move * dt` vectors. On the fixed 0.2 probe that gives a stop 0.014 above the ledge (0.001 at 60 fps), plus
  push-out `0.004 * ny` that the mod scaled down to 0.0003 (0.004 at 60 fps). The probe therefore reports a ledge about
  0.01 higher than at 60 fps and the hero must rise that much more to fall inside the 0.05 window. The measured apex
  shortfall (failures 3.446 vs the 3.4627 that triggers it) is the same size.
* Fix: `StepUpProbe` wraps the call at `0x68FC07`, sets `work+160` to 0.005 and the push-out table to its 60 fps values
  for the duration of the probe. Switch: `FixGameplayRates=0` turns it off.
* To confirm on the Deck: same scenario as section 6 (10+ first jumps at 850 fps, `ProbeHero=1`); expect apex >= 3.50 and
  the hero standing on the log. If it still fails, set `ProbeSweep=1` and compare probe landing y with 60 fps.
* Other fixed-length sweeps would have the same problem; there are none besides this probe (`sub_68F940` has one caller).

## 11. Result of section 10 and the follow-up (tested)

* `StepUpProbe` (section 10) fixed most of it: 9 of 10 jumps climb at 850 fps instead of about 1 in 3.
* Remaining difference found with `ProbeSweep=1` captures of one jump at 60 Hz and one uncapped (`S` lines: step, hit
  normal, resolved move). Positions and normals agree, but the velocity that enters the sweep (`v` of the S line) was 14 %
  lower uncapped: 0.172 vs 0.200 standing against the log. The walk speed eases to its target by 1/6 of the gap per
  1/60 s (a fraction that scales with dt); against a wall the speed that enters the sweep settles at
  `f*v / (s + f*(1-s))` with blend `s` (`VelRebuild`), `f*v` when `s = 1` (60 fps). `VelRebuild` now uses
  `s * (1-c) / (1-c*s)` with `c = 0.19` (1/6 predicts the value, 0.19 fits the measured jumps).
* Tuning with `c` live from the ini (10 jumps per value, ratio of the pre-contact `vx` to the 60 Hz reference per 0.05 s
  of the jump): `c = 0.167`: 0.95 early; `0.22`: 1.02-1.05 early, 11 of 12 climbs; `0.19`: 0.99-1.01 early, 10 of 10.
  The last 0.15 s of the rise stay 3-6 % low (0.94-0.97); the cause is not found.
* The load `0x679675` (`sub_679310`, movement `-= normal * 0.01` on a hit with a moving object) and `0x68CA90` (editor)
  were also scaled. `0x67A23C` and `0x6790F2` are fixed world-unit offsets / thresholds, not rates.

## 12. Open items

* **Log climb, remaining shortfall.** With the fixes of sections 10 and 11 the speed that enters the sweep matches
  60 fps within 1 % for the first 0.35 s of the jump (ratio 0.99-1.01) but falls to 0.97, 0.96, 0.94 and 0.94 of the 60 fps
  value in the windows starting at 0.35, 0.40, 0.45 and 0.50 s (10 jumps, `c = 0.19`), while the jump apex still matches
  (3.488-3.499 against 3.49 for 60 fps) and 10 of 10 and 11 of 12 jumps climbed in the tuning rounds. Not found. Untested guesses:
  the easing fraction `c` is a constant but the air and wall cases may ease at different rates; the contact normal turns
  from (-1, 0) to (-0.5, 0.86) over the rise, and the blend removes the normal part of the velocity per step while the 60 fps
  game does it in one projection. Tried and dropped: running the logic at exactly 60 Hz for 1.2 s after each
  jump (`ExactJumps`, 11 of 11 climbs, steps verified at 1/60 s); the world, camera and enemies step at 60 Hz during
  the window, which was judged not worth it.
* Resolved (static, section 15.5): the build-start shake of `sub_5912E0` does not accumulate. It reads the rest-pose translation
  (`sub_5CF9C0`, struct `+48`) and writes the current one (`sub_5EA5D0`, struct `+112`).
* Fixed since: the FPS limit below the refresh rate (`LimiterRefresh` and `PaceFrame` in `src/framerate.c`: the game's
  limiter takes `max(refresh, limit)` and sleeps whole milliseconds, so 90 gave 97 fps; now exact for every value of the
  Tweaks list) and the audio left over after a cutscene skip (the skip now calls the game's stream stop `sub_693FD0` on
  the audio object `0x924DF8`, which the game otherwise only does on level changes).

## 13. Lightsaber trail (streak) pool (fixed in 1.1.2)

Symptom: at an unlocked frame rate the saber trail was gone after the first swing. `sub_5A3730` adds a segment to a trail
(pool of 128 at `dword_95DA38`, trails at `dword_95DA3C` / `dword_95DA40`) and the saber code (`sub_49FA50`, `sub_4B1AB0`)
calls it once per frame with a segment life of 0.25 s; `sub_5A3BD0(dt)` expires one tail segment per call. Counting the
free list over the game's memory (`/proc/<pid>/mem`) while a swing was driven by poking the attack bit (`0x80`) into the
controller: free segments fell to 0 within one swing and stayed at 0 with no active trail afterwards. With the fix they
bottom out at 7-16 and return to 128. Fix: `StreakAdd` wraps the three calls and only marks the trail in use (`+16`,
`+24`) on frames without a 60 Hz tick (`g_steps`). Not explained: how the segments become unreachable once the pool is
empty (free list empty, no active trail); at true 60 Hz logic the pool also reached 0 once with six trails and recovered.

## 14. Saber sparks (fixed in 1.1.4)

Symptom: at an unlocked frame rate the sparks of clashing sabers were much rarer. Hits call `sub_62C350(rate, dt)` (a random
count of events for a rate per second, 0 or 1 at 10 and 100 per second; `0x4A0455` in `sub_49FA50`, `0x4B231D` and `0x4B24E0`
in `sub_4B1AB0`) and pass the count to `sub_684C90` -> `sub_656DC0` -> `sub_6561D0`, which emits particles over a window of
`flt_8108E4` (the frame time) seconds. The rate already scales with dt and so does the window, so the sparks per second fall
with dt squared. The three calls go through `SparkRate`, which rolls with at least 1/60 s. Not checked in game (needs a saber
fight); the other `sub_62C350` callers (about 25) have the same shape and were left alone.

## 15. Findings after 1.1.7 (version 1.2.0)

Two problems were still open after section 14: the Death Star Escape button row could not be completed at high frame rates
(`LogicHz=60` made it work), and the zipline swung wrongly. They were found by bisecting the game update at run time with a
temporary "call gate" (removed again in 1.2.0, see the git history of `src/gate.c`): every direct call of `sub_491D50` (and later of
the obstacle class and its button handler) went through a thunk, and an ini range made the calls in it run only on a 60 Hz tick
of real time, with the accumulated time as frame time, like `LogicHz=60` does for the whole update. Halving the range found call 67
(`sub_559CD0`, the update of every gizmo class), then the class (0, `GizObstacle`, update `sub_558070`), and a trace of the button
states (`ProbeButtons`) showed what the gate was really changing. The method is cheap and finds a fault even when reading does not,
but read the trace before trusting a gate result: gating a class with the cart changed the *timing* of its logic, not its maths.

### 15.1 Button row: the game is balanced on a two-call release lag (fixed)

Traces of the row (five buttons 0.5 units apart, hold 1.1 s, all five must be down at once). The cart crosses at 1.800 units/s
(`tgt`, `vel` and measured speed equal, flat from 520 to 715 fps), so the five buttons are pressed over 1.111 s, and each reaches
state 2 (fully pressed) 0.234 s after the cart enters it, at every frame rate. `sub_557B90` (`0x557FEC`) does "if (timer <= 0)
release, else timer -= dt" once per call, and the release only sets the animation speed negative: the animation step
(`sub_5F4B90`, call 37 of `sub_491D50`) runs before the obstacle class (call 67) in a frame, so the button leaves state 2 two calls
after the timer reached zero: 33 ms at 60 fps, 4 ms at 600 fps (measured), 18 ms with the class on 60 Hz ticks. All five are down
together only if that lag exceeds about 11 ms: +22 ms margin at 60 fps, -8 ms at 600 fps. That is the forum report ("activation
time is reduced the higher the frame rate is"). Fix: `ReleaseSlackStub` makes a mode 2 button wait until its timer is
`-(2/60)(1 - min(1, 60 dt))`, so the lag is 2/60 s at every rate and nothing changes at 60 fps and below (mode 4 keeps 0, its timer
does not count below 0). Verified on the Deck ("it works").

### 15.2 Zipline (fixed)

`sub_4D6060` updates the rope offset vector (`+68..+76` of the rope) every call as `p = 0.9 p` and `p.y += 0.5 * hero+180`.
1.1.7 scaled only the `0.9` (`0.9^s`), so the equilibrium `0.5 h / (1 - 0.9^s)` was 67 h instead of 5 h at 850 fps and the rope
hung far too low. The load of `hero+180` at `0x4D6DDD` now goes through `RopeImpulseStub`, which multiplies it by
`(1 - 0.9^s) / 0.1` (1 at 60 fps), the exact sum of `s` steps of the 60 fps rule. The `0.5` itself is shared with a position offset
in the same function and is not touched.

### 15.3 Loose building blocks twitched too often (fixed)

`GizBuildit` (`sub_5912E0`): each loose piece that is not twitching rolls `p > random()` on every call (p = 0.1 minus 0.002 per
unplaced piece, at least 0.01, times 0.1 unless flag `+122 & 4`) and then twitches for 0.2 s (a timer that does count by `dt`). The
random number of that roll (call at `0x591F2B`) comes from `BuilditRoll`: the real one on frames with a 60 Hz tick (`g_steps`), a
number that always fails otherwise. `ShakeOffset` does the same for the three per-call sites of `sub_5A1480` in `sub_468B50` (a
random +-0.025 jitter of animated objects), so the jitter changes 60 times a second; the other callers of `sub_5A1480` place objects
once and are not touched.

### 15.4 A wrong patch removed, and the rule (fixed)

`sub_4AC620`, hero state 43 (a fall; `+3639` is the kind of fall), blend `+3480` below 1: `+3864 *= 1 - blend` and, for `+3639` 2 or
4, `+3864 *= 0.5` (same for `+3872`). The targets are rebuilt on every call before this (default path `sin(heading) * speed`), so
these are constant gains on a fresh value, not decays. Round 1 reviewers called them "per-call halving" and 1.1.7 scaled them with
`pow(x, 60 dt)`, which at 850 fps made the 0.5 about 0.99: the hero's own target velocity in that state came out about twice as
large as at 60 fps. Both patches (`ADDR_DAMP_PUSH`, `ADDR_DAMP_BLEND`) are gone. Rules for this audit: an `x *= c` is a decay only if
`x` survives to the next call (check the writers of `x`); and a decay patch needs its matching source term patched too (15.2). The
remaining decay patches are the `+3140` multiplier halving (`sub_4AB620`, `sub_4AB5A0`; set by pads, only decays) and the step-up
`0.75` of `sub_68EE40` (not verified whether it acts on a fresh or a persistent value).

The smoothing helpers were made exact too: `Lerp60` (`sub_54FB40`) and `Approach` (`sub_550070`) use the fraction
`1 - (1 - rate/60)^(60 dt)` of the gap per call instead of `rate * dt`, which is what a 60 fps game does per second (the two differ
for rates above about 30, and a rate of 60 or more snapped at 60 fps).

### 15.5 What was read and found clean

* Object helpers keep a rest pose at struct `+0` (translation `+48`) and the current pose at `+64` (translation `+112`); `sub_5CFA20`
  returns the struct, `sub_5CF9C0` the rest translation, `sub_5EA570` / `sub_5EA5D0` write the current pose. Shake, twitch and
  `sub_5A1480` therefore compute "rest pose + offset" every call; they set, they do not accumulate.
* `sub_558070` (obstacle class): timers by `dt`, animation speed set idempotently (`sub_5A1030`), moving colliders registered in a
  16-entry table (`sub_58B3B0`, emptied every frame by `sub_58B3A0`) that `sub_58B440` turns into a shove velocity on the hero
  (`+512/+520`, at most 2 units/s, proportional to penetration), consumed by `sub_4AC620`. Rate independent.
* Hit gate `sub_438230` (`+4227`, accept every 3rd call): left alone. `sub_58E4E0` polls it every call, the saber path may call
  it once per hit, and a tick-gated counter would drop such hits.
* Hero state 43 is the fall state, not the cart; the cart ride (state `-1`, surface 2) uses the animation's authored speed 1.8.

### 15.6 Gizmo class lists

The class table of `sub_559660` is built from one of three getter lists, chosen in the level loader (`0x4CA1F6..0x4CA29A`):
`sub_46A150` (23 classes) when `dword_876580` is set, `sub_46A340` (28) when `dword_7F39E4` is set, otherwise `sub_46A230` (28).
Each getter fills a 30-dword descriptor (name, update function at `+24`) from the template at `unk_802728`. The record array is at
`*(dword_93B268 + 8)`, 156 bytes per class, update function at `+56`, called by `sub_559CD0(game, list, dt)`. Updates seen: GizObstacle
`sub_558070`, GizBuildit `sub_5912E0`, GizmoPickup `sub_5716C0`, Grapple `sub_54E0C0`, Techno `sub_4CD220`, Tube `sub_56DB80`, ZipUp
`sub_4D6FD0`, GizTurret `sub_5AC080`, PushBlocks `sub_5A5BD0`, SecurityDoor `sub_4BD280`, Shard `sub_4C0420`, Signal `sub_4C7D30`,
Attracto `sub_402CE0`; class numbers differ per list, match them by update address.

## 16. Findings after 1.2.0 (version 1.3)

* The wall lever (and hat machine) wobble at high frame rates is a per-frame roll in a **render-prep class hook**, a place
  the audit of the game update never looked at: `sub_4903F0` / `sub_472FE0` roll `random < 2048` on every call to start a
  0.15 s handle wobble. `LogicHz` cannot reach render-prep hooks. Same shape as the building-block twitch. The crank class
  ("Spinner") spawned its sparks the same way. All four rolls are gated to 60 Hz (`BuilditRoll`), and so are four more found by scanning every call of the random function in the exe (crank puffs while a crank is held, Force Lightning shake and rumble, an ambient sound). `class_map.py rolls` lists them all; no small-chance per-call roll is left.
* New rule for the audit: besides the game update `sub_491D50`, read the **class hooks** (`+52`, `+56`, `+60`) and
  everything `sub_4927F0` calls. `docs/CLASSES.md` has the class map, the method and the list of hooks still to read.
* A shader clock in 1/60 s steps (`flt_2857274`) was tried and removed: no visible effect, and it would step shader
  animation at 60 Hz on faster displays.

