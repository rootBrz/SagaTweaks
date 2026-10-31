# Gizmo classes and the "per-frame roll" bugs

How the game's level objects ("gizmos") are updated and drawn, a map of the 28 gizmo classes, and the method that found the
lever wobble (1.3). Read this first when an object misbehaves at high frame rates. Addresses are those of the unpacked exe
(`orig/LEGOStarWarsSaga_unp.exe`, see `REVERSING.md`).

## 1. How gizmo classes are dispatched

`dword_93B268` points to the class registry: `+0` class count (28), `+8` the table. An entry is 156 bytes: `+0` name,
`+52` / `+56` / `+60` three hook pointers (any may be 0). Three dispatchers walk the table and call the hook of every class
with `(game, class data, dt)`:

| hook | dispatcher | called from | when |
|---|---|---|---|
| `+52` "pre" | `sub_559C60` | `sub_491D50` at `0x492328` | game update, early |
| `+56` "update" | `sub_559CD0` | `sub_491D50` at `0x4924FC` | game update, late (after the animation step `sub_5F4B90`) |
| `+60` "render prep" | `sub_559D40` | `sub_4927F0` at `0x492995` | **once per rendered frame**, outside the game update |

The class data of a class is `*(*(game + off))`: records of 160 bytes (obstacles, levers, hat machine) or 772 bytes (spinner)
in a list whose pointer and count live in the game object (`game + 20584 / 20588` for levers, `+ 20596` for the hat machine,
`+ 18144` for spinners, `*(game + 10952)` for the obstacle groups).

The main loop is `sub_492B40`. One iteration of its inner loop (call `0x4934BF` is the game update `sub_491D50`):
timing / input, `sub_491D50` (the game update, what `LogicHz` caps), then `sub_4927F0` (scene build: calls the render-prep
hooks above and queues the draw lists), `sub_4FD050` (frame limiter, which also calls `sub_60B5B0` -> `sub_60AFF0`: renders
the queued scene and presents). Consequences:

* `LogicHz` and every "call gate" inside `sub_491D50` only reach the `+52` / `+56` hooks. A render-prep (`+60`) hook runs
  once per **rendered** frame, so at 800 fps it runs 13 times as often as at 60 whatever `LogicHz` says. That is why the lever
  wobble could not be fixed by capping the logic rate.
* Skipping `sub_4927F0` (or anything that fills the draw lists) on a frame still presents: the world goes black while the HUD
  stays (HUD is drawn elsewhere). Do not gate the scene build when bisecting by eye.

## 2. Class table

Read from the running game with `tools/audit/class_map.py dump`.

| # | class | pre `+52` | update `+56` | render prep `+60` | notes |
|---|---|---|---|---|---|
| 0 | GizObstacle | - | `sub_558070` | `sub_558670` | floor buttons (types 1, 2, 5, 6), pulled switch / door objects (type 0, `sub_5593F0`), pushable-cart triggers; 160-byte records, list per group at `game+10952`, update `sub_558070`; buttons `sub_557B90` (section 15) |
| 1 | GizBuildit | `sub_590610` | `sub_5912E0` | `sub_5907B0` | loose building-block pieces; update `sub_5912E0`; roll fixed (`0x591F2B`) |
| 2 | GizForce | - | `sub_468B50` | `sub_467670` | "LegShake" objects; update `sub_468B50`, shake calls patched (`ShakeOffset`) |
| 3 | blowup | `sub_568010` | `sub_5684F0` | `sub_56B160` |  |
| 4 | GizmoPickup | - | `sub_5716C0` | `sub_56F590` |  |
| 5 | Lever | - | `sub_490250` | `sub_4903F0` | **wall levers**: pole with red / green light you pull; 160-byte records at `game+20584` (count `+20588`); render prep `sub_4903F0`; wobble roll fixed (`0x49069A`); sound "SwLever" |
| 6 | Spinner | `sub_5568E0` | - | `sub_554630` | cranks / wheels you turn (772-byte records at `game+18144`, hero state `dword_802850`, `sub_556190`, `sub_554E30`); the spark roll in `sub_5568E0` fixed (`0x556A95`) |
| 7 | MiniCut | - | - | - |  |
| 8 | Tube | - | `sub_56DB80` | `sub_56D5A0` |  |
| 9 | ZipUp | - | `sub_4D6FD0` | `sub_4D71A0` | zipline (`sub_4D6FD0`, rope swing, section 15) |
| 10 | GizTurret | - | `sub_5AC080` | `sub_5AD1D0` | turrets; update `sub_5AC080` rolls random at `0x5ACB98` (not checked) |
| 11 | BombGenerator | - | `sub_466330` | - |  |
| 12 | AIProcessor | - | - | - |  |
| 13 | NuSpecial | - | - | - |  |
| 14 | Message | - | - | - |  |
| 15 | GizTimer | `sub_5B0400` | - | - |  |
| 16 | GizRandom | - | - | - |  |
| 17 | Panel | - | `sub_46DF70` | `sub_46E290` |  |
| 18 | HatMachine | - | `sub_472E90` | `sub_472FE0` | hat machine (160-byte records at `game+20596`); render prep `sub_472FE0` is a copy of the lever code, roll fixed (`0x4734A9`) |
| 19 | PushBlocks | - | `sub_5A5BD0` | - | pushable blocks (the Death Star Escape cart is not one of them, it is ridden); `sub_5A5BD0` |
| 20 | Door | - | - | - |  |
| 21 | Teleport | - | - | - |  |
| 22 | Torp Machine | - | `sub_46FB80` | `sub_46FBE0` |  |
| 23 | ShadowEditor | - | - | - |  |
| 24 | Portal | - | - | - |  |
| 25 | Grapple | - | `sub_54E0C0` | `sub_54DB40` |  |
| 26 | Plug | - | `sub_5AFC90` | `sub_5AFCA0` |  |
| 27 | Techno | `sub_4CD1E0` | `sub_4CD220` | `sub_4CD2A0` |  |

