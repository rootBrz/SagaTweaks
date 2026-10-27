# How SagaTweaks was reverse engineered

Personal notes: the methods, the tools, and how each feature was found. All addresses are for the
unpacked `LEGOStarWarsSaga.exe` (image base `0x400000`, no ASLR). Names like `sub_4FD050` are
IDA's auto names; I never had symbols, every meaning below was inferred.

---

## 1. The working loop

Everything ran as a tight edit / deploy / observe loop against the real game on the handheld,
driven from my PC over SSH.

```
edit src/*.c -> make -> scp SagaTweaks.asi -> kill + relaunch game -> drive it -> read log / screenshot
```

* **Build**: `i686-w64-mingw32-gcc`, one C file, no runtime dependencies. 32-bit because the game is.
* **Deploy script** (piped with `ssh deck@... 'bash -s' < script.sh`, because the remote shell is fish):
  kill the game, move the new `.asi` into place, delete the log, relaunch through
  `steam steam://rungameid/32440`, wait, `tail` the log. Never `pkill -f` over ssh: it matches the
  ssh shell's own command line and kills the session.
* **The mod writes its own log** (`SagaTweaks.log`, flushed per line). That is the main
  observability channel: one-line state dumps, "hook installed", and temporary traces. Cheap,
  survives crashes, and a log that stops mid-line tells you exactly which call crashed.
* **Driving the game**: a fake keyboard through `/dev/uinput` (Python, `struct.pack` of
  `input_event`s, hold times per key). The Deck user can open `/dev/uinput`, no root needed.
  I scripted whole menu walks (new game, walk to a door, choose chapter). Caveat learned the hard
  way: if the game is not the focused window the keys go to the Steam UI, so check the screenshot
  and that the process exists before sending input.
* **Looking at the game**: gamescope hides the window from X11 capture (`x11grab` is black), so
  screenshots come from PipeWire:
  `gst-launch-1.0 pipewiresrc target-object=gamescope num-buffers=3 ! videoconvert ! videoscale ! video/x-raw,format=RGB,width=1280,height=720 ! filesink location=/tmp/cap.raw`
  then `ffmpeg -f rawvideo` to PNG (the box has no PNG/JPEG encoders in gst).
  Frames are delivered late, so a screenshot is "a recent frame", never "this exact moment".
* **Frame-accurate analysis**: record raw RGB frames (`filesink`, 320x180, hundreds of frames in
  one file), load with numpy and look at per-frame mean brightness. That is how I verified the
  "black while skipping, then wipe" behaviour numerically instead of by eye (section 6).

## 2. Getting analysable code out of a packed exe

The Steam exe is wrapped by **SteamStub** (a `.bind` section). On disk `.text` is encrypted, so
static analysis of the shipped file is useless: IDA sees garbage.

Technique: let the stub do the work, then **dump the decrypted image from the running process**.

1. Start the game, find the Wine process, read `/proc/<pid>/mem` for the `.text` range
   (a few lines of Python: `open(...,"rb")`, `seek(addr)`, `read(n)`). gdb attach works too.
2. Patch those bytes into a copy of the exe at the same file offsets
   (`orig/LEGOStarWarsSaga_unp.exe`). The header/section layout is unchanged, so the file stays a
   valid PE and IDA analyses it normally (imports, strings and data come from the original).
3. Open that copy in IDA (Hex-Rays decompiler on). Because the process has a fixed base
   (`0x400000`) every address in IDA equals the address in the live process, which is what makes
   the rest of the workflow possible: an address read in the decompiler can be poked in the live
   process unchanged.

I drove IDA through an MCP server (`decompile`, `xrefs_to`, `find_bytes`, `disasm`, `get_bytes`,
`insn_query`, `list_funcs`, `survey_binary`). Sessions die when idle, so "Worker not reachable"
just means re-open the database.

Pitfall: the *runtime* data also differs from the file. Example: the menu table has 31 entries in
the dumped file but 54 at run time (game init appends more), so some patches had to be applied
lazily from the frame hook once the table is complete.

