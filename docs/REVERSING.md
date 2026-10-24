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
  `TraceRT` logs D3D9 calls, `TracePasses` + `SkipPass` bisect the render passes (skip pass N and
  see what disappears), `DumpTextures` writes render targets to TGA. Per-frame state lines during
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
time; (2) vertical velocity update `sub_4ABAC0` integrates with the frame dt, giving different jump
heights at different fps. A detour feeds it a dt quantised to 1/60 s steps with an accumulator
(ticks accumulate, zero-tick frames pass 0). The menu highlight blink turned out to be time based
already.

### Render scale
The game creates its effect render targets from the backbuffer size in three functions; the size
loads (`0x605813/20`, `0x605894/A3`, `0x605989/95`) are replaced by stubs that multiply. Problems
solved in order: (a) bigger RTs but the original depth buffer -> everything dynamic vanished, fixed
by a depth surface "twin" of the scaled size swapped in at `SetDepthStencilSurface`; (b) shader
constant `c45 = (w, h, 1/w, 1/h)` must match the bound target, so it is rewritten whenever a scaled
RT is bound (hooks on constant set/get); (c) the **HUD/particle pass draws straight into the real
backbuffer with an empty depth buffer**, so blue particles showed through walls at 200 %; fixed by
substituting a scaled *virtual backbuffer* (same size, shares the scaled depth) and `StretchRect`ing
it to the real one in `Present`. Found by comparing screenshots at 100 % / 200 % and bisecting the
passes with `SkipPass`.

Later bug, "characters vanish after changing the scale at run time": the game saves the bound
surfaces with `GetDepthStencilSurface` / `GetRenderTarget` and restores them later. While the scaled
depth twin was bound it saved *our twin*; when the scale changed the twin was released and replaced,
but the game kept restoring the old, smaller one, so the depth test clipped the scene and every
dynamic object disappeared (statics and shadows were fine, which made it look random). Found with
per-3-second diagnostics (`engine=` depth surface != `main=`, and `engine` equal to the *retired*
twin's address) plus a scripted soak test: a debug file (`<ini>.scale`) changes the scale at run
time while a screenshot metric (brown robe pixels in the centre) flags missing characters. Fix:
hook the two `Get*` calls and hand the game its own surfaces instead of ours (and forget the
cached surfaces on `Reset`). After that 24 random scale changes, 3.0x and 4.0x all passed.

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