## 3. The per-frame roll bug

**Pattern.** A class hook decides "does my object twitch / spark / wobble now" with
`if (sub_54FA60() < N) start_effect_for_T_seconds` on **every call**. `sub_54FA60` is the game's random function (0..65535).
At 60 calls a second the chance per second is what the artists tuned; at 850 calls a second the effect starts 14 times as
often and, because it lasts a fixed time, looks like permanent fast shaking. The effect itself is fine and time based
(`timer -= dt`), only the *start roll* is per call.

**Fix.** The roll's `call sub_54FA60` is redirected to `BuilditRoll` (`src/framerate.c`): it returns a real random number only
on frames that carry a 60 Hz tick of real time (`g_steps > 0`, same accumulator as the foliage and script counters) and a
number that always fails otherwise. The chance per second is then the 60 fps one at every frame rate. Switch:
`FixGameplayRates`.

| class | hook | roll (call site) | effect | since |
|---|---|---|---|---|
| GizBuildit | update `sub_5912E0` | `0x591F2B` | loose brick twitches 0.2 s | 1.2.0 |
| Spinner | pre `sub_5568E0` | `0x556A95` | two spark effects at the crank (about 26 / s at 850 fps) | 1.3 |
| **Lever** | render prep `sub_4903F0` | `0x49069A` | handle wobbles 0.15 s (timer at `+116`, pose by `sub_5526C0`) | 1.3 |
| HatMachine | render prep `sub_472FE0` | `0x4734A9` | same code as the lever, `+152` | 1.3 (not seen in game) |
| (hero, not a class) | `sub_59BD40`, crank held | `0x59C85A` | spark puff + sound of a crank (`sub_554D70`), 3.1 % per call | 1.3 (untested) |
| (Force Lightning) | `sub_4EA110` | `0x4EA11C`, `0x4EA43A` | screen shake 3.1 % and pad rumble 6.3 % per call | 1.3 (untested) |
| (ambient) | `sub_547D70` | `0x547D80` | rare ambient sound, 1.6 % per call after a 3 to 5 s cooldown | 1.3 (untested) |

**Class hooks checked, no further gate of this kind (1.3).** Every other roll reached from a class hook was read at the instruction
level (`tools/audit/class_map.py scan` lists them; a *gate* is `cmp eax, N / jcc` or `setl` right after the call, a *value* is
`fild` / `imul` / `idiv`):

* GizTurret update `0x5ACB98`, blowup pre via `sub_55E660`, PushBlocks update via `sub_5A54B0`, `sub_556190` (spinner pose),
  the helper `sub_46D270` (4 rolls) and the 9 rolls of `sub_468B50` (GizForce shake): all use the number as a **value** (random
  index, direction, shake amount). `sub_468B50`'s shake is applied through `ShakeOffset` (section above, 1.2.0).
* `sub_46C4F0` is a chance helper (`return random < chance`, chance from the stud / health counters). Its nine callers
  (`sub_412760`, `sub_42D2A0`, `sub_438230`, `sub_468B50`, `sub_46D780`, `sub_49C080`, `sub_5672D0`, `sub_5912E0` and one more)
  call it when an object breaks or is hit; the two class hooks among them (`0x46932C`, `0x59143A`) sit behind a one-shot flag
  (`or [flags], 0x200` / `test [flags], 0x200`), so they roll once per object, not per frame.
* `sub_5912E0` also has a 50 % roll at `0x591EB1` that decides whether the twitching brick plays its sound
  (`sub_547930(0x3C)`), but the *time* threshold `flt_9555A8` decides when it is allowed to roll, and it is re-randomized only
  after a success. The few extra rolls per frame change when a brick's sound starts by a frame or two, nothing audible.
* The random calls that run every frame in an idle scene (`rand_callers.gdb`: `0x41517F`, `0x434AC8` and friends, `0x47CF41`,
  `0x547ED3`, `0x549CAB`, `0x40AC07`, `0x4239A7`, `0x423A17`, `0x49B940`) all feed a value (a random smoothing target, idle clip,
  sway); the targets are approached with the time based `sub_54FAD0` / `sub_54FB40`, which `Approach` / `Lerp60` already scale.