## 3. Getting code into the process

* **Loader**: any ASI loader works; the mod is a plain DLL renamed `.asi`. I started with a
  ~70-line `dinput8.dll` proxy (export `DirectInput8Create`, forward it to the real system DLL,
  `LoadLibrary` every `*.asi` on attach) and later dropped it from the repo in favour of
  [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader). Wine/Proton only loads a
  native `dinput8.dll` if told to: `WINEDLLOVERRIDES="dinput8=n,b"` (or the same override in the
  prefix `user.reg`). The game already imports dinput8, so no injector is needed.
* **Timing**: the mod's DLL loads *before* SteamStub has decrypted `.text`. `DllMain` only starts a
  thread; the thread polls until the bytes at several known addresses equal the expected values
  (`Matches(addr, expected, len)`), then installs everything. The same compare doubles as a
  version check: wrong exe, no patches.
* **Patching primitive**: `VirtualProtect` RWX, `memcpy`, restore. Every patch verifies the
  original bytes first and logs success/failure.

## 4. How to find things (the actual techniques)

### 4.1 Static, in IDA

* **Strings -> code**: strings are the cheapest anchors. `"Episode_I.dat"`, `"FreePlay"`,
  `"MissionMode"`, `"WipeScreen"`, `"Buttons"` located the main loop, the level-enter menu and the
  fade code within minutes via string xrefs.
* **Data xrefs on globals** (`xrefs_to 0x951BE0`): the list of functions touching a global tells
  you what the global means. `dword_951BE0` is read by ~50 functions that all say "if a cutscene is
  playing then don't draw the HUD / don't take input", so it is "cutscene active".
* **Byte-pattern search for stores**: the decompiler lists readers easily, writers are what matter.
  `find_bytes "D9 1D 80 07 95 00"` (`fstp dword ptr [0x950780]`) found the five instructions that
  *write* the wipe progress; one of them sits in the per-frame function, which is how I learned the
  frame function already force-completes wipes in some cases.
* **Decompile the caller chain**: start at the thing you can see and walk up. Menu row ->
  draw function -> table entry -> table base. Main loop `sub_492B40` (the `do/while` with the
  inner frame loop) explained load / transition / fade ordering that no single small function did.
* **Struct recovery by access pattern**: offsets in the pseudo-code (`*(int*)(game+296)`,
  `*(short*)(inst+236)`, `*(_DWORD*)(menu+0x5C)`) are enough to write a reliable struct in the mod
  without ever knowing the original type; I only name the fields I use.
* **Calling conventions matter**: IDA's header line tells you (`__thiscall`, `__usercall`). I once
  called `sub_694490` (a `__thiscall`) as `__cdecl` from a log helper; `ecx` held garbage and the
  game crashed on the first skip. Check before you call anything you did not write.

### 4.2 Static, scripted (`tools/`)

* `scan_framerate.py` (capstone + pefile): disassembles `.text` and reports three classes of
  frame-rate assumptions: globals incremented by a constant (per-frame counters), globals used in
  modulo / mask expressions (blink, alternate-frame logic), and uses of 1/60, 60.0, 30.0 style
  constants. `scan_report.txt` is its output. This is how the clock counters were found without
  playing the whole game.
* `find_disp.py`: "list every instruction that uses displacement `+0xNN`", grouped by function,
  to find users of a struct field when the base register varies.

### 4.3 Dynamic

* **A/B memory sampling** (`ab_sample.py`, `ab_compare.py`): snapshot the whole data range of the
  live process twice, 2 s apart, and diff. Do it at 120 fps and again at 60 fps. Any dword whose
  rate of change doubles at 120 fps is a per-frame counter. That is evidence, not guesswork, and
  it ruled things in and out (menu blink turned out to be time based; the clock counters were not).
