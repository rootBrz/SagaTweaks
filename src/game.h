/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "common.h"

#define ADDR_LOGO_BRANCH   0x004CA5E2u  /* jz loc_4CAD3A in the logo sequence */
#define ADDR_LOGO_SKIP_TGT 0x004CAD3Au
#define ADDR_FRAME_CALL    0x004934BFu  /* call sub_491D50 in the main loop */
#define ADDR_FRAME_FUNC    0x00491D50u

#define ADDR_OPEN_MENU     0x00509830u  /* sub_509830(menu, flag, -1): push a menu */
#define ADDR_MENU_DEPTH    0x00800944u  /* dword_800944: menu stack depth */
#define ADDR_MENU_FLAG     0x00924810u  /* dword_924810: passed as 2nd arg by the title screen */
#define MENU_MAIN          1
#define ADDR_TITLE_TIME    0x0092505Cu  /* flt_92505C: seconds since the titles level started (logo animation + option fade-in) */
#define TITLE_ANIM_END     4.0f         /* logo animation done and options fully faded in */
#define ADDR_PLAYER_ACTIVE 0x0093D516u  /* byte_93D516[16 * slot]: slot pressed a button, menu shows its options */

/* input device handling (same calls the title screen makes on a button press) */
#define ADDR_DETECT_DEVICE 0x006D9120u  /* int sub_6D9120(mask, kinds, &a, &b, skipTaken): device with a pressed input, or -1 */
#define ADDR_MAP_DEVICE    0x006D9510u  /* int sub_6D9510(raw, 0): raw device -> id used by the slot table */
#define ADDR_ASSIGN_DEVICE 0x004F47F0u  /* sub_4F47F0(slot, device) */
#define ADDR_ENABLE_DEVICE 0x004F4340u  /* sub_4F4340(device, 1) */
#define ADDR_INPUT_SAVE    0x006D6350u  /* sub_6D6350(): current input context */
#define ADDR_INPUT_SELECT  0x006D6340u  /* sub_6D6340(ctx) */
#define ADDR_SLOT_DEVICE   0x0297E044u  /* dword_297E044[3 * slot]: device bound to slot */
#define ADDR_SLOT_INPUT    0x009253B0u  /* dword_9253B0[24 * slot]: per-slot input state, +4 = buttons held */
#define ADDR_SLOT_ASSIGNED 0x0297E048u  /* dword_297E048[3 * slot]: slot has a device */
#define ADDR_PLAYER_OBJ    0x0093D7F0u  /* dword_93D7F0[slot]: player object */
#define PLAYER_ACTIVE_OFS  508          /* *(char *)(player + 508) < 0: slot is playing */
#define DETECT_KINDS       0x1A   /* what the game's own press-any-button scans check: buttons and keys */
#define DETECT_KINDS_ANALOG 0x01  /* gamepad sticks and triggers: sub_6D9120 only looks at them when this bit is set */
#define DEVICE_COUNT       14
#define FIRST_KEYBOARD_DEVICE 10   /* raw/mapped ids 0-9 are gamepads, 10-13 keyboard layouts */
#define ADDR_TITLES_LEVEL  0x00951B88u  /* dword_951B88: "titles" level entry */
#define GAME_CUR_LEVEL_OFS 296          /* *(game + 296): current level entry */

#define TITLE_SETTLE_FRAMES 2

/*
 * Frame limiter in sub_4FD050: target fps = max(display refresh, 85.0), where 85.0
 * comes from a float (0x75EC2C) and a double (0x75EC30) constant. With vsync on a
 * 120 Hz display the 85 fps target makes frames land on every other vblank (60 fps).
 * The four instructions that read those constants are pointed at our own copies.
 */
#define ADDR_LIMIT_CMP1    0x004FD222u  /* fcomp ds:dbl_75EC30 */
#define ADDR_LIMIT_LOAD1   0x004FD22Fu  /* fld   ds:flt_75EC2C */
#define ADDR_LIMIT_CMP2    0x004FD275u  /* fcomp ds:dbl_75EC30 */
#define ADDR_LIMIT_LOAD2   0x004FD282u  /* fld   ds:flt_75EC2C */
#define UNLIMITED_FPS      100000.0

