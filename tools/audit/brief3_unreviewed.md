# Frame-rate dependence audit, round 3 (functions that read dt but were never reviewed) — instructions for each reviewer

CONTEXT. We are reverse engineering LEGO Star Wars: The Complete Saga (2007 PC). IDA/Hex-Rays pseudocode is in
`/tmp/claude-1000/-home-alan-lego-sw-cs/6888468d-58f1-4759-a2da-2d818d89ba3d/scratchpad/shards2/compact/<hexaddr>.c`
(one function per file, blank lines and local variable declarations removed). The game logic was written for ~60 fps. We now run
it at 800+ fps and want to find every piece of code whose behaviour depends on the frame rate.
`flt_925018` is the global "seconds elapsed during the last frame" (dt). A float function parameter (a1/a2/a3) is often dt or
dt*60 passed by the caller. IDA names are generic (sub_XXXXXX, dword_XXXXXX, v12 ...); field offsets like `*(float *)(a1 + 108)`
are struct members of an entity.

YOUR SHARD. Your prompt names a shard file `shards2/shard_NN.txt`. Each line is `<file> <first_line> <last_line>`:
read that line range of `shards2/compact/<file>` COMPLETELY (use the Read tool with offset/limit; do not skim, do not skip
functions, do not grep only). Every function in your shard must be read.

WHAT TO LOOK FOR (report only real findings):
 1. State that changes by a FIXED amount per call/frame instead of per second: per-call counters used as timers or thresholds
    (`if (++x > 5)`, `x = x - 1` then compare), additions/subtractions of literal constants to positions, velocities, timers, phases,
    angles (nudges, push-outs, `x += 0.01`), frame-count based animation.
 2. Per-call smoothing/damping/lerp with a literal factor not scaled by dt (`x = x * 0.9`, `x += (t - x) * 0.1`).
 3. dt used in a way that is not linear/safe: truncated to int (`(int)(rate * dt)`, `sub_6E6B90(...)` is float->int), clamped or
    compared against a constant, squared, divided by, multiplied twice, `min(dt, const)`.
 4. Random numbers (sub_5B9DD0, sub_54FA60, sub_5B9E30) added/applied on every call (jitter, random walk) without dt scaling.
 5. Code that assumes 1/60 s: literals 60.0, 0.01666, 30.0, 0.0333, 16.6, or calls like fn(1.0) where a time step is expected.
 6. Integration in the wrong order, or values re-derived from position deltas divided by dt (`(pos - old) / dt`).
 7. Anything else you can argue changes real-time speed when the call rate goes from 60 to 900 per second.
IGNORE: loop counters, array indices, pool allocation, flag handling, one-shot/event code, rendering/state setup, menus.
EXTRA CONTEXT. These 228 functions read the frame time but were missed by the earlier audits (they are reached through
function-pointer tables: per-character-class callbacks `(*(*(hero+84)+24))(hero)`, per-animation state handlers, etc.).
Many are probably hero (player) state handlers. The bug we chase: in Episode VI Speeder Showdown the hero pushes against a
fallen log (a wall with a rounded top), is lifted by the step-up logic, then jumps; at 850 fps the jump usually ends
below the log top, at 60 fps it works every time. Position is hero+92/96/100, velocity +104/108/112, target velocity
+3864/+3872 (velocity eases to it), ground flags +641, flags +508. So pay special attention to anything that touches the hero
velocity, position, jump, ground/wall state, step-up, air control, facing, input-to-velocity, or animation-driven movement.
If the output viewer shows "... N sections elided (caveman) ..." and a handle `<<ccr:ccr_xxx>>`, recover the hidden text with
`mcp__caveman__caveman_retrieve` (load via ToolSearch `select:mcp__caveman__caveman_retrieve`; pass the handle, no query).
Never judge code you only saw elided. Helpers sub_54FB40 / sub_550070 / sub_550100 / sub_54FB80 multiply by dt inside (fine).
ALREADY HANDLED, do not report: collision tolerances and push-outs (sub_6786F0, sub_677FB0, sub_678010, sub_678F60, sub_68F940),
the velocity rebuild in sub_416D60, gravity in sub_4ABAC0, jump impulse, wind phase dword_97390C, script tick sub_686950,
effect step flt_8108E4, the 0.75/0.5 per-call decays in sub_68EE40/sub_4AB620/sub_4AC620, int truncation in sub_550070/sub_550100, grass/wind/wobble systems.

OUTPUT. Write `/tmp/claude-1000/-home-alan-lego-sw-cs/6888468d-58f1-4759-a2da-2d818d89ba3d/scratchpad/notes3/shard_NN.md`
(NN = your shard number). First line: `# shard NN: reviewed <count> functions, <count> findings`. Then ONE line per finding:
`sub_XXXXXX line N | H/M/L | <quoted code> | <why it depends on frame rate> | <how to make it per-second>`
Severity: H = visible speed of movement/animation/timers will differ at high fps; M = likely; L = possible/minor.
If a function has no finding do not list it. Be precise and conservative: read the surrounding code before reporting; do not
guess from names. Your final reply to the caller: the number of functions reviewed and the H findings only (max 15 lines).