* **Hardware watchpoints in gdb** (`gdb -p <pid> -batch -x cmds`):
  `watch *(int*)0x951B80` plus `commands ... bt 6; x/12i $eip-30; continue` prints every writer of
  a variable with a backtrace. Used to learn who sets the "next level" pointer during a cutscene
  transition. Hardware watchpoints are free (no single-stepping), so they are safe on a running game.
* **/proc/pid/mem peeking** (`cs.py`, `mi.py`, `an2.py`): tiny Python scripts that dump a set of
  globals or a structure (menu stack, playing animations, device table) from the live process.
  First tool I reach for to answer "what is this variable *right now*".
* **In-mod tracing**: because the mod is inside the process it can log what the game does.
  Earlier builds had `TraceRT` / `TracePasses` / `SkipPass` (log D3D9 calls, bisect the render passes by skipping
  pass N); `DumpTextures` writes textures to TGA. Per-frame state lines during
  a skip (`wipe 0.897 dir -1.333 pend 0/0 be0 0`) were what finally showed the whole transition.
* **Experiments with a falsifiable prediction**: write the prediction down, change one thing,
  look. Example: "if the gap is frame-count driven, stepping the game 8x per frame will shorten
  it" (it did not: the gap was a blocking level load).

## 5. Hooking techniques used (and when each is right)

| Technique | Used for | Why this one |
|---|---|---|
| Patch a **branch** (`jz` -> `jmp`) | boot logos (`0x4CA5E2`) | The game already has a "skip logos" path; make it unconditional. Smallest possible change. |
| Replace the **target of a call** | main loop frame call (`0x4934BF` -> `FrameHook`) | A once-per-frame entry point with all state valid. Almost every feature hangs off it. |
| Redirect **constant loads** | FPS cap | The limiter loads `85.0f` from four places; point them at the mod's own value. |
| **Inline detour** (5-byte `jmp` + trampoline) | clock update, vertical physics | Need to run before/around the original and sometimes call it back. |
| **Table entry** redirect | menus | The menu system is data driven: `[id, ?, ?, draw, update, ...]` x 28 bytes. Swap pointers, call the original from the wrapper. |
| **COM vtable** patch | Direct3D 9 | `IDirect3DDevice9` method indices: Reset 16, Present 17, CreateTexture 23, SetRenderTarget 37, SetDepthStencilSurface 39, SetViewport 47, SetTexture 65, SetScissorRect 75, SetVertexShaderConstantF 94, SetPixelShaderConstantF 109. |
| Replace a **callback pointer** | button glyph label builder (`dword_29755D8`) | The game stores a function pointer; swap it. |
| Patch an **immediate** | text splitter lower bound `0B1h` -> `0A1h` | One byte widens the accepted glyph range. |
| **Poke data** | pause owner `dword_802AF8` | State bug, not logic: fix the value. |
| Rewrite **heap strings** from a thread | join prompt text | Strings come from loaded text data, not `.data`; scan memory, edit in place, restore when option is off. |

Rules I followed: verify bytes before patching; keep every patch independent and individually
logged; prefer calling the game's own function with the game's own arguments over reimplementing it
(menu push `sub_509830(menu, flag, -1)`, device detection `sub_6D9120`, drop-out `sub_427CD0`).

## 6. Feature by feature

### Boot logos and title screen
Decompiled the sequence function `sub_4CA5B0`; it already tests `dword_87B51C` to skip the logos.
Patched that branch. The "Press any button" screen is menu 0 on the menu stack with its own update
function; when the titles level is running and the stack depth is 1, `FrameHook` calls the same
`sub_509830(MENU_MAIN, ...)` the title screen calls on a button press, and sets the title clock
`flt_92505C` to 4.0 so the main menu options are already faded in.

### FPS unlock and refresh rate
Frame limiter `sub_4FD050` targets `max(refresh, 85.0)` using constants at `0x75EC2C/30`; four
loads redirected. Separately the video mode table (`0x2976BE4`, 16-byte entries) is all 60 Hz and
`CreateDevice` (`sub_6E3940`) copies the entry's refresh into the present parameters (`0x6E3AE8`).
A stub overrides that with the desktop refresh and the table entries are rewritten so the Video
Settings screen shows the real number. Verified by an in-mod fps counter (`LogFPS`).