/*
 * Refresh rate. The game builds its video mode list with every entry at 60 Hz and
 * sub_6E3940 copies the selected entry's refresh into the D3D present parameters
 * (mov eax,[eax+8] / mov [esi+168h],eax). That load is redirected to a stub that
 * uses RefreshRate from the ini, or the desktop refresh rate the game already
 * queried (this+610h). The mode table entries are rewritten to match so the video
 * settings screen shows the real value.
 */
/*
 * Frame counters. sub_548FE0(clock) advances a clock struct once per call:
 * time, previous time, seconds accumulator and an integer frame counter at +12.
 * Blink / flicker code takes that counter modulo something (e.g. the body
 * disappear flash in sub_4191D0: (dword_925068 & 3) >= 2), so at 120 fps it
 * flickered twice as fast. The detour keeps everything else and makes the
 * counter advance at 60 per second of real time.
 */
#define ADDR_CLOCK_UPDATE  0x00548FE0u
#define ADDR_FRAME_DT      0x00925018u  /* flt_925018: seconds of the last frame */
#define COUNTER_HZ         60.0f

/*
 * Collision tolerance. The swept-sphere "collide and slide" routine sub_68F940 (character vs world) loads two
 * constants into its work block: the "very close distance" 0.005 (flt_7597A4, block +160) and 5e-6
 * (flt_76E818, block +164). The first is a FRACTION of the step (hit time t = d0 / (d0 - d1)), so its size in
 * world units is 0.005 x step: 1e-4 at 60 fps, 7e-6 at 850 fps, below the float resolution at the hero's
 * coordinates. The two loads read the mod's values; the fraction is scaled up for short steps.
 */
#define ADDR_STEPUP_CALL   0x0068FC07u  /* call sub_68EE40 in sub_68F940 */
#define ADDR_STEPUP_FN     0x0068EE40u
#define ADDR_WORK_BLOCK    0x0292186Cu  /* dword_292186C: collision work block, +160 = sweep tolerance fraction */
#define ADDR_SWEEP         0x0068F940u  /* sub_68F940(pos, move, flags, idx, a5, radius y, radius, 0, 0, mask): swept sphere */
#define ADDR_SWEEP_RESUME  0x0068F949u
#define ADDR_CLOSE_DIST_1  0x0068FAFEu  /* fld ds:flt_7597A4 */
#define ADDR_CLOSE_DIST_2  0x0068FB10u  /* fld ds:flt_76E818 */

/*
 * Effect / script timeline. sub_491D50 ticks sub_686950(1.0) once per logic frame; the function adds (int)a1 to the
 * tick counter dword_2962B48 and steps its effect spawns by a1 / 60 s. With logic frames at 860 per second the
 * timeline ran 14x fast. The call goes through a wrapper that passes whole 1/60 s ticks from a time accumulator.
 */
#define ADDR_SCRIPT_TICK_CALL 0x004922ADu
#define ADDR_SCRIPT_TICK_FN   0x00686950u

/* Per-call systems of sub_491D50 that assume one call per 1/60 s: grass chains (sub_544420), wind strands
 * (sub_546380), sway phase (sub_545A80), wobble springs (sub_66CC60). */
#define ADDR_CHAIN_CALL       0x004922B7u
#define ADDR_CHAIN_FN         0x00544420u
#define ADDR_STRAND_CALL      0x004922CDu
#define ADDR_STRAND_FN        0x00546380u
#define ADDR_SWAY_CALL        0x004922D7u
#define ADDR_SWAY_FN          0x00545A80u
#define ADDR_WOBBLE_CALL      0x0049233Bu
#define ADDR_WOBBLE_FN        0x0066CC60u

