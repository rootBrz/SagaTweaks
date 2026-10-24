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
#define DETECT_KINDS       0x1A
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
 * Vertical physics at a fixed 60 Hz. Jump height is the integral of explicit Euler
 * steps (sub_4ABAC0 eases the vertical velocity, sub_55D2B0 adds velocity * dt to
 * the height), so a smaller step gives a slightly lower jump: 0.770 at 60 fps but
 * 0.762 at 120 fps, enough to fail borderline obstacles. Both are therefore fed a
 * physics time step that is a multiple of 1/60 s and zero on frames in between.
 * Horizontal movement keeps the real frame time, only height is stepped.
 */
#define ADDR_PHYS_VERTICAL 0x004ABAC0u  /* sub_4ABAC0(player, ...): vertical velocity update */
#define ADDR_PHYS_RESUME   0x004ABAC9u
#define ADDR_POS_VY_1      0x0055D345u  /* fmul ds:flt_925018 after fld [esi+78h] (operand) */
#define ADDR_POS_VY_2      0x0055D3B4u
#define ADDR_POS_VY_3      0x0055D475u
#define ADDR_POS_VY_4      0x0055D577u

/*
 * Render scale. The video mode list ends at the desktop resolution. Entries for
 * desktop size * scale (RenderScales in the ini, default 1.5 and 2) are appended before
 * the game first looks a mode up (sub_6E1B70, whose first five bytes are hooked), so
 * they show up in the Screen Resolution row and a saved choice is restored on start.
 * The game then renders at the bigger size and gamescope scales it down to the display.
 */
#define ADDR_MODE_LOOKUP    0x006E1B70u  /* sub_6E1B70(this, w, h, hz): nearest mode lookup (thiscall) */
#define ADDR_MODE_LOOKUP_2  0x006E1B75u  /* resume address after the displaced 5 bytes */
#define DEVICE_MODE_COUNT_OFS 0x618
#define DEVICE_MODE_TABLE_OFS 0x61C
#define DEVICE_DESKTOP_W_OFS  0x608
#define DEVICE_DESKTOP_H_OFS  0x60C
#define MAX_RENDER_SCALE    2.8   /* 3.0 (5760 px wide) makes every character vanish; 2.85 still works */
#define MAX_RENDER_SCALES   8
#define MAX_RENDER_DIM      8192

/*
 * Render scale (real). The effect render targets are created by sub_605800 / sub_605890
 * with size = backbuffer size / divisor, reading dword_2976700 (width) and dword_2976704
 * (height). The four loads are replaced by calls that return the size times RenderScale.
 * The scene is then drawn into larger (or smaller) targets and the composite pass maps the
 * whole target onto the real backbuffer, so the HUD stays at native resolution.
 */
#define ADDR_RT_H_1  0x00605813u  /* mov eax, [dword_2976704] in sub_605800 */
#define ADDR_RT_W_1  0x00605820u  /* mov eax, [dword_2976700] in sub_605800 */
#define ADDR_RT_H_2  0x00605894u  /* ... in sub_605890 */
#define ADDR_RT_W_2  0x006058A3u
#define ADDR_RT_H_3  0x00605989u  /* the same loads in a third render target creator (character pass) */
#define ADDR_RT_W_3  0x00605995u
#define ADDR_BACKBUFFER_W 0x02976700u
#define ADDR_BACKBUFFER_H 0x02976704u

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
#define VT_SET_RENDER_TARGET 37
#define VT_SET_VIEWPORT      47
#define VT_SET_DEPTH_STENCIL 39
#define VT_SET_SCISSOR_RECT  75
#define VT_SET_VS_CONST_F    94
#define VT_SET_PS_CONST_F    109
#define ADDR_DEVICE_OBJECT   0x029765C8u   /* this; *(this + 4) is the IDirect3DDevice9 */

#define MAX_DUMP_TEX 4096

/* ---- button prompts: PlayStation glyphs in the "Buttons" font ------------------------ */
#define VT_SET_TEXTURE 65

typedef HRESULT (STDMETHODCALLTYPE *SetTextureFn)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);

typedef int (__cdecl *MenuDrawFn)(int menu);

typedef int (__cdecl *MenuUpdateFn)(int menu);

typedef int (__cdecl *MenuRowFn)(int menu, const char *text, int alpha);

typedef int (__attribute__((thiscall)) *BuildTargetsFn)(void *device);

typedef int (__cdecl *PauseRowFn)(int menu, const char *text);

typedef void (__attribute__((thiscall)) *DeviceRefreshFn)(void *self, int slots);

typedef void (__cdecl *PhysFn)(int player, int *a2, float a3, float a4, float *a5);