### Frame-rate independence
Two separate causes found by the scan + A/B tools: (1) clock objects with a frame counter at
`clock+12`, updated by `sub_548FE0`; detoured (`ClockUpdate`) so counters advance at 60/s of real
time; (2) vertical movement. First attempt: gate `sub_4ABAC0` (vertical velocity) and the height
integration to 60 Hz ticks with an accumulator. It fixed the apex but was wrong in two ways found later:
the committed height still advances every frame by `vy * real dt`, so a jump impulse landing mid-tick lost up to
`speed / fps` of height (random +/- 0.02 of 0.46 at 120 fps), and the gated gravity made the hero slowly slide
on slopes at 850 fps (drift 0.015 units/s, exactly 0 with the gating off). The gating was removed.

What actually breaks at high fps (jump `vy` is `player+108`, committed height `+96`, candidate `+132`):
* The jump is `vy = v0` (2.3) set at the end of a frame; the next frame lifts the player by `v0 * dt`:
  0.038 at 60 fps, 0.002 at 850 fps. On a platform (the pallet in Speeder Showdown) the contact test
  tolerates that, lands the player again and resets `vy` to 0: the jump is cancelled one frame after it
  started (17 of 18 scripted jumps failed; the traced frame sequence was `vy 2.30, f641 0` then `vy 0, f641 2`).
  Ordinary ground does not have this check, which is why only platforms failed.
* With real-time integration the apex is `v^2 / 2g + v dt / 2` instead of the 60 fps value 0.4616
  (4 % lower at 850 fps).
Fix (`JumpFix`): when `vy` jumps up by >= 1 from <= 0.5 (at most once per 0.3 s, only above 63 fps) the
player is lifted at once by `v0/60 - v dt` (so after one frame it sits at the 60 fps first-frame height) and `v`
is solved from `lift + v^2/2g + v dt/2 = apex60` with the character's own gravity (`*(*(player+84)+36)+36`).
Verified: 8/8 jumps from the pallet at 0.46 (60 fps reference 0.4616), 0/6 without; ground jumps 0.459-0.460.
Test setup: write the level entry pointer into `dword_951B80` to load a level directly, poke the jump bit into
the controller (`player+3236`, +4/+8) and poll `player+96`. `LiveIni=1` re-reads `FixJumpPhysics`,
`FixFrameCounters` and `MaxFPS` once a second so the fixes can be flipped without restarting.