/*
 * Velocity after a collision. sub_416D60 moves the player with velocity * dt through the swept collision and
 * then rebuilds velocity = resolved movement / dt (sub_5B3830(player + 104, move, 1 / dt) at 0x4178D6). A wall
 * removes the push into it every frame and the next frame the velocity only eases back by rate * dt, so the
 * steady push into a wall (and the slide up a slope) is proportional to the frame time. The call goes through
 * a wrapper that keeps (1 - 60 dt) of the pre-collision velocity, which is what makes 60 fps behave as it does.
 */
#define ADDR_VEL_REBUILD   0x004178D6u  /* call sub_5B3830 */
#define ADDR_VEL_REBUILD_FN 0x005B3830u

/*
 * Contact response offsets. Every time the character slides along a surface the collision code pushes the
 * movement away from it by fixed world units (double constants shared from .rdata): 0.0003 along the plane
 * normal in the projection helpers sub_677FB0 / sub_678010 and in sub_6786F0, 0.0006 / 0.0009 / 0.0015 /
 * 0.0035 in the slope and wall responses of sub_6786F0, 0.01 x normal.y as an upward kick there, and
 * 0.004 x normal.y lifted after a hit in sub_678F60. They are per contact, i.e. per frame: at 850 fps they
 * push the hero off the tree 14x harder per second than the game was tuned for (and make it creep on slopes).
 * Each load is redirected to a double scaled by the step length.
 */

/*
 * Foliage wind. sub_5EC3D0 (the sway offsets of plants) takes its phase from dword_97390C, which the
 * renderer frame function sub_60AFF0 increments once per frame (add dword_97390C, 1). The wind therefore
 * blew at fps / 60 times its speed. The add is replaced by a stub that adds 60 steps per second of real time.
 */
#define ADDR_WIND_PHASE    0x0097390Cu
#define ADDR_WIND_TICK     0x0060B551u  /* add dword_97390C, 1 (7 bytes) */
#define ADDR_WIND_RESUME   0x0060B558u

/*
 * Jump height. See JumpFix in framerate.c: the jump impulse is corrected so the apex matches the 60 fps
 * one at any frame rate. Player objects are dword_93D7F0[slot]; +108 is the vertical speed, +84 the type
 * whose physics block (+36) holds the gravity at +36.
 */

/* Video mode table and desktop size of the device object. */
#define DEVICE_MODE_COUNT_OFS 0x618
#define DEVICE_MODE_TABLE_OFS 0x61C
#define DEVICE_DESKTOP_W_OFS  0x608
#define DEVICE_DESKTOP_H_OFS  0x60C
#define MAX_RENDER_SCALE    4.0


/*
 * VSync and window mode. sub_6E3940 (device create / reset, Accept in Video Settings calls it with
 * 1) fills the present parameters: PresentationInterval is the constant 1 (mov [esi+16Ch], ebx) and
 * Windowed comes from the requested-windowed byte at device+65Ch. When windowed and the argument
 * is set it moves the window to nWidth, nHeight, X, Y (globals), X / Y < 0 centre it. The window
 * style (overlapped for windowed, popup otherwise) is chosen by the creation code, so it is set
 * again here when the mode changes.
 */
#define ADDR_PRESENT_INTERVAL 0x006E3A19u
#define ADDR_DEVICE_RESET     0x006E3940u  /* sub_6E3940(this, moveWindow) (thiscall) */
#define ADDR_GAME_WINDOW      0x00924864u  /* hWnd */
#define ADDR_WINDOW_RECT      0x0082647Cu  /* nWidth, nHeight, X, Y */
#define DEVICE_MODE_INDEX_OFS 0x660        /* mode table index in use */
#define DEVICE_WINDOWED_OFS   0x65C        /* byte: windowed requested */

