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
* Per-call decay `0.75` (`sub_68EE40`), `0.5` (`sub_4AB620`, `sub_4AC620`), `1 - blend` (`sub_4AC620`).
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
* Ledge shimmy `sub_56B8F0` is fine: the value added to `dt * speed` is the length of the hero's displacement this frame
  (square root of the squared sum), which already scales with the frame time. Not a defect.

Checked on the Deck: all patches apply (log lines without `FAILED`) and the level runs at about 1,000 fps; the
physical effect is untested.