### Per-call systems in the logic update (grass, wind, wobble, hero damping)
The logic update `sub_491D50` is written as "one call = 1/60 s" in many places, not only the two causes above.
At 850 fps the grass wobbled about 14x too fast (the user's words: "constant wobble even when I stand still"), and
with `LogicHz=60` (logic update at most 60x per second) it was right, which proved the culprit is inside
the update and not in drawing. Finding it:

* A/B memory diff (`tools/ab_sample.py`, snapshots of `0x7EF000-0x29AB000` two seconds apart, uncapped vs
  `LogicHz=60`) lists every global that changes once per logic call: `0x2962B48` (effect timeline),
  `0x296205C/2060` (effect clocks), `0x2983A40/A44/A48` and `0x2983A4C/A54/A58` (wind and sway phase of the
  grass), audio spawn counters and housekeeping round-robins.
* Complete audit: IDA Hex-Rays pseudocode of every function reachable from `sub_491D50` (1626 functions) was split into
  77 shards of about 1300 lines and read by reviewers (one Haiku agent per shard, brief in `AUDIT_TASK.md`),
  who wrote one finding per line: location, quoted code, why it depends on the call rate, per-second form.
  Regex detectors were tried first and rejected: they flag every `+=` and say nothing about units. Every
  claim was then checked against the code and the live value before it was patched; many were false (below).

Foliage, all called once per frame from `sub_491D50` (call sites `0x4922AD..0x49233B`):
* `sub_546380` / `sub_545A80`: `dword_2983A40/A48/A44 += 501/133/377` and `dword_2983A4C/A54/A58 += 501/133/377`
  (wind and sway phase) plus lerps with literal factors 0.2 and 0.05.
* `sub_66CC60`: the wobble. Unit-step Euler spring (`v += -k x ... ; x += v`), countdown timers in calls
  (`--word`, set to 128 = 2.1 s at 60 fps).
* `sub_544420`: grass chain contact with a cooldown of 5 calls.
* `sub_686950(1.0)`: the effect timeline, `dword_2962B48 += (int)a1`.
* `dword_97390C` (`sub_60AFF0`, `add dword_97390C, 1` at `0x60B551`): wind phase read by `sub_5EC3D0`.

They are scripted at fixed steps, so the faithful fix is to keep the step and call each one as often as at 60 fps:
`ScriptTick` (the patched call at `0x4922AD`) accumulates `dt * 60`, takes `n = (int)(acc + 0.05)` and stores it
in `g_steps`; `sub_686950` gets `n`, and the call sites of `sub_544420`, `sub_546380`, `sub_545A80` and
`sub_66CC60` are redirected to wrappers that call the original `g_steps` times (0 on frames where less than
1/60 s has passed, 2 or more below 30 fps). The wind phase at `0x60B551` has its own 60/s accumulator. Below
60 fps `g_steps` is 1 on average and the game behaves as before.

Hero physics, per-call constants that were multiplied or added every call (all x87 constant operands, so the
operand address is patched to a variable `c^min(1, dt*60)`, updated every logic frame in
`UpdateCollisionTolerance`):
* `sub_68EE40` step-up hit: vertical velocity `*= 0.75` (`0x68F5C5`). Velocity died 14x faster than at 60 fps
  while the hero pushes up the rounded shoulder of the fallen tree.
* `sub_4AB620`: speed multiplier `+3140 *= 0.5` (`0x4AB754`).
* `sub_4AC620`: platform terms `+3864` and `+3872` halved (`0x4AEB87`, one shared constant load) and multiplied
  by `1 - blend` where `+3480` is the time since landing (`0x4AEB53`; replaced by a stub that stores
  `(1 - blend)^min(1,dt*60)`, flags and caller-saved registers preserved because `je` at `0x4AEB7B` still uses the
  `cmp al,2` from before).
* `sub_550100` (heading approach): `rate * dt` was truncated to int by `sub_6E6B90`, so a turn slower than
  `fps` units/s never happened and every step lost up to one unit. The call at `0x550121` returns the integer part
  plus 1 with probability equal to the fraction (`DitherTruncate`), so the mean turn rate is exact.
* Collision (earlier): `veryClose` is a fraction of the step, so its constant is scaled up as the step shrinks; seven
  fixed push-out doubles (`0.0003 .. 0.01`, `.rdata`) are scaled by `dt*60`; the velocity rebuilt after a sweep
  (`0x4178D6`) is blended with the previous one (steady state = 60 fps).

Looked suspicious, were fine (do not chase again):
* `sub_434A20` `(dword_925068 + id) % v113`: `dword_925068` is the frame counter already advanced at 60/s by `ClockUpdate`.
* `sub_4ABAC0` `flt_925018 < 0.016666668`: both formulas give the 60 fps result below 1/60 s.
* `sub_678410`: `velocity *= work+88` is a transform into ellipsoid space (`+88 = 1/radius`), not drag. `sub_678580`
  subtracts `+160` (the sweep tolerance fraction), not a timer.
* `sub_550070`, `sub_54FB40`, `sub_54FB80`, `sub_54FAD0` look like fixed-factor smoothers but multiply the factor by
  `flt_925018` inside.
* `sub_690610`: see the next paragraph.

The whole-loop audit that produced the list above (how the data is built, the reviewer briefs, the false leads and the
still open log-climb problem) is described in `docs/AUDIT.md`.

Test setup for all of this: `LogicHz=0` (logic every frame), `VSync=0`, `RenderScale=0.5`, level loaded via
`dword_951B80` as above, `LiveIni=1` to flip `FixFrameCounters` / `LogicHz` without restarting.

A wrong turn worth remembering: `sub_690610` (the 0x65xxxx-0x69xxxx "grass" cluster is the particle/effect
system and its editor) looks like a per-frame update with the constant `flt_8108E4` = 1/60 in the data
section, so I gated it to 60 Hz. But the game rewrites `flt_8108E4` with the real frame time every frame, so it
was already correct, and the gate made effects run slow at high fps. Check the live value, not the initial one.
The menu highlight blink turned out to be time based
already.

### VSync and window mode
`sub_6E3940` (device create / reset; Accept in Video Settings calls it) fills the present parameters:
`PresentationInterval` is the constant 1 (`mov [esi+16Ch], ebx` at `0x6E3A19`, redirected to a stub
reading `g_present_interval`) and `Windowed` comes from the byte at `device+65Ch`. With a windowed device
and argument 1 it moves the window to the globals `nWidth, nHeight, X, Y` (`0x82647C`, in that order;
X/Y < 0 centre). The style (`0xCF0000` overlapped, else popup) is picked at window creation, so a mode
change sets it with `SetWindowLong`, fills those globals, sets `+65Ch` and calls the same reset.
Borderless = windowed device + popup window at the desktop size. Menu rows reuse the Render Scale row
mechanism (three rows drawn before Back, update wrapper maps Back to row 4 for the original code).
Mistake worth remembering: I first assumed the globals were `X, Y, nWidth, nHeight` and overwrote other
settings; check the order in the data, not the decompiler's variable names.

### Render scale
The scale is a **backbuffer size**: `sub_6E3940` (device create / reset) copies the video mode's width and height into the
present parameters (`+138h`, `+13Ch`); the copy at `0x6E3AD7` is replaced by a call that multiplies them when the device
is windowed (clamped to 8192 per side). The game then runs at the larger size by itself, so every render target, depth
buffer, shader constant and the HUD agree, and Present stretches the backbuffer to the window (any windowed D3D9 does;
exclusive fullscreen cannot, so the Window Mode row skips Fullscreen while the scale is not 100 %). Changing the scale
resets the device through the same routine as a window mode change.

The game treats the backbuffer size as the window size in three places, and these get the unscaled size: the mode lookup
`sub_6E1B70` (windowed, it writes the size it is given into the mode table, which showed the multiplied resolution in Video
Settings and multiplied it again on every reset) at `0x6E3FC7`, and the saved resolution (`sub_6DAEF0` / `sub_6DAF00`,
`0x6E409A` / `0x6E40AB`).

History: the first implementation kept the game at the native size and redirected pieces: the three effect render targets
(`0x605813` ...), a depth surface twin, the `c45` screen constant, viewport / scissor rescaling and a scaled virtual
backbuffer for the HUD pass. Every effect that reads "the screen" its own way (pause snapshot, heat distortion, shadows
on the pause screen) was another place where the sizes could disagree, and it was removed in favour of the backbuffer size.
Lessons that still apply: the game saves and restores the surfaces it gets from `GetRenderTarget` / `GetDepthStencilSurface`;
on pause it copies the swap chain's backbuffer into a snapshot (`StretchRect` in the middle of a frame) and draws that under
the menu; the paused background is shifted by 3-5 px against the running game even at 100 %, which is the game's own behaviour.

### Menus: Render Scale row, Tweaks page, hidden FSAA row
The menu system: table at `0x800AA8`; menu stack depth `0x800944`; menu objects at
`0x921440 + 224 * depth` with the current row (`+10`), row count (`+18`), left/right/select
flags (`+0x5C/0x60/0x64`). A page = a draw function + an update function. Row helpers:
`sub_50F740(menu, text, alpha)` (draws and counts a row), `sub_574DE0` for the pause menu.
* Render Scale row: the call that draws the *Back* row of Video Settings (`0x5120E6`) is redirected
  to a stub that draws the extra row first; the update wrapper presents "row 5 = Back" to the
  original as "row 4".
* Tweaks page: a spare table entry (`0x406`) gets my draw/update; rows are plain `sub_50F740` calls.
  Entries for main/pause menu are patched so Tweaks sits before Quit (the Quit-row call is replaced
  by a stub that draws Tweaks then Quit; an index map sends the shifted row back to the original
  handler). The main menu fades its rows in; Tweaks uses the same alpha (`dword_921D60[0]`) so it
  fades with them.
* FSAA row removal: the row-draw call (`0x51242C`) becomes a no-op and the cursor row is shifted
  by one in the wrapper.

### Input
Slots (`dword_297E044[3*slot]` device, `...48` assigned), raw device ids 0-9 pads / 10-13 keyboard
layouts, device records at `0x917000 + 132*dev`. The game's own "which device is pressed"
routine `sub_6D9120` is reused for hot-switching. Bugs found with state dumps in the log:
* held key re-took P1 every frame (log showed ~1700 "switched" lines) -> edge-triggered detection;
* P2 pad disappears -> the game parks slot 1 on keyboard layout 13 and leaves it as owner of the
  pause menu (`dword_802AF8`), so P1's pad controls nothing. Release the slot, clear the owner;
  hook the device re-seat `sub_6DA970` so a lost pad is not handed a keyboard;
* "join with Start": the join code calls `sub_6D9120` at `0x4280D3`; hooking that call lets only
  Start (code 7) through. The "Press F2 or any button" text lives in heap memory, rewritten in place.

### Button prompts
Prompts are glyphs in a font called "Buttons": a 256x128 DXT5 atlas, glyph table (12-byte records
x,y,w) at `font+0x34`, code table at `font+0x38`. The mod builds a 256x256 atlas (Xbox art on top,
procedurally drawn PlayStation art below), swaps the texture at `SetTexture`, adds glyphs at
U+0521..0528, and patches the splitter's lower bound so they are recognised as glyphs. The label
callback learns which device the *player* uses (hook on `sub_6D87F0`), so P1 can show Xbox and P2
DualShock at once. First attempt used U+0541+ and rendered tofu: the splitter only accepted
`D4 B1..BF`, which I found by reading `sub_50C780`.

### Cutscene skip (the long one)
Notes because it took many wrong turns:
1. Found "cutscene active" `dword_951BE0` and the running instance `dword_951BE8` (`+4` = animation
   object). Tried to jump the animation time: wrong fields, nothing happened.
2. Fast-forwarding (stepping the frame 8-12x with a big dt) "worked" but is not a skip, and it left
   gameplay sped up for ~1.5 s after the end. Rejected.
3. Decompiled `sub_57EF50` (cutscene update): an animation is finished when bit `0x1000` of its flag
   dword (`anim+136`) is set (`sub_500210`). Setting that flag on every playing instance makes the
   game run its *own* ending code, including chaining to the next part (`inst+236`).
4. Level handover is in the main loop (`sub_492B40`): the inner frame loop exits when a wipe
   (`flt_950780`, 0..1) reaches 1.0 with a transition pending (`dword_951B80`/`87658C`), then the
   outer loop loads the next level (blocking, ~0.2 s) and wipes it in (`flt_950780` falls 1 -> 0
   at 1.333/s).
5. Per-present logging during a skip showed exactly that sequence. The visible "3 frames of the
   next cutscene" were the wipe-in. Fix: while a skip window is open, force the wipe to 0 as soon as
   nothing is pending, keep the screen black (`ColorFill` in the `Present` hook) until then, and end
   the window after 200 ms of "nothing pending, no wipe, no cutscene".
6. The remaining wait is the real level load; it cannot be skipped, only hidden.

7. (1.1.6) Skip = move each part's animation time to `length - 1 - 1.5 * step` (`anim+144`, data `anim+88`,
   `+8` = length) and let the game end it; marking it finished (`0x1000`) skips the completion callback
   (`anim+180`), which is what makes things like Jar Jar joining happen. The screen wipe is the transition
   manager at `0x95077C` (`+0` style bits, `+4` progress, `+8` rate, `+36` id): capture the picture with
   `byte_950740`, `sub_5B1220(&2)` + `sub_5B11B0(1)` start a wipe-in that sweeps the captured picture away;
   hold it with rate 0 (and rewrite the style, the end of a cutscene clears it) until gameplay is back.