#define ADDR_STREAK_FN     0x005A3730u  /* sub_5A3730: adds a segment to a trail (lightsaber streak) */
#define ADDR_STREAK_CALLS  { 0x0049FC89u, 0x0049FD53u, 0x004B1E8Cu }
#define ADDR_SPARK_RATE_FN 0x0062C350u  /* sub_62C350(rate, dt): random number of events for a rate per second */
#define ADDR_SPARK_CALLS   { 0x004A0455u, 0x004B231Du, 0x004B24E0u }  /* its calls in the lightsaber code sub_49FA50, sub_4B1AB0 */
#define ADDR_REFRESH_FN    0x006DF270u  /* sub_6DF270: refresh rate in Hz */
#define ADDR_LIMITER_CALLS { 0x004FD20Bu, 0x004FD237u, 0x004FD25Eu, 0x004FD28Au }  /* its calls in the frame limiter sub_4FD050 */
#define ADDR_REFRESH_LOAD  0x006E3AE8u  /* mov eax,[eax+8]; mov [esi+168h],eax */
#define ADDR_REFRESH_BACK  0x006E3AF1u  /* jz short ... (flags from the earlier test are kept) */
#define ADDR_DESKTOP_HZ    0x02976BD8u  /* this+610h: refresh of the adapter's current display mode */
#define ADDR_MODE_COUNT    0x02976BE0u  /* number of entries in the mode table */
#define ADDR_MODE_TABLE    0x02976BE4u  /* pointer to {width, height, refresh, format} entries */

typedef void (__cdecl *FrameFn)(int game, int *out);

typedef void (__cdecl *OpenMenuFn)(int menu, int flag, int arg);

typedef int  (__cdecl *DetectDeviceFn)(int mask, int kinds, int *a, int *b, int skipTaken);

typedef int  (__cdecl *MapDeviceFn)(int raw, int arg);

typedef void (__cdecl *AssignDeviceFn)(int slot, int device);

typedef void (__cdecl *EnableDeviceFn)(int device, int on);

typedef int  (__cdecl *InputSaveFn)(void);

typedef void (__cdecl *InputSelectFn)(int ctx);

/*
 * Viewport / scissor scaling for the scaled render targets. The game sets its viewport from
 * the native resolution, so with bigger targets the scene would only fill the top-left part.
 * IDirect3DDevice9::SetRenderTarget, SetViewport and SetScissorRect are hooked through the
 * device vtable; while render target 0 is one of the scaled targets the rectangles are scaled.
 */
#define VT_SET_VS_CONST_F    94
#define ADDR_DEVICE_OBJECT   0x029765C8u   /* this; *(this + 4) is the IDirect3DDevice9 */

#define MAX_DUMP_TEX 4096

/* ---- button prompts: PlayStation glyphs in the "Buttons" font ------------------------ */
#define VT_SET_TEXTURE 65

typedef HRESULT (STDMETHODCALLTYPE *SetTextureFn)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);

typedef int (__cdecl *MenuDrawFn)(int menu);

typedef int (__cdecl *MenuUpdateFn)(int menu);

typedef int (__cdecl *MenuRowFn)(int menu, const char *text, int alpha);


typedef int (__cdecl *PauseRowFn)(int menu, const char *text);

typedef void (__attribute__((thiscall)) *DeviceRefreshFn)(void *self, int slots);

typedef int (__attribute__((thiscall)) *ApplyDeviceFn)(void *device, int move_window);

/*
 * Hero physics that is applied once per call with a fixed factor (written for 60 calls/s). Each site loads its
 * constant from memory, so the operand is redirected to a variable holding constant^(dt*60).
 */
#define ADDR_DAMP_STEPUP      0x0068F5C5u   /* sub_68EE40: fmul qword 0.75, vertical velocity on a step-up hit  */
#define ADDR_DAMP_BOOST       0x004AB754u   /* sub_4AB620: fmul qword 0.5, speed multiplier +3140               */
#define ADDR_DAMP_BOOST2      0x004AB5E1u   /* sub_4AB5A0: same halving of +3140, second copy                    */
#define ADDR_HEADING_TRUNC    0x00550121u   /* sub_550100: call sub_6E6B90, heading step truncated to int       */
#define ADDR_TRUNC_FN         0x006E6B90u
#define ADDR_ANGLE_APPROACH   0x00550070u   /* sub_550070(from, to, rate): angle lerp, result truncated to int  */
#define ADDR_WALLPUSH_STEP    0x0059BE31u   /* sub_59BD40: per-call step of the wall-push timer +3524 (dt <= 1/30 branch) */
