# Second-pass audit: hero movement and collision at tiny time steps

You review decompiled C (IDA Hex-Rays pseudocode) of LEGO Star Wars: The Complete Saga. Read-only job: do not run the game, do not edit anything except your notes file.

## Background
Global `flt_925018` is the frame time in seconds. The game was written for ~1/60 s. We run it at ~1/850 s
(dt = 0.00118). A first audit already fixed everything of the type "constant added/multiplied once per call".
Those are done (do NOT report them): wind/sway phases, sub_66CC60 wobble, sub_544420 chains,
`*= 0.75` in sub_68EE40, `*= 0.5` of +3140/+3864/+3872, `1-blend`, int truncation in sub_550100, the push-out
constants 0.0003/0.0035/0.01 (absolute epsilons) , the sweep tolerance fraction (work+160), velocity rebuild after sweep.

## The remaining bug (what we measured)
Scenario: Episode VI "Speeder Showdown": the hero walks into a fallen log (a static wall with a rounded top).
Hero object `a1`: pos +92/96/100, velocity +104/108/112, ground flags +641, flags +508, contact timer +472.
Measured at 850 fps (60 fps is reference and works every time):
* The hero is pushed against the log face and is lifted to the maximum step height by the step-up logic
  (y 2.33 -> 2.94, about 0.8 units/s), then stands there with horizontal speed vx about 0.15 (stick pushed
  into the wall; velocity after collision = resolved_move / dt).
* A jump from there rises 0.50 and reaches y = 3.445, just below the log top 3.505. Whether the hero then
  slides over the rounded shoulder depends on small horizontal motion near the apex. In the failed jumps vx
  grows to 0.28 and then drops to ~0.05 at the apex (x stops at 37.218). In the good jump vx stays 0.29 and x
  passes 37.23. So horizontal speed in the air next to the wall is decided by tiny effects.
* After the sweep `velocity = resolved_move * (1/dt)` (sub_416D60 around line 663).
So we are looking for dt-dependent behaviour of a different kind than "per-call increments":
1. A threshold, clamp, deadzone or epsilon compared against a per-step quantity (move length, velocity*dt,
   position difference) instead of a per-second quantity or a world distance: e.g. `if (len < 0.001)`,
   `if (fabs(dx) < eps)`, `max(step, 0.01)`. At dt = 1/850 per-step quantities are 14x smaller, so such tests
   change outcome.
2. Quantities derived by dividing by dt or multiplying by 1/dt (`/ flt_925018`, `* v139`): noise amplification;
   also mixed use of position deltas and velocities.
3. Float precision: tiny dt-sized amounts added to large coordinates (x ~ 37, ulp 3.8e-6), comparisons of
   positions that only differ by such amounts, exact equality `==`/`!=` on floats that are recomputed.
4. Integer/short truncation of per-step quantities (angles, counters) other than the ones listed above.
5. State that waits a fixed number of calls, or a flag that is reset every call and so is only valid for one
   call (contact flags +641, +4216 contact buffer, +508 bits, +3616 bits, +3828 bits).
6. Smoothing / easing / ramp code `a + (b-a)*k` where k is a constant not multiplied by dt (the helpers
   sub_54FB40, sub_550070, sub_54FB80, sub_54FAD0 DO multiply by dt inside; calls to them are fine).
7. Anything that makes the air control / ground acceleration / wall slide depend on how many calls happen.
8. Min/max step limits such as "move at least X per call" or "at most X per call" with X not scaled by dt.

## How to work
* Your job file lists function files `pseudo/<addr>.c` (full Hex-Rays text, one function per file) in
  `/tmp/claude-1000/-home-alan-lego-sw-cs/6888468d-58f1-4759-a2da-2d818d89ba3d/scratchpad/pseudo/`. Read EVERY line of every
  assigned function. Long functions: read in chunks of ~150 lines with the Read tool.
* The Read/Bash output viewer replaces parts of long output with "... N sections elided (caveman) ..." and prints a
  handle like `<<ccr:ccr_xxxx>>` at the end. You MUST recover the hidden text: call `mcp__caveman__caveman_retrieve`
  (load it with ToolSearch `select:mcp__caveman__caveman_retrieve`) with that handle and no query to get the exact
  original. Never review code you only saw elided.
* For every callee that is not in your list and whose behaviour you need (is it per-second or per-call?), open
  its file in `pseudo/` too (small helpers only).
* Think about the scenario above while reading. For each suspicious line decide what happens at dt 0.00118 vs
  0.0167 and whether it changes the climb/slide behaviour. Prefer few well-argued findings over many guesses.

## Output
Write `/tmp/claude-1000/-home-alan-lego-sw-cs/6888468d-58f1-4759-a2da-2d818d89ba3d/scratchpad/notes2/<JOB>.md`.
First line: `# <JOB>: reviewed N functions, M findings`. Then one line per finding:
`sub_XXXXXX line N | H/M/L | <quoted code> | <what differs between 60 and 850 fps, with numbers> | <fix in per-second terms>`
H = clearly changes behaviour in the scenario; M = plausible; L = unlikely. Final reply to me: counts and the H
findings only, max 15 lines.