**Whole exe (1.3).** The class hooks are not the only place. `tools/audit/class_map.py rolls EXE` lists every call of the
random functions in the exe (420 sites: 414 of `sub_54FA60`, the six of the C runtime `rand`) and keeps the ones whose result is
compared: 42 gates. Seven of them have a small chance (below 25 %); they are exactly the rolls in the table above, so after
1.3 **every small-chance per-call roll in the exe is gated**. The other 35 gates are 50 % coin flips (a choice, which side a
particle goes, a variant of an animation: it does not matter how soon after an event it is made), the chance helper
`sub_46C4F0` behind one-shot flags, and event code (`sub_438230` hit reactions, `sub_42D2A0` drops). The 378 other sites use the
number as a value (an offset, a direction, a stud tier); the six `rand` callers divide it by `RAND_MAX` for a particle offset.

What the scan cannot see: a gate whose compare is far from the call (the random number kept in a register or on the stack across
other code), and effects whose rate depends on a counter instead of a random number (those are the "per-call counters" of
sections 5 to 15 of `AUDIT.md`). The way to find such a thing is the same as for the lever: look at what the frame rate changes,
name the object's class, read its three hooks, count the callers of the random function (`rand_callers.gdb`).

## 4. How it was found (and what went wrong), for next time

1. **What the user sees decides the object.** "Levers hanging from the wall wobble in idle, slower when the FPS is capped,
   `LogicHz=60` does not change it". Independent of `LogicHz` means render side or a per-frame hook outside the update.
2. **Name the object from the class table, not from a guess.** The first hours went into the 772-byte "lever" records
   (`sub_5568E0`, `sub_556190`), which are the **Spinner** class (cranks). The wall lever is the class literally called
   `Lever` (sound `SwLever` in its update `sub_490250`). Run `class_map.py dump`, find the class by name,
   decompile its three hooks first.
3. **Bisect live, with the user as the detector.** A video metric of the handle (red pixel centroid, frame to frame) was
   too noisy to tell fixed from broken; only the person looking at the screen can say "slow" or "fast". So the loop was:
   thunk every call of the main loop and of `sub_4927F0` so that it runs only on a 60 Hz tick of real time, switch sites on
   and off through the ini (`LiveIni=1` re-reads once a second, no restarts), ask "slow or fast". Result: all sites gated =
   slow, the main-loop calls alone = fast, calls inside `sub_4927F0` = the scene build; the world went black when the group
   holding the scene build was gated, the lever disappeared when `sub_559D40` was gated, which pointed at the class prep
   hooks.
4. **The tool (not in the repo, rebuild it in 100 lines):** per call site `E8 rel32` of the loop, a thunk
   `cmp byte [flag_i],0 / jne +1 / ret / jmp callee` in `VirtualAlloc`ed RWX memory, the call patched to the thunk, one flag
   per site set once per frame from a bit mask (several ini words for more than 32 sites); the first call of the frame also
   computes the 60 Hz tick. Skipping with `ret` is only valid for `cdecl` callees; check every callee with a recursive
   descent for its `ret` immediate (the callees of the main loop and of `sub_4927F0` are all `ret 0`, except seven that tail
   jump and were left out). Gate nothing while no hero exists (menus and loading need every frame and key pulse).
5. **Confirm with the random callers.** `tools/audit/rand_callers.gdb`: with the game idle next to the object, a caller that
   rolls once per frame (as many `C` lines as `F` lines) is the suspect. Here `0x49069A` showed up in the very first
   count and was ignored for hours because the function around it was never looked at.

**Dead ends worth knowing (all measured, none was the cause):** lever / obstacle records and animation lists do not change in
idle; the pole's matrices sent to the vertex shader are identical every frame at 60 and 835 fps (`ProbeDraw`, with
`ProbeDrawX/Z/R` to keep only draws near a point); the pixel shader clock `flt_2857274` (+= frame time per rendered frame, pixel
shader constant 46) runs at 1.0 per second at any frame rate; making it advance in 1/60 s steps changed nothing visible and
would make shader animation step at 60 Hz on a 144 Hz screen, so it was removed; `sub_5F4B90` (animation step) is stepped with
a total of 60.0 per second at any frame rate (`ProbeAnim`).

## 5. Testing on the Deck without screenshots

Scripts are throw-away, the recipe is what matters: `/dev/uinput` keyboard (struct `80sHHHHi256i`, wait 1.5 s after creating
the device; main menu New Game = Up x3, Enter), a hero teleport by writing x, y, z to `hero+92 / 128 / 400 / 1864` with
`hero = *(int*)0x93D7F0` through `/proc/<pid>/mem` (pid from `pgrep LEGO`), `gdb -batch -p` with breakpoints that print and
continue (`kill -INT` ends it; the game sometimes dies afterwards), `LiveIni=1` to change `MaxFPS`, `LogicHz`, `FixFrameCounters`
and (while it existed) the gate mask live. A true 60 fps run needs `MaxFPS=60` **and** `RefreshRate=60` in the ini at start,
a live `MaxFPS` alone does not cap below the video mode's refresh. Windowed capture: PipeWire `gst-launch-1.0 ... pipewiresrc
target-object=gamescope` (no `videocrop` element, crop in numpy).