Verification trick: brightness-per-frame from raw capture proved no non-black frame is presented
between pressing Start and gameplay.

## 7. Mistakes worth remembering

* Calling a `__thiscall` as `__cdecl` (crash). Always read the signature line.
* Trusting guessed struct offsets (cutscene time) instead of finding a consumer of the field first.
* "It works on my test" is not "it works": the first cutscene fix passed on the intro I used and
  failed on the user's. Log enough state to tell a skipped scene from a lucky one.
* Latching state with a timer ("keep skipping for 1.5 s") hides bugs and creates new ones. State
  based exit conditions (pending flags, wipe value) are better.
* Debug code that touches freed objects (`DumpTextures`) crashes after a while; keep such tools
  behind ini flags, default off.
* Screenshots lie about timing; numbers from logs and raw frame captures do not.
* Ask before experiments that can take down the user's desktop session (a test dxvk config
  was refused, rightly).

## 8. Address cheat sheet

| What | Address |
|---|---|
| Main loop (`sub_492B40`), frame call | `0x492B40`, call at `0x4934BF` -> `sub_491D50` |
| Game object ptr / current level | `0x802C54` / `*(game+296)` |
| Titles level ptr, menu depth, open menu | `0x951B88`, `0x800944`, `sub_509830` |
| Menu table | `0x800AA8` (stride 28), objects `0x921440 + 224*depth` |
| Frame dt | `flt_925018` |
| Frame limiter | `sub_4FD050`, consts `0x75EC2C/30` |
| Cutscene: active / instance / list | `0x951BE0` / `0x951BE8` / `*(game+10928)` |
| Cutscene update / end | `sub_57EF50` / `sub_57EEC0` |
| Wipe progress / speed / style | `0x950780` / `0x950784` / `0x95077C` |
| Next-level request | `0x951B80`, `0x87658C` |
| Input slots | `0x297E044`, `0x297E048`, per-slot input `0x9253B0 + 96*slot` |
| Pause menu owner | `0x802AF8` |
| Device detect / re-seat | `sub_6D9120` / `sub_6DA970` |
| Buttons font | `dword_921EA4` |

## 9. Doing it again for a new feature

1. Write down what you can observe in the running game (a string, a number, a visual).
2. Find an anchor: string xref, a global the symptom touches, or a diff of memory with and without
   the symptom (A/B sampling).
3. Decompile upward until you see who decides; find the cheapest place to change the decision.
4. Prefer calling the game's own routines; patch the smallest thing; verify bytes first.
5. Log state around it from inside the mod, run, look at numbers, then at pixels.
6. Test the unhappy paths (other chapter, other device, held key, device unplugged).

## Tried and dropped: fixed 60 Hz logic with interpolated drawing

Logic at exactly 1/60 s per step plus blending the c16 world x view x projection matrix of each mesh draw between the
last two steps was built and tested on the Deck (Speeder Showdown, 850 fps). The user judged the picture "really bad" and
the log was climbable with the variable step anyway, so it was removed. Findings worth keeping: every mesh draw is
preceded by one `SetVertexShaderConstantF(16, 4)` (the matrix), then `40x1`, `28x11` (bones), `111x1` per draw; at 60 fps
all 10 jumps rose to the full height while at 850 fps they split into 0.50 (fail) and 0.535 (success) rises.
