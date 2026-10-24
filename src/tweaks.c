/*
 * SagaTweaks.asi - quality-of-life patches for LEGO Star Wars: The Complete Saga
 * (Steam build, LEGOStarWarsSaga.exe, 5,472,256 bytes).
 *
 * Startup
 *   - skips the boot logos and the "Press any button" title screen
 *   - skips cutscenes with Start (the whole chain of parts, no wipe frames)
 * Display
 *   - removes the 85 fps cap, runs at the desktop refresh rate (shown in Video Settings)
 *   - render scale 50-300% (native row in Video Settings), with the HUD / particle pass
 *     rendered into a matching scaled backbuffer so effects are depth tested correctly
 *   - FSAA is available together with enhanced graphics
 * Frame-rate independence
 *   - clock counters, blink timers and vertical (jump) physics behave as at 60 fps
 * Input
 *   - player 1 hot-switches between keyboard and gamepad
 *   - a second gamepad joins as player 2 with Start; P1 / P2 drop-outs are recovered
 *   - button prompts: Xbox or PlayStation glyphs, selectable per player
 * Menus
 *   - "Tweaks" page (main menu and pause menu) with every option above
 *
 * Settings live in SagaTweaks.ini next to the exe, section [SagaTweaks]; the log is
 * SagaTweaks.log.
 *
 * The exe is SteamStub-packed, so .text is encrypted until the stub hands over to the real
 * entry point. The patch thread waits for the expected code bytes to show up before touching
 * anything. All addresses are for the unpacked image (base 0x400000, no ASLR).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <d3d9.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>

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
 * FSAA lock. byte_8264C7 is "UseHires" (enhanced graphics). The video settings screen
 * greys the FSAA row out, ignores presses on it, and the config loader forces FSAA to 0
 * whenever it is set. Each of those three tests is redirected to a byte that is always
 * zero, so FSAA stays selectable with enhanced graphics on.
 */
#define ADDR_USE_HIRES         0x008264C7u  /* byte_8264C7 */
#define ADDR_FSAA_LOCK_UI      0x005121E6u  /* cmp byte_8264C7, 0 (operand) in the video settings draw */
#define ADDR_FSAA_LOCK_INPUT   0x0050F0FFu  /* ... in the FSAA row input handler */
#define ADDR_FSAA_LOCK_CONFIG  0x006DC059u  /* ... in the config loader that forces FSAA to 0 */

/*
 * Keep shadows and bloom when FSAA is on. The game downgrades them in sub_6E2610
 * (shadow method to 0, bloom method to 1) whenever FSAA is active, and the menu
 * greys the Shadows row / refuses the toggle for the same reason. The downgrade is
 * skipped and the three menu tests of dword_2976C30 are pointed at a zero dword.
 */
#define ADDR_FSAA_DOWNGRADE    0x006E26CBu  /* jz short: skips the downgrade when FSAA is off */
#define ADDR_FSAA_MENU_1       0x00509362u  /* cmp dword_2976C30, 0 (operand) in sub_509360 */
#define ADDR_FSAA_MENU_2       0x00512436u  /* ... in the effects settings draw (shadows text) */
#define ADDR_FSAA_MENU_3       0x005124A3u  /* ... in the effects settings draw (shadows alpha) */

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

static const uint8_t k_logo_branch_orig[6] = { 0x0F, 0x84, 0x52, 0x07, 0x00, 0x00 };
static const uint8_t k_frame_call_orig[5]  = { 0xE8, 0x8C, 0xE8, 0xFF, 0xFF };
static const uint8_t k_limit_cmp_orig[6]   = { 0xDC, 0x1D, 0x30, 0xEC, 0x75, 0x00 };
static const uint8_t k_limit_load_orig[6]  = { 0xD9, 0x05, 0x2C, 0xEC, 0x75, 0x00 };
static const uint8_t k_clock_orig[6]       = { 0x8B, 0x4C, 0x24, 0x04, 0xD9, 0x01 };
static const uint8_t k_phys_orig[9]        = { 0x83, 0xEC, 0x18, 0xD9, 0x05, 0xDC, 0xB8, 0x87, 0x00 };
static const uint8_t k_posvy_orig[4]       = { 0x18, 0x50, 0x92, 0x00 };
static const uint8_t k_hires_addr_orig[4]  = { 0xC7, 0x64, 0x82, 0x00 };
static const uint8_t k_fsaa_cmp_orig[4]   = { 0x30, 0x6C, 0x97, 0x02 };
static const uint8_t k_modelookup_orig[5]  = { 0x83, 0xEC, 0x18, 0x53, 0x55 };
static const uint8_t k_rt_h_orig[5]       = { 0xA1, 0x04, 0x67, 0x97, 0x02 };
static const uint8_t k_rt_w_orig[5]       = { 0xA1, 0x00, 0x67, 0x97, 0x02 };
static const uint8_t k_refresh_orig[9]     = { 0x8B, 0x40, 0x08, 0x89, 0x86, 0x68, 0x01, 0x00, 0x00 };

static double g_limit_d = UNLIMITED_FPS;
static float  g_limit_f = (float)UNLIMITED_FPS;

typedef void (__cdecl *FrameFn)(int game, int *out);
typedef void (__cdecl *OpenMenuFn)(int menu, int flag, int arg);
typedef int  (__cdecl *DetectDeviceFn)(int mask, int kinds, int *a, int *b, int skipTaken);
typedef int  (__cdecl *MapDeviceFn)(int raw, int arg);
typedef void (__cdecl *AssignDeviceFn)(int slot, int device);
typedef void (__cdecl *EnableDeviceFn)(int device, int on);
typedef int  (__cdecl *InputSaveFn)(void);
typedef void (__cdecl *InputSelectFn)(int ctx);

static FILE *g_log;
static int g_title_frames;
static int g_skip_logos = 1;
static int g_skip_title = 1;
static int g_hot_switch = 1;
static int g_join_start = 1;     /* gamepads join only with the Start button */
static int g_start_button = 7;   /* button code reported by the input scan for Start */
static int g_max_fps;          /* 0 = unlimited (vsync decides) */
static int g_refresh_override; /* 0 = desktop refresh rate */
static int g_fix_counters = 1;  /* make per-frame counters tick at 60 Hz */
static int g_fix_physics = 1;   /* step vertical physics at a fixed 60 Hz */
static int g_unlock_fsaa;       /* obsolete: the FSAA row is hidden (render scale replaces it) */
static uint8_t g_zero_byte;
static int g_fsaa_effects;  /* keep shadows and bloom with FSAA */
static uint32_t g_zero_dword;
static double g_scales[MAX_RENDER_SCALES];
static int g_scale_count;
void *g_lookup_tramp;
static double g_render_scale = 1.0;
static char g_ini_path[MAX_PATH];
static int g_log_fps;          /* log frame rate once a second, first 120 s */
static int g_detect_logged;
static int g_join_logged;
static int g_skip_cutscenes = 1;   /* Start fast-forwards the running cutscene */
static int g_join_start_text;

static void ApplyMaxFps(void)
{
    g_limit_d = g_max_fps > 0 ? (double)g_max_fps : UNLIMITED_FPS;
    g_limit_f = (float)g_limit_d;
}

static void Log(const char *msg)
{
    if (g_log) {
        fprintf(g_log, "%s\n", msg);
        fflush(g_log);
    }
}

static int WritePatch(uintptr_t addr, const void *bytes, size_t len)
{
    DWORD old;

    if (!VirtualProtect((void *)addr, len, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy((void *)addr, bytes, len);
    VirtualProtect((void *)addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)addr, len);
    return 1;
}

/*
 * Replacement for "mov eax,[eax+8]; mov [esi+168h],eax": eax = refresh override or
 * the desktop refresh, store it, resume at the jz that follows. pushfd/popfd keep
 * the flags that jz depends on.
 */
extern void RefreshStub(void);
__asm__(
    ".text\n"
    ".globl _RefreshStub\n"
    "_RefreshStub:\n"
    "    pushfl\n"
    "    movl _g_refresh_override, %eax\n"
    "    testl %eax, %eax\n"
    "    jnz 1f\n"
    "    movl 0x610(%esi), %eax\n"
    "1:  popfl\n"
    "    movl %eax, 0x168(%esi)\n"
    "    movl $0x006E3AF1, %eax\n"
    "    jmp *%eax\n"
);

/* Scaled render target sizes (even, at least 16). */
int __cdecl ScaledWidth(void)
{
    int v;

    if (g_render_scale == 1.0)
        return *(int *)ADDR_BACKBUFFER_W;
    v = (int)(*(int *)ADDR_BACKBUFFER_W * g_render_scale + 0.5);

    return v < 16 ? 16 : (v & ~1);
}

int __cdecl ScaledHeight(void)
{
    int v;

    if (g_render_scale == 1.0)
        return *(int *)ADDR_BACKBUFFER_H;
    v = (int)(*(int *)ADDR_BACKBUFFER_H * g_render_scale + 0.5);

    return v < 16 ? 16 : (v & ~1);
}

/* register-preserving entry points so they can replace a "mov eax,[abs]" (5 bytes) */
extern void ScaledWidthStub(void);
extern void ScaledHeightStub(void);
__asm__(
    ".text\n"
    ".globl _ScaledWidthStub\n"
    "_ScaledWidthStub:\n"
    "    pushl %ecx\n    pushl %edx\n"
    "    call _ScaledWidth\n"
    "    popl %edx\n    popl %ecx\n    ret\n"
    ".globl _ScaledHeightStub\n"
    "_ScaledHeightStub:\n"
    "    pushl %ecx\n    pushl %edx\n"
    "    call _ScaledHeight\n"
    "    popl %edx\n    popl %ecx\n    ret\n"
);

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

typedef HRESULT (STDMETHODCALLTYPE *SetRenderTargetFn)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
typedef HRESULT (STDMETHODCALLTYPE *SetViewportFn)(IDirect3DDevice9 *, const D3DVIEWPORT9 *);
typedef HRESULT (STDMETHODCALLTYPE *SetScissorRectFn)(IDirect3DDevice9 *, const RECT *);
typedef HRESULT (STDMETHODCALLTYPE *SetDepthStencilFn)(IDirect3DDevice9 *, IDirect3DSurface9 *);

static SetRenderTargetFn g_orig_set_rt;
static SetViewportFn g_orig_set_viewport;
static SetScissorRectFn g_orig_set_scissor;
static SetDepthStencilFn g_orig_set_depth;
typedef HRESULT (STDMETHODCALLTYPE *SetConstFFn)(IDirect3DDevice9 *, UINT, const float *, UINT);
static SetConstFFn g_orig_set_vs_const, g_orig_set_ps_const;
static IDirect3DSurface9 *g_depth_main;     /* the game's full-size depth buffer */
static IDirect3DSurface9 *g_depth_twin;     /* same format, size of the scaled targets */
static IDirect3DSurface9 *g_engine_depth;   /* what the game last asked for */
static UINT g_twin_w, g_twin_h;

/* Depth buffer that matches the scaled targets (a smaller one would clip or disable depth). */
static int EnsureDepthTwin(IDirect3DDevice9 *dev)
{
    D3DSURFACE_DESC desc;
    UINT w = (UINT)ScaledWidth(), h = (UINT)ScaledHeight();

    if (!g_depth_main)
        return 0;
    if (g_depth_twin && g_twin_w == w && g_twin_h == h)
        return 1;
    if (FAILED(IDirect3DSurface9_GetDesc(g_depth_main, &desc)))
        return 0;
    if (g_depth_twin) {
        IDirect3DSurface9_Release(g_depth_twin);
        g_depth_twin = NULL;
    }
    if (FAILED(IDirect3DDevice9_CreateDepthStencilSurface(dev, w, h, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_depth_twin, NULL)))
        return 0;
    g_twin_w = w;
    g_twin_h = h;
    Log("render scale: depth buffer created");
    return 1;
}
static void **g_hooked_vtable;
static int g_rt0_scaled;
static int g_trace_rt;          /* debug: log device calls for a few frames */
static int g_trace_budget;
static void Trace(const char *fmt, ...)
{
    va_list ap;

    if (!g_log || g_trace_budget <= 0)
        return;
    g_trace_budget--;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}


/* ---- debug: per-pass draw statistics / pass skipping (TraceRT=1, SkipPass=N) ---------- */
#define VT_END_SCENE 42
#define VT_DRAW_PRIMITIVE 81
#define VT_DRAW_INDEXED 82
#define VT_DRAW_PRIMITIVE_UP 83
#define VT_DRAW_INDEXED_UP 84
typedef HRESULT (STDMETHODCALLTYPE *EndSceneFn)(IDirect3DDevice9 *);
typedef HRESULT (STDMETHODCALLTYPE *DrawPrimFn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DrawIdxFn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DrawPrimUPFn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DrawIdxUPFn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT);
static EndSceneFn g_orig_end_scene;
static DrawPrimFn g_orig_draw_prim;
static DrawIdxFn g_orig_draw_idx;
static DrawPrimUPFn g_orig_draw_prim_up;
static DrawIdxUPFn g_orig_draw_idx_up;
static int g_pass_index, g_pass_draws, g_skip_pass = -1;
static unsigned g_pass_w, g_pass_h;

static int g_skip_scaled = -1, g_scaled_done;
static int g_trace_passes, g_pass_lines;
static char g_frame_sig[1024], g_last_sig[1024];

static void EndPass(void)
{
    if (g_pass_draws && g_pass_w == (unsigned)ScaledWidth() && g_pass_h == (unsigned)ScaledHeight())
        g_scaled_done++;
    if (g_trace_passes && g_pass_draws) {
        size_t n = strlen(g_frame_sig);

        if (n < sizeof g_frame_sig - 64)
            snprintf(g_frame_sig + n, sizeof g_frame_sig - n, "[%d: %ux%u] ", g_pass_index, g_pass_w, g_pass_h);
    }
    g_pass_draws = 0;
}

static void EndFrameSig(void)
{
    if (g_trace_passes && g_log && g_pass_lines < 400 && g_frame_sig[0] && strcmp(g_frame_sig, g_last_sig)) {
        fprintf(g_log, "frame passes: %s\n", g_frame_sig);
        fflush(g_log);
        g_pass_lines++;
        memcpy(g_last_sig, g_frame_sig, sizeof g_last_sig);
    }
    g_frame_sig[0] = 0;
}

static int SkipDraw(void)
{
    g_pass_draws++;
    if (g_skip_scaled >= 0 && g_pass_w == (unsigned)ScaledWidth() && g_pass_h == (unsigned)ScaledHeight() &&
        g_scaled_done == g_skip_scaled)
        return 1;
    return g_skip_pass >= 0 && g_pass_index == g_skip_pass;
}

static HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9 *dev)
{
    EndPass();
    EndFrameSig();
    g_pass_index = 0;
    g_scaled_done = 0;
    return g_orig_end_scene(dev);
}

static HRESULT STDMETHODCALLTYPE HookDrawPrim(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT b)
{
    return SkipDraw() ? D3D_OK : g_orig_draw_prim(d, t, a, b);
}

static HRESULT STDMETHODCALLTYPE HookDrawIdx(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT a, UINT b, UINT c, UINT e, UINT f)
{
    return SkipDraw() ? D3D_OK : g_orig_draw_idx(d, t, a, b, c, e, f);
}

static HRESULT STDMETHODCALLTYPE HookDrawPrimUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, const void *b, UINT c)
{
    return SkipDraw() ? D3D_OK : g_orig_draw_prim_up(d, t, a, b, c);
}

static HRESULT STDMETHODCALLTYPE HookDrawIdxUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT c, const void *e, D3DFORMAT f, const void *g, UINT h)
{
    return SkipDraw() ? D3D_OK : g_orig_draw_idx_up(d, t, a, b, c, e, f, g, h);
}


/*
 * Everything the game draws straight onto the backbuffer after the scene (HUD, glow and
 * sparkle particles that are depth tested against the scene) has to use the same resolution
 * and the same scaled depth buffer as the scene, otherwise the particles are tested against an
 * empty depth buffer and show through walls. While the scale is not 100% the backbuffer is
 * replaced by a render target of the scaled size and copied (filtered) onto the real
 * backbuffer just before Present.
 */
#define VT_PRESENT 17
#define VT_RESET   16
typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT (STDMETHODCALLTYPE *ResetFn)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static int g_trace_skip;
static DWORD g_skip_done_tick, g_skip_first_tick;
static int g_cutscene_skipping;      /* a cutscene skip is running: the screen is kept black */
static PresentFn g_orig_present;
static ResetFn g_orig_reset;
static IDirect3DSurface9 *g_virtual_bb;
static void *g_real_bb;                 /* identity of the swap chain's backbuffer, not a reference */
static UINT g_vbb_w, g_vbb_h;
static int g_vbb_used;

static IDirect3DSurface9 *VirtualBackbuffer(IDirect3DDevice9 *dev, IDirect3DSurface9 *surface)
{
    D3DSURFACE_DESC desc;
    UINT w, h;

    if (g_render_scale == 1.0)
        return NULL;
    if (!g_real_bb) {
        IDirect3DSurface9 *bb = NULL;

        if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
            return NULL;
        g_real_bb = bb;
        IDirect3DSurface9_Release(bb);
    }
    if ((void *)surface != g_real_bb || FAILED(IDirect3DSurface9_GetDesc(surface, &desc)) ||
        desc.MultiSampleType != D3DMULTISAMPLE_NONE)
        return NULL;
    w = (UINT)ScaledWidth();
    h = (UINT)ScaledHeight();
    if (g_virtual_bb && (g_vbb_w != w || g_vbb_h != h)) {
        IDirect3DSurface9_Release(g_virtual_bb);
        g_virtual_bb = NULL;
    }
    if (!g_virtual_bb) {
        if (FAILED(IDirect3DDevice9_CreateRenderTarget(dev, w, h, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_virtual_bb, NULL)))
            return NULL;
        g_vbb_w = w;
        g_vbb_h = h;
        Log("render scale: scaled backbuffer created");
    }
    g_vbb_used = 1;
    return g_virtual_bb;
}

static HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty)
{
    if (g_vbb_used && g_virtual_bb && g_real_bb && g_render_scale != 1.0)
        IDirect3DDevice9_StretchRect(dev, g_virtual_bb, NULL, (IDirect3DSurface9 *)g_real_bb, NULL, D3DTEXF_LINEAR);
    g_vbb_used = 0;
    if (g_trace_skip && g_skip_first_tick && GetTickCount() - g_skip_first_tick < 4000) {
        char buf[160];

        snprintf(buf, sizeof buf, "present +%lu ms black %d wipe %.3f dir %.3f style %02x pend %d/%d be0 %d",
                 GetTickCount() - g_skip_first_tick, g_cutscene_skipping, *(float *)0x950780, *(float *)0x950784,
                 *(uint8_t *)0x95077C, *(int *)0x87658C, *(int *)0x951B80 != 0, *(int *)0x951BE0);
        Log(buf);
    }
    if (g_cutscene_skipping) {
        IDirect3DSurface9 *bb = NULL;

        if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            HRESULT hr = IDirect3DDevice9_ColorFill(dev, bb, NULL, 0xFF000000);
            static int logged;

            if (FAILED(hr) && !logged++) {
                char buf[64];

                snprintf(buf, sizeof buf, "skip black fill failed: %08lx", (unsigned long)hr);
                Log(buf);
            }
            IDirect3DSurface9_Release(bb);
        }
    }
    return g_orig_present(dev, src, dst, wnd, dirty);
}

static HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    if (g_virtual_bb) {
        IDirect3DSurface9_Release(g_virtual_bb);
        g_virtual_bb = NULL;
    }
    if (g_depth_twin) {
        IDirect3DSurface9_Release(g_depth_twin);
        g_depth_twin = NULL;
    }
    g_real_bb = NULL;
    g_vbb_used = 0;
    return g_orig_reset(dev, pp);
}

static void ResendScreenConst(IDirect3DDevice9 *dev);

static HRESULT STDMETHODCALLTYPE HookSetRenderTarget(IDirect3DDevice9 *dev, DWORD index, IDirect3DSurface9 *surface)
{
    HRESULT hr;

    if (index == 0 && surface) {
        IDirect3DSurface9 *vbb = VirtualBackbuffer(dev, surface);

        if (vbb)
            surface = vbb;
    }
    hr = g_orig_set_rt(dev, index, surface);

    if (g_trace_rt && surface) {
        D3DSURFACE_DESC d;

        if (SUCCEEDED(IDirect3DSurface9_GetDesc(surface, &d)))
            Trace("SetRenderTarget idx %lu surf %p %ux%u fmt %d ms %d", (unsigned long)index, (void *)surface, d.Width, d.Height, (int)d.Format, (int)d.MultiSampleType);
    }
    if (index == 0 && SUCCEEDED(hr)) {
        int was_scaled = g_rt0_scaled;

        if (surface) {
            D3DSURFACE_DESC pd;

            EndPass();
            g_pass_index++;
            if (SUCCEEDED(IDirect3DSurface9_GetDesc(surface, &pd))) {
                g_pass_w = pd.Width;
                g_pass_h = pd.Height;
            }
        }

        g_rt0_scaled = 0;
        if (surface) {
            D3DSURFACE_DESC desc;

            if (SUCCEEDED(IDirect3DSurface9_GetDesc(surface, &desc)))
                g_rt0_scaled = desc.Width == (UINT)ScaledWidth() && desc.Height == (UINT)ScaledHeight() &&
                               (desc.Width != (UINT)*(int *)ADDR_BACKBUFFER_W ||
                                desc.Height != (UINT)*(int *)ADDR_BACKBUFFER_H);
        }
        if (g_engine_depth && g_engine_depth == g_depth_main)
            g_orig_set_depth(dev, (g_rt0_scaled && EnsureDepthTwin(dev)) ? g_depth_twin : g_depth_main);
        if (was_scaled != g_rt0_scaled)
            ResendScreenConst(dev);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookSetDepthStencil(IDirect3DDevice9 *dev, IDirect3DSurface9 *surface)
{
    D3DSURFACE_DESC desc;

    if (g_trace_rt && surface && SUCCEEDED(IDirect3DSurface9_GetDesc(surface, &desc)))
        Trace("SetDepthStencil surf %p %ux%u fmt %d scaledRT=%d main=%p", (void *)surface, desc.Width, desc.Height, (int)desc.Format, g_rt0_scaled, (void *)g_depth_main);
    if (surface && surface != g_depth_twin && SUCCEEDED(IDirect3DSurface9_GetDesc(surface, &desc)) &&
        desc.Width == (UINT)*(int *)ADDR_BACKBUFFER_W && desc.Height == (UINT)*(int *)ADDR_BACKBUFFER_H)
        g_depth_main = surface;                    /* the game's own full-size depth buffer */
    g_engine_depth = surface;
    if (surface && surface == g_depth_main && g_rt0_scaled && EnsureDepthTwin(dev))
        surface = g_depth_twin;
    return g_orig_set_depth(dev, surface);
}

static HRESULT STDMETHODCALLTYPE HookSetViewport(IDirect3DDevice9 *dev, const D3DVIEWPORT9 *vp)
{
    D3DVIEWPORT9 scaled;

    if (g_trace_rt && vp)
        Trace("SetViewport %lu,%lu %lux%lu scaledRT=%d", (unsigned long)vp->X, (unsigned long)vp->Y, (unsigned long)vp->Width, (unsigned long)vp->Height, g_rt0_scaled);
    if (vp && g_rt0_scaled) {
        scaled = *vp;
        scaled.X = (DWORD)(vp->X * g_render_scale + 0.5);
        scaled.Y = (DWORD)(vp->Y * g_render_scale + 0.5);
        scaled.Width = (DWORD)(vp->Width * g_render_scale + 0.5);
        scaled.Height = (DWORD)(vp->Height * g_render_scale + 0.5);
        return g_orig_set_viewport(dev, &scaled);
    }
    return g_orig_set_viewport(dev, vp);
}

static HRESULT STDMETHODCALLTYPE HookSetScissorRect(IDirect3DDevice9 *dev, const RECT *rect)
{
    RECT scaled;

    if (g_trace_rt && rect)
        Trace("SetScissorRect %ld,%ld %ld,%ld scaledRT=%d", rect->left, rect->top, rect->right, rect->bottom, g_rt0_scaled);
    if (rect && g_rt0_scaled) {
        scaled.left = (LONG)(rect->left * g_render_scale + 0.5);
        scaled.top = (LONG)(rect->top * g_render_scale + 0.5);
        scaled.right = (LONG)(rect->right * g_render_scale + 0.5);
        scaled.bottom = (LONG)(rect->bottom * g_render_scale + 0.5);
        return g_orig_set_scissor(dev, &scaled);
    }
    return g_orig_set_scissor(dev, rect);
}


static void TraceConstants(const char *kind, UINT start, const float *data, UINT count)
{
    UINT i, k;
    double w = *(int *)ADDR_BACKBUFFER_W, h = *(int *)ADDR_BACKBUFFER_H;

    for (i = 0; i < count; i++)
        for (k = 0; k < 4; k++) {
            double v = data[i * 4 + k];

            if (fabs(v - w) < 0.5 || fabs(v - h) < 0.5 || fabs(v - 1.0 / w) < 2e-6 || fabs(v - 1.0 / h) < 2e-6 ||
                fabs(v - 0.5 / w) < 1e-6 || fabs(v - 0.5 / h) < 1e-6 || fabs(v - 2.0 / w) < 4e-6 || fabs(v - 2.0 / h) < 4e-6)
                Trace("%s const reg %u [%u] = %.7f (%g %g %g %g)", kind, start + i, k, v, data[i * 4], data[i * 4 + 1], data[i * 4 + 2], data[i * 4 + 3]);
        }
}

/*
 * c45 holds (width, height, 1/width, 1/height) of the screen. Shaders turn pixel positions into
 * texture coordinates with it (effects compare against the scene depth that way), so while a
 * scaled target is bound it has to describe that target instead of the backbuffer.
 */
#define SCREEN_CONST_REG 45
static float g_screen_const[4];
static int g_screen_const_valid;

static void ScaledScreenConst(float *out)
{
    out[0] = (float)(g_screen_const[0] * g_render_scale);
    out[1] = (float)(g_screen_const[1] * g_render_scale);
    out[2] = (float)(g_screen_const[2] / g_render_scale);
    out[3] = (float)(g_screen_const[3] / g_render_scale);
}

static int IsScreenConst(const float *v)
{
    return fabsf(v[0] - (float)*(int *)ADDR_BACKBUFFER_W) < 0.5f && fabsf(v[1] - (float)*(int *)ADDR_BACKBUFFER_H) < 0.5f;
}

static HRESULT SetConstFiltered(IDirect3DDevice9 *dev, SetConstFFn orig, UINT start, const float *data, UINT count)
{
    float copy[256 * 4];

    if (data && start <= SCREEN_CONST_REG && start + count > SCREEN_CONST_REG && count <= 256 &&
        IsScreenConst(data + (SCREEN_CONST_REG - start) * 4)) {
        memcpy(g_screen_const, data + (SCREEN_CONST_REG - start) * 4, sizeof g_screen_const);
        g_screen_const_valid = 1;
        if (g_rt0_scaled && g_render_scale != 1.0) {
            memcpy(copy, data, (size_t)count * 16);
            ScaledScreenConst(copy + (SCREEN_CONST_REG - start) * 4);
            return orig(dev, start, copy, count);
        }
    }
    return orig(dev, start, data, count);
}

static void ResendScreenConst(IDirect3DDevice9 *dev)
{
    float v[4];

    if (!g_screen_const_valid)
        return;
    if (g_rt0_scaled && g_render_scale != 1.0)
        ScaledScreenConst(v);
    else
        memcpy(v, g_screen_const, sizeof v);
    g_orig_set_vs_const(dev, SCREEN_CONST_REG, v, 1);
    g_orig_set_ps_const(dev, SCREEN_CONST_REG, v, 1);
}

static HRESULT STDMETHODCALLTYPE HookSetVSConst(IDirect3DDevice9 *dev, UINT start, const float *data, UINT count)
{
    if (g_trace_rt && data)
        TraceConstants("VS", start, data, count);
    return SetConstFiltered(dev, g_orig_set_vs_const, start, data, count);
}

static HRESULT STDMETHODCALLTYPE HookSetPSConst(IDirect3DDevice9 *dev, UINT start, const float *data, UINT count)
{
    if (g_trace_rt && data)
        TraceConstants("PS", start, data, count);
    return SetConstFiltered(dev, g_orig_set_ps_const, start, data, count);
}


/* ---- debug: texture dump (DumpTextures=1) -------------------------------------------- */
#define VT_CREATE_TEXTURE 23
typedef HRESULT (STDMETHODCALLTYPE *CreateTextureFn)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);
static CreateTextureFn g_orig_create_texture;
static IDirect3DTexture9 *g_buttons_tex;   /* 256x128 DXT5 atlas with the Xbox glyphs */
static int g_dump_textures;
#define MAX_DUMP_TEX 4096
static IDirect3DTexture9 *g_tex_list[MAX_DUMP_TEX];
static int g_tex_count;

static HRESULT STDMETHODCALLTYPE HookCreateTexture(IDirect3DDevice9 *dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *shared)
{
    HRESULT hr = g_orig_create_texture(dev, w, h, levels, usage, fmt, pool, out, shared);

    if (SUCCEEDED(hr) && out && *out && w == 256 && h == 128 && fmt == D3DFMT_DXT5 && !g_buttons_tex)
        g_buttons_tex = *out;
    if (g_dump_textures && SUCCEEDED(hr) && out && *out && g_tex_count < MAX_DUMP_TEX && !(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))
        g_tex_list[g_tex_count++] = *out;
    return hr;
}

static void Rgb565(uint16_t c, uint8_t *o)
{
    o[0] = (uint8_t)((c >> 11 & 31) * 255 / 31);
    o[1] = (uint8_t)((c >> 5 & 63) * 255 / 63);
    o[2] = (uint8_t)((c & 31) * 255 / 31);
}

static void DecodeDxt(const uint8_t *b, int kind, uint8_t *px /* 16 * rgba */)
{
    const uint8_t *cb = kind == 1 ? b : b + 8;
    uint16_t c0 = (uint16_t)(cb[0] | cb[1] << 8), c1 = (uint16_t)(cb[2] | cb[3] << 8);
    uint8_t col[4][4];
    uint32_t bits = (uint32_t)(cb[4] | cb[5] << 8 | cb[6] << 16 | (uint32_t)cb[7] << 24);
    int i, k;

    Rgb565(c0, col[0]);
    Rgb565(c1, col[1]);
    col[0][3] = col[1][3] = col[2][3] = col[3][3] = 255;
    for (k = 0; k < 3; k++) {
        if (kind != 1 || c0 > c1) {
            col[2][k] = (uint8_t)((2 * col[0][k] + col[1][k]) / 3);
            col[3][k] = (uint8_t)((col[0][k] + 2 * col[1][k]) / 3);
        } else {
            col[2][k] = (uint8_t)((col[0][k] + col[1][k]) / 2);
            col[3][k] = 0;
        }
    }
    if (kind == 1 && c0 <= c1)
        col[3][3] = 0;
    for (i = 0; i < 16; i++) {
        const uint8_t *c = col[bits >> (2 * i) & 3];

        px[i * 4 + 0] = c[0];
        px[i * 4 + 1] = c[1];
        px[i * 4 + 2] = c[2];
        px[i * 4 + 3] = c[3];
        if (kind == 3) {
            int a = b[i / 2] >> (4 * (i & 1)) & 15;

            px[i * 4 + 3] = (uint8_t)(a * 17);
        } else if (kind == 5) {
            uint8_t a0 = b[0], a1 = b[1], al[8];
            uint64_t ab = 0;
            int j;

            for (j = 0; j < 6; j++)
                ab |= (uint64_t)b[2 + j] << (8 * j);
            al[0] = a0;
            al[1] = a1;
            for (j = 0; j < 6; j++)
                al[2 + j] = a0 > a1 ? (uint8_t)(((6 - j) * a0 + (j + 1) * a1) / 7) : j < 4 ? (uint8_t)(((4 - j) * a0 + (j + 1) * a1) / 5) : (j == 4 ? 0 : 255);
            px[i * 4 + 3] = al[ab >> (3 * i) & 7];
        }
    }
}

static void WriteTga(const char *path, int w, int h, const uint8_t *rgba)
{
    FILE *f = fopen(path, "wb");
    uint8_t hdr[18] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)h, (uint8_t)(h >> 8), 32, 0x28 };
    int i;

    if (!f)
        return;
    fwrite(hdr, 1, 18, f);
    for (i = 0; i < w * h; i++) {
        uint8_t p[4] = { rgba[i * 4 + 2], rgba[i * 4 + 1], rgba[i * 4 + 0], rgba[i * 4 + 3] };

        fwrite(p, 1, 4, f);
    }
    fclose(f);
}

static void DumpTextures(void)
{
    int i;

    for (i = 0; i < g_tex_count; i++) {
        IDirect3DTexture9 *t = g_tex_list[i];
        D3DSURFACE_DESC d;
        D3DLOCKED_RECT lr;
        uint8_t *rgba;
        int x, y, ok = 0;
        char path[MAX_PATH], dir[MAX_PATH];

        if (!t || FAILED(IDirect3DTexture9_GetLevelDesc(t, 0, &d)) || d.Width > 1024 || d.Height > 1024 || d.Width < 32)
            continue;
        if (FAILED(IDirect3DTexture9_LockRect(t, 0, &lr, NULL, D3DLOCK_READONLY)))
            continue;
        rgba = calloc((size_t)d.Width * d.Height, 4);
        if (rgba) {
            if (d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8) {
                for (y = 0; y < (int)d.Height; y++)
                    for (x = 0; x < (int)d.Width; x++) {
                        const uint8_t *p = (const uint8_t *)lr.pBits + y * lr.Pitch + x * 4;
                        uint8_t *o = rgba + ((size_t)y * d.Width + x) * 4;

                        o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = d.Format == D3DFMT_X8R8G8B8 ? 255 : p[3];
                    }
                ok = 1;
            } else if (d.Format == D3DFMT_DXT1 || d.Format == D3DFMT_DXT3 || d.Format == D3DFMT_DXT5) {
                int kind = d.Format == D3DFMT_DXT1 ? 1 : d.Format == D3DFMT_DXT3 ? 3 : 5;
                int bs = kind == 1 ? 8 : 16;

                for (y = 0; y < (int)d.Height / 4; y++)
                    for (x = 0; x < (int)d.Width / 4; x++) {
                        uint8_t px[64];
                        int j;

                        DecodeDxt((const uint8_t *)lr.pBits + y * lr.Pitch + x * bs, kind, px);
                        for (j = 0; j < 16; j++)
                            memcpy(rgba + (((size_t)(y * 4 + j / 4)) * d.Width + x * 4 + j % 4) * 4, px + j * 4, 4);
                    }
                ok = 1;
            }
            if (ok) {
                GetModuleFileNameA(NULL, dir, MAX_PATH);
                strrchr(dir, '\\')[1] = 0;
                snprintf(path, sizeof path, "%.200sdump_%04d_%ux%u_f%d.tga", dir, i, d.Width, d.Height, (int)d.Format);
                WriteTga(path, (int)d.Width, (int)d.Height, rgba);
            }
            free(rgba);
        }
        IDirect3DTexture9_UnlockRect(t, 0);
    }
    Log("textures dumped");
}


/* ---- button prompts: PlayStation glyphs in the "Buttons" font ------------------------ */
#define VT_SET_TEXTURE 65
typedef HRESULT (STDMETHODCALLTYPE *SetTextureFn)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);
static SetTextureFn g_orig_set_texture;
static IDirect3DTexture9 *g_combined_tex;  /* 256x256: Xbox glyphs on top, PlayStation glyphs below */
static int g_atlas_failed;
static int g_atlas_ready;                  /* font patched, combined atlas in use */
static int g_prompt_glyphs = 1;            /* PromptGlyphs=0 turns the whole feature off */
static int g_prompt_style[2];              /* per player: 0 auto, 1 Xbox, 2 PlayStation */
int g_label_device;                 /* device the button label being built is for */
#define ATLAS_W 256
#define ATLAS_H 128

static float SdSegment(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay, t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
    float cx, cy;

    t = t < 0 ? 0 : t > 1 ? 1 : t;
    cx = ax + t * dx - px;
    cy = ay + t * dy - py;
    return sqrtf(cx * cx + cy * cy);
}

/* Face button: dark glossy disc with a coloured symbol (0 cross, 1 circle, 2 square, 3 triangle). */
static void DrawFaceButton(uint8_t *rgba, int bx, int by, int size, int symbol)
{
    static const uint8_t col[4][3] = { { 120, 160, 235 }, { 240, 105, 105 }, { 235, 160, 215 }, { 90, 210, 175 } };
    int x, y, sx, sy;

    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++) {
            float acc[4] = { 0, 0, 0, 0 };

            for (sy = 0; sy < 3; sy++)
                for (sx = 0; sx < 3; sx++) {
                    float u = ((x + (sx + 0.5f) / 3) / size) * 2 - 1, v = ((y + (sy + 0.5f) / 3) / size) * 2 - 1;
                    float r = sqrtf(u * u + v * v), d, shade, a;
                    float rgb[3];
                    int k;

                    if (r > 0.97f)
                        continue;
                    shade = 0.5f - v * 0.5f;                           /* lighter at the top */
                    for (k = 0; k < 3; k++)
                        rgb[k] = 28 + 40 * shade + (r > 0.85f ? 40 : 0);
                    if (symbol == 0)
                        d = fminf(SdSegment(u, v, -0.42f, -0.42f, 0.42f, 0.42f), SdSegment(u, v, -0.42f, 0.42f, 0.42f, -0.42f)) - 0.09f;
                    else if (symbol == 1)
                        d = fabsf(r - 0.46f) - 0.085f;
                    else if (symbol == 2)
                        d = fabsf(fmaxf(fabsf(u), fabsf(v)) - 0.40f) - 0.085f;
                    else {
                        float e = fminf(fminf(SdSegment(u, v, 0, -0.5f, 0.47f, 0.34f), SdSegment(u, v, 0.47f, 0.34f, -0.47f, 0.34f)),
                                        SdSegment(u, v, -0.47f, 0.34f, 0, -0.5f));

                        d = e - 0.085f;
                    }
                    if (d < 0)
                        for (k = 0; k < 3; k++)
                            rgb[k] = col[symbol][k];
                    a = r > 0.92f ? (0.97f - r) / 0.05f : 1.0f;
                    acc[0] += rgb[0] * a;
                    acc[1] += rgb[1] * a;
                    acc[2] += rgb[2] * a;
                    acc[3] += a;
                }
            {
                uint8_t *o = rgba + ((size_t)(by + y) * ATLAS_W + bx + x) * 4;

                if (acc[3] > 0.01f) {
                    o[0] = (uint8_t)(acc[0] / acc[3]);
                    o[1] = (uint8_t)(acc[1] / acc[3]);
                    o[2] = (uint8_t)(acc[2] / acc[3]);
                }
                o[3] = (uint8_t)(acc[3] / 9.0f * 255.0f);
            }
        }
}

/* Replaces the label of a shoulder / trigger glyph ("LB" ...) with "L1" ... in place. */
static void RelabelShoulder(uint8_t *rgba, int bx, int by, int ex, int ey, int ew, int eh, int cx, int cy, char side, char num)
{
    static const char *font[4][7] = {
        { "10000", "10000", "10000", "10000", "10000", "10000", "11111" },   /* L */
        { "11110", "10001", "10001", "11110", "10100", "10010", "10001" },   /* R */
        { "00100", "01100", "00100", "00100", "00100", "00100", "01110" },   /* 1 */
        { "01110", "10001", "00001", "00010", "00100", "01000", "11111" },   /* 2 */
    };
    enum { GW = 8, GH = 11 };
    int x, y, k;
    char text[2] = { side, num };

    for (y = ey; y < ey + eh; y++)
        for (x = ex; x < ex + ew; x++) {
            uint8_t *o = rgba + ((size_t)(by + y) * ATLAS_W + bx + x) * 4;

            if (o[3] > 128)
                o[0] = o[1] = o[2] = 228;
        }
    for (k = 0; k < 2; k++) {
        int glyph = text[k] == 'L' ? 0 : text[k] == 'R' ? 1 : text[k] == '1' ? 2 : 3;
        int ox = cx - (2 * GW + 1) / 2 + k * (GW + 1), oy = cy - GH / 2;

        for (y = 0; y < GH; y++)
            for (x = 0; x < GW; x++)
                if (font[glyph][y * 7 / GH][x * 5 / GW] == '1') {
                    uint8_t *o = rgba + ((size_t)(by + oy + y) * ATLAS_W + bx + ox + x) * 4;

                    if (o[3] > 128)
                        o[0] = o[1] = o[2] = 55;
                }
    }
}

#define COMBINED_H 256
#define PS_ROW_OFFSET 128

static void BuildCombinedAtlas(IDirect3DDevice9 *dev)
{
    D3DLOCKED_RECT lr;
    uint8_t *rgba;
    IDirect3DTexture9 *tex = NULL;
    int x, y;

    g_atlas_failed = 1;
    if (!g_buttons_tex || FAILED(IDirect3DTexture9_LockRect(g_buttons_tex, 0, &lr, NULL, D3DLOCK_READONLY)))
        return;
    rgba = calloc((size_t)ATLAS_W * COMBINED_H, 4);
    if (!rgba) {
        IDirect3DTexture9_UnlockRect(g_buttons_tex, 0);
        return;
    }
    for (y = 0; y < ATLAS_H / 4; y++)
        for (x = 0; x < ATLAS_W / 4; x++) {
            uint8_t px[64];
            int j;

            DecodeDxt((const uint8_t *)lr.pBits + y * lr.Pitch + x * 16, 5, px);
            for (j = 0; j < 16; j++)
                memcpy(rgba + (((size_t)(y * 4 + j / 4)) * ATLAS_W + x * 4 + j % 4) * 4, px + j * 4, 4);
        }
    IDirect3DTexture9_UnlockRect(g_buttons_tex, 0);

    /* The two glyph rows that get PlayStation versions are copied below the original atlas. */
    memcpy(rgba + (size_t)PS_ROW_OFFSET * ATLAS_W * 4, rgba, (size_t)72 * ATLAS_W * 4);
    DrawFaceButton(rgba, 4, PS_ROW_OFFSET + 4, 28, 0);       /* A -> cross */
    DrawFaceButton(rgba, 40, PS_ROW_OFFSET + 4, 28, 1);      /* B -> circle */
    DrawFaceButton(rgba, 76, PS_ROW_OFFSET + 4, 28, 2);      /* X -> square */
    DrawFaceButton(rgba, 112, PS_ROW_OFFSET + 4, 28, 3);     /* Y -> triangle */
    RelabelShoulder(rgba, 145, PS_ROW_OFFSET + 6, 18, 1, 26, 16, 30, 8, 'L', '1');
    RelabelShoulder(rgba, 204, PS_ROW_OFFSET + 6, 8, 1, 26, 16, 20, 8, 'R', '1');
    RelabelShoulder(rgba, 7, PS_ROW_OFFSET + 37, 4, 16, 24, 16, 14, 24, 'L', '2');
    RelabelShoulder(rgba, 50, PS_ROW_OFFSET + 37, 6, 17, 20, 14, 14, 25, 'R', '2');

    if (SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, ATLAS_W, COMBINED_H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL)) &&
        SUCCEEDED(IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0))) {
        for (y = 0; y < COMBINED_H; y++)
            for (x = 0; x < ATLAS_W; x++) {
                const uint8_t *p = rgba + ((size_t)y * ATLAS_W + x) * 4;
                uint8_t *o = (uint8_t *)lr.pBits + y * lr.Pitch + x * 4;

                o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3];
            }
        IDirect3DTexture9_UnlockRect(tex, 0);
        g_combined_tex = tex;
        g_atlas_failed = 0;
        Log("combined button atlas built");
        if (g_dump_textures)
            WriteTga("ps_buttons.tga", ATLAS_W, COMBINED_H, rgba);
    }
    free(rgba);
}

static HRESULT STDMETHODCALLTYPE HookSetTexture(IDirect3DDevice9 *dev, DWORD stage, IDirect3DBaseTexture9 *tex)
{
    if (g_atlas_ready && tex && (void *)tex == (void *)g_buttons_tex)
        tex = (IDirect3DBaseTexture9 *)g_combined_tex;
    return g_orig_set_texture(dev, stage, tex);
}

/*
 * The Buttons font (dword_921EA4) maps Unicode points U+0531..U+053F to glyph cells of the
 * atlas. Eight more glyphs (PlayStation cross / circle / square / triangle / L1 / R1 / L2 / R2)
 * are added as U+0521..U+0528: the glyph table is moved to a bigger block, the code table gets
 * its unused tail entries, and the font's idea of the atlas size becomes 256x256.
 */
#define ADDR_BUTTONS_FONT   0x00921EA4u
#define ADDR_TEXTURE_SIZES  0x0097157Cu   /* dword_97157C: pointer to an array of {width, height} pointers */
#define PS_GLYPH_COUNT 8
#define ADDR_GLYPH_RANGE_1 0x0050CA02u   /* cmp bl, 0B1h: lowest second byte of a button glyph (draw) */
#define ADDR_GLYPH_RANGE_2 0x0050CEFEu   /* ... (measure) */

static int PatchButtonsFont(void)
{
    static const float cells[PS_GLYPH_COUNT][3] = {          /* x, row y, width */
        { 4, 0, 28 }, { 40, 0, 28 }, { 76, 0, 28 }, { 112, 0, 28 }, { 145, 0, 51 }, { 204, 0, 51 },
        { 7, 36, 30 }, { 50, 36, 29 },
    };
    uint8_t *font = *(uint8_t **)ADDR_BUTTONS_FONT;
    uint32_t *codes;
    float *old_table, *table;
    int *sizes, id, i, count;

    if (!font || !g_combined_tex)
        return 0;
    id = *(short *)(*(uint8_t **)(font + 64) + 116);
    if (id <= 0 || id > 4096)
        return 0;
    sizes = *(int **)(*(uint8_t **)ADDR_TEXTURE_SIZES + 4 * (id - 1));
    old_table = *(float **)(font + 0x34);
    codes = *(uint32_t **)(font + 0x38);
    count = *(int *)(font + 0x10);
    if (!sizes || sizes[0] != ATLAS_W || sizes[1] != ATLAS_H || !old_table || !codes || count < 24 ||
        codes[16] != 0xFFFFFFFFu || (codes[15] & 0xFFFF) != 0x53F)
        return 0;                                       /* not the layout this was written for */

    if (*(uint8_t *)ADDR_GLYPH_RANGE_1 != 0xB1 || *(uint8_t *)ADDR_GLYPH_RANGE_2 != 0xB1)
        return 0;
    table = calloc(17 + PS_GLYPH_COUNT, 12);
    if (!table)
        return 0;
    memcpy(table, old_table, 17 * 12);
    for (i = 0; i < PS_GLYPH_COUNT; i++) {
        table[(17 + i) * 3 + 0] = cells[i][0];
        table[(17 + i) * 3 + 1] = cells[i][1] + PS_ROW_OFFSET;
        table[(17 + i) * 3 + 2] = cells[i][2];
    }
    /* code table: sorted (code, glyph) pairs; U+0521..U+0528 go in front of U+0531.. */
    memmove(codes + 1 + PS_GLYPH_COUNT, codes + 1, 15 * sizeof(uint32_t));
    for (i = 0; i < PS_GLYPH_COUNT; i++)
        codes[1 + i] = (uint32_t)(0x521 + i) | (uint32_t)(17 + i) << 16;
    {
        /* the text splitter only treats U+0531..U+053F (lead D4, second byte B1..BF) as button glyphs */
        uint8_t low = 0xA1;

        WritePatch(ADDR_GLYPH_RANGE_1, &low, 1);
        WritePatch(ADDR_GLYPH_RANGE_2, &low, 1);
    }
    *(float **)(font + 0x34) = table;
    sizes[1] = COMBINED_H;
    Log("button font patched for PlayStation glyphs");
    return 1;
}

/* Replacement for the button label callback sub_50C140(button, out). */
static const char *const k_ps_glyph[12] = {
    "\xD4\xA1", "\xD4\xA2", "\xD4\xA3", "\xD4\xA4", "\xD4\xA5", "\xD4\xA6", NULL, NULL, NULL, NULL, "\xD4\xA8", "\xD4\xA7",
};
#define ADDR_BUTTON_GLYPHS 0x008015F8u    /* off_8015F8[12]: glyph string per pad button */
#define ADDR_HAT_GLYPHS    0x00801428u    /* dword_801428[128..131] */
#define ADDR_LABEL_CALLBACK 0x029755D8u   /* dword_29755D8 */
#define ORIG_LABEL_CALLBACK 0x0050C140u
static int g_sony[16];

static int StyleIsPlayStation(int device)
{
    int slot = 0, s;

    for (s = 0; s < 2; s++)
        if (*(int *)(ADDR_SLOT_ASSIGNED + 12 * s) && *(int *)(ADDR_SLOT_DEVICE + 12 * s) == device)
            slot = s;
    if (g_prompt_style[slot] == 2)
        return 1;
    if (g_prompt_style[slot] == 1)
        return 0;
    return device >= 0 && device < FIRST_KEYBOARD_DEVICE && g_sony[device];
}

static void __cdecl GlyphCallback(unsigned button, char *out)
{
    const char *glyph = NULL;

    if (button <= 11) {
        if (g_atlas_ready && k_ps_glyph[button] && StyleIsPlayStation(g_label_device))
            glyph = k_ps_glyph[button];
        else
            glyph = ((const char **)ADDR_BUTTON_GLYPHS)[button];
    } else if (button - 128 <= 3) {
        glyph = ((const char **)ADDR_HAT_GLYPHS)[button];
    }
    if (glyph)
        strcpy(out, glyph);
    else
        out[0] = 0;
}

/* sub_6D87F0 builds the label of one input for a device; remember which device. */
#define ADDR_LABEL_BUILDER 0x006D87F0u
extern void LabelBuilderHook(void);
__asm__(
    ".text\n"
    ".globl _LabelBuilderHook\n"
    "_LabelBuilderHook:\n"
    "    movl 4(%esp), %eax\n"
    "    movl %eax, _g_label_device\n"
    "    subl $0x2C, %esp\n"
    "    movl 0x829570, %eax\n"
    "    pushl $0x6D87F8\n"
    "    ret\n"
);

/* Is the gamepad with this index a Sony controller (visible only when Steam Input is not hiding it)? */
typedef UINT (WINAPI *JoyGetDevCapsFn)(UINT_PTR, void *, UINT);

static int PadIsSony(int pad)
{
    static JoyGetDevCapsFn fn;
    static int tried;
    unsigned char caps[512];

    if (!tried) {
        HMODULE m = LoadLibraryA("winmm.dll");

        tried = 1;
        if (m)
            fn = (JoyGetDevCapsFn)(void *)GetProcAddress(m, "joyGetDevCapsA");
    }
    memset(&caps, 0, sizeof caps);
    return fn && pad >= 0 && pad < 16 && fn((UINT_PTR)pad, caps, 404) == 0 && *(WORD *)caps == 0x054C;
}

static void UpdatePromptStyle(void)
{
    static int frames;
    IDirect3DDevice9 *dev = *(IDirect3DDevice9 **)(ADDR_DEVICE_OBJECT + 4);

    if (frames++ % 120 == 0) {
        int i;

        for (i = 0; i < 16; i++)
            g_sony[i] = PadIsSony(i);
    }
    if (g_prompt_glyphs && !g_atlas_ready && !g_atlas_failed && dev && g_buttons_tex && *(uint8_t **)ADDR_BUTTONS_FONT) {
        BuildCombinedAtlas(dev);
        if (g_combined_tex && PatchButtonsFont()) {
            g_atlas_ready = 1;
            if (*(uint32_t *)ADDR_LABEL_CALLBACK == ORIG_LABEL_CALLBACK)
                *(uint32_t *)ADDR_LABEL_CALLBACK = (uint32_t)(uintptr_t)GlyphCallback;
        } else {
            g_atlas_failed = 1;
            Log("button atlas not patched");
        }
    }
}

static void PatchVtableEntry(void **vtable, int index, void *hook, void **orig)
{
    DWORD old;

    if (!VirtualProtect(&vtable[index], sizeof(void *), PAGE_READWRITE, &old))
        return;
    *orig = vtable[index];
    vtable[index] = hook;
    VirtualProtect(&vtable[index], sizeof(void *), old, &old);
}

/* Called every frame; hooks the device once it exists. */
static void InstallRenderScaleHooks(void)
{
    IDirect3DDevice9 *dev;
    void **vtable;

    dev = *(IDirect3DDevice9 **)(ADDR_DEVICE_OBJECT + 4);
    if (!dev)
        return;
    vtable = *(void ***)dev;
    if (vtable == g_hooked_vtable)
        return;
    PatchVtableEntry(vtable, VT_SET_RENDER_TARGET, (void *)HookSetRenderTarget, (void **)&g_orig_set_rt);
    PatchVtableEntry(vtable, VT_SET_VIEWPORT, (void *)HookSetViewport, (void **)&g_orig_set_viewport);
    PatchVtableEntry(vtable, VT_SET_DEPTH_STENCIL, (void *)HookSetDepthStencil, (void **)&g_orig_set_depth);
    PatchVtableEntry(vtable, VT_SET_SCISSOR_RECT, (void *)HookSetScissorRect, (void **)&g_orig_set_scissor);
    PatchVtableEntry(vtable, VT_CREATE_TEXTURE, (void *)HookCreateTexture, (void **)&g_orig_create_texture);
    PatchVtableEntry(vtable, VT_SET_TEXTURE, (void *)HookSetTexture, (void **)&g_orig_set_texture);
    if (g_trace_passes || g_skip_pass >= 0 || g_skip_scaled >= 0) {
        PatchVtableEntry(vtable, VT_END_SCENE, (void *)HookEndScene, (void **)&g_orig_end_scene);
        PatchVtableEntry(vtable, VT_DRAW_PRIMITIVE, (void *)HookDrawPrim, (void **)&g_orig_draw_prim);
        PatchVtableEntry(vtable, VT_DRAW_INDEXED, (void *)HookDrawIdx, (void **)&g_orig_draw_idx);
        PatchVtableEntry(vtable, VT_DRAW_PRIMITIVE_UP, (void *)HookDrawPrimUP, (void **)&g_orig_draw_prim_up);
        PatchVtableEntry(vtable, VT_DRAW_INDEXED_UP, (void *)HookDrawIdxUP, (void **)&g_orig_draw_idx_up);
    }
    PatchVtableEntry(vtable, VT_PRESENT, (void *)HookPresent, (void **)&g_orig_present);
    PatchVtableEntry(vtable, VT_RESET, (void *)HookReset, (void **)&g_orig_reset);
    PatchVtableEntry(vtable, VT_SET_VS_CONST_F, (void *)HookSetVSConst, (void **)&g_orig_set_vs_const);
    PatchVtableEntry(vtable, VT_SET_PS_CONST_F, (void *)HookSetPSConst, (void **)&g_orig_set_ps_const);
    g_hooked_vtable = vtable;
    Log("render scale: device hooks installed");
}

/*
 * Native "Render Scale" row on the Video Settings page (menu id 0x401), placed just above the
 * Back / Accept row. The page's draw routine (sub_511EA0) draws its rows with sub_50F740, which
 * also counts them for the cursor range; a call is patched in right before the Back row to draw
 * one more. The row sits at index 4 and pushes Back to index 5, so the update wrapper handles
 * the new row itself and shows the original update routine Back as row 4 again. Changing the
 * value rebuilds the render targets (sub_6E2D20, the routine a device reset uses) and stores
 * the choice in the ini.
 */
#define ADDR_VID_MENU_UPDATE  0x00800D70u  /* menu table entry 0x401, update callback */
#define VID_MENU_UPDATE_ORIG  0x0050EF80u
#define ADDR_VID_ROW_INSERT   0x005120E6u  /* mov ecx, offset device: first instruction of the Back row code */
#define ADDR_MENU_ALPHA       0x00921D60u  /* dword_921D60[0]: alpha of a normal row */
#define ADDR_MENU_ROW_TEXT    0x0050F740u  /* sub_50F740(menu, text, alpha): draws and counts one row */
#define ADDR_BUILD_TARGETS    0x006E2D20u  /* sub_6E2D20(this): (re)creates the effect render targets */
#define ADDR_MENU_SOUND       0x00800940u
#define ADDR_MENU_SOUND_ID    0x00800950u
#define MENU_BUSY_OFS         0x68  /* dword: update is handled elsewhere while set */
#define MENU_CUR_ROW_OFS      10    /* word */
#define SCALE_ROW             4
#define BACK_ROW              5
#define MENU_LEFT_OFS         0x5C
#define MENU_RIGHT_OFS        0x60
#define MENU_SELECT_OFS       0x64

typedef int (__cdecl *MenuDrawFn)(int menu);
typedef int (__cdecl *MenuUpdateFn)(int menu);
typedef int (__cdecl *MenuRowFn)(int menu, const char *text, int alpha);
typedef int (__attribute__((thiscall)) *BuildTargetsFn)(void *device);

static const double k_scale_steps[] = { 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 3.0 };
#define SCALE_STEPS ((int)(sizeof k_scale_steps / sizeof k_scale_steps[0]))

static int ScaleStepIndex(void)
{
    int i, best = 0;
    double d, bestd = 1e9;

    for (i = 0; i < SCALE_STEPS; i++) {
        d = k_scale_steps[i] > g_render_scale ? k_scale_steps[i] - g_render_scale : g_render_scale - k_scale_steps[i];
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    return best;
}

void __cdecl VideoRowDraw(int menu)
{
    char text[48];

    snprintf(text, sizeof text, "Render Scale: %d%%", (int)(g_render_scale * 100.0 + 0.5));
    ((MenuRowFn)ADDR_MENU_ROW_TEXT)(menu, text, *(int *)ADDR_MENU_ALPHA);
}

/* Replaces "mov ecx, offset device" in sub_511EA0; the menu pointer is in edi there. */
extern void VideoRowStub(void);
__asm__(
    ".text\n"
    ".globl _VideoRowStub\n"
    "_VideoRowStub:\n"
    "    pushl %edi\n"
    "    call _VideoRowDraw\n"
    "    addl $4, %esp\n"
    "    movl $0x029765C8, %ecx\n"
    "    ret\n"
);

static void StepRenderScale(int step)
{
    int idx = (ScaleStepIndex() + step + SCALE_STEPS) % SCALE_STEPS;
    char buf[16];

    g_render_scale = k_scale_steps[idx];
    snprintf(buf, sizeof buf, "%.2f", g_render_scale);
    WritePrivateProfileStringA("SagaTweaks", "RenderScale", buf, g_ini_path);
    ((BuildTargetsFn)ADDR_BUILD_TARGETS)((void *)ADDR_DEVICE_OBJECT);
    *(int *)ADDR_MENU_SOUND = *(int *)ADDR_MENU_SOUND_ID;
    Log("render scale changed from the menu");
}

static int __cdecl VideoMenuUpdate(int menu)
{
    short *row = (short *)(menu + MENU_CUR_ROW_OFS);
    int result;

    if (*(int *)(menu + MENU_BUSY_OFS))
        return ((MenuUpdateFn)VID_MENU_UPDATE_ORIG)(menu);

    if (*row == SCALE_ROW) {
        int step = *(int *)(menu + MENU_RIGHT_OFS) || *(int *)(menu + MENU_SELECT_OFS) ? 1 :
                   *(int *)(menu + MENU_LEFT_OFS) ? -1 : 0;

        if (step)
            StepRenderScale(step);
        return 0;
    }
    if (*row == BACK_ROW) {
        *row = SCALE_ROW;                         /* the original routine knows Back as row 4 */
        result = ((MenuUpdateFn)VID_MENU_UPDATE_ORIG)(menu);
        if (*row == SCALE_ROW)
            *row = BACK_ROW;
        return result;
    }
    return ((MenuUpdateFn)VID_MENU_UPDATE_ORIG)(menu);
}

/*
 * "Tweaks" page: every option of SagaTweaks.ini as a menu row. It lives in menu table entry
 * 0x406, a placeholder with empty callbacks that nothing opens, and is reached through a
 * "Tweaks" row appended to the main menu and the pause menu.
 */
#define ADDR_TWEAKS_DRAW      0x00800DF8u  /* menu table entry 0x406, draw callback */
#define ADDR_TWEAKS_UPDATE    0x00800DFCu  /* ... update callback */
#define MENU_TWEAKS           0x406
#define ADDR_MAIN_UPDATE      0x00800E34u  /* menu table entry 1 (main menu), update callback */
#define MAIN_UPDATE_ORIG      0x00494E20u
#define ADDR_PAUSE_UPDATE     0x00800E50u  /* menu table entry 2 (pause menu) */
#define PAUSE_UPDATE_ORIG     0x00495900u
#define ADDR_MAIN_QUIT_CALL   0x00495274u  /* call sub_50F740: draws the last row (Quit) of the main menu */
#define ADDR_PAUSE_QUIT_CALL_1 0x00495CD3u /* call sub_574DE0: the three variants of the pause menu's last row */
#define ADDR_PAUSE_QUIT_CALL_2 0x00495CF3u
#define ADDR_PAUSE_QUIT_CALL_3 0x00495D13u
#define ADDR_PAUSE_ROW_DRAW   0x00574DE0u  /* sub_574DE0(menu, text) */
#define ADDR_MENU_POP         0x00509AB0u  /* sub_509AB0(): close the current menu */
#define ADDR_TEXT_READY       0x0297AE1C   /* dword_297AE1C: localized strings loaded flag */
#define ADDR_TEXT_INIT        0x006DED20u
#define ADDR_TEXT_BACK        0x0297ADA8u  /* dword_297ADA8: "Back" */
#define MENU_SELECT_FLAG_OFS  0x64
#define MENU_MAX_ROW_OFS      18

enum { OPT_BOOL, OPT_LIST, OPT_SCALE, OPT_NAMED };

typedef struct {
    const char *label;
    const char *key;
    int *var;
    int type;
    int restart;
    const int *list;
    int count;
    const char *zero;       /* text for the value 0 of a list */
    const char *unit;
    const char *const *names;   /* OPT_NAMED: value texts, count in .count */
} TweakOpt;

static const char *const k_prompt_names[] = { "Auto", "Xbox", "PlayStation" };
static const int k_fps_list[] = { 0, 30, 60, 90, 120, 144, 165, 240 };
static const int k_hz_list[]  = { 0, 60, 75, 90, 120, 144, 165, 240 };

static const TweakOpt k_tweaks[] = {
    { "Skip Logos (restart)",        "SkipLogos",        &g_skip_logos,    OPT_BOOL,  1, NULL, 0, NULL, NULL, NULL },
    { "Skip Title Screen",           "SkipTitle",        &g_skip_title,    OPT_BOOL,  0, NULL, 0, NULL, NULL, NULL },
    { "Hot-Switch Input",            "HotSwitchInput",   &g_hot_switch,    OPT_BOOL,  0, NULL, 0, NULL, NULL, NULL },
    { "Join With Start",             "JoinWithStart",    &g_join_start,    OPT_BOOL,  0, NULL, 0, NULL, NULL, NULL },
    { "Player 1 Button Prompts",     "ButtonPromptsP1",  &g_prompt_style[0], OPT_NAMED, 0, NULL, 3, NULL, NULL, k_prompt_names },
    { "Player 2 Button Prompts",     "ButtonPromptsP2",  &g_prompt_style[1], OPT_NAMED, 0, NULL, 3, NULL, NULL, k_prompt_names },
    { "FPS Limit",                   "MaxFPS",           &g_max_fps,       OPT_LIST,  0, k_fps_list, 8, "Unlimited", " FPS", NULL },
    { "Refresh Rate",                "RefreshRate",      &g_refresh_override, OPT_LIST, 0, k_hz_list, 8, "Auto", " Hz", NULL },
    { "Render Scale",                "RenderScale",      NULL,             OPT_SCALE, 0, NULL, 0, NULL, NULL, NULL },
    { "Fix Frame Counters",          "FixFrameCounters", &g_fix_counters,  OPT_BOOL,  0, NULL, 0, NULL, NULL, NULL },
    { "Fix Jump Physics",            "FixJumpPhysics",   &g_fix_physics,   OPT_BOOL,  0, NULL, 0, NULL, NULL, NULL },
    { "Skip Cutscenes (Start)",      "SkipCutscenes",    &g_skip_cutscenes, OPT_BOOL, 0, NULL, 0, NULL, NULL, NULL },
};
#define TWEAK_COUNT ((int)(sizeof k_tweaks / sizeof k_tweaks[0]))

static void TweakValueText(const TweakOpt *o, char *out, size_t size)
{
    switch (o->type) {
    case OPT_BOOL:
        snprintf(out, size, "%s", *o->var ? "On" : "Off");
        break;
    case OPT_NAMED:
        snprintf(out, size, "%s", o->names[*o->var]);
        break;
    case OPT_LIST:
        if (*o->var == 0)
            snprintf(out, size, "%s", o->zero);
        else
            snprintf(out, size, "%d%s", *o->var, o->unit);
        break;
    default:
        snprintf(out, size, "%d%%", (int)(g_render_scale * 100.0 + 0.5));
        break;
    }
}

static void TweakStore(const TweakOpt *o)
{
    char buf[16];

    if (o->type == OPT_SCALE)
        snprintf(buf, sizeof buf, "%.2f", g_render_scale);
    else
        snprintf(buf, sizeof buf, "%d", *o->var);
    WritePrivateProfileStringA("SagaTweaks", o->key, buf, g_ini_path);
}

static void TweakStep(const TweakOpt *o, int step)
{
    int i;

    switch (o->type) {
    case OPT_BOOL:
        *o->var = !*o->var;
        break;
    case OPT_NAMED:
        *o->var = (*o->var + step + o->count) % o->count;
        break;
    case OPT_LIST:
        for (i = 0; i < o->count - 1 && o->list[i] != *o->var; i++)
            ;
        if (o->list[i] != *o->var)
            i = 0;
        *o->var = o->list[(i + step + o->count) % o->count];
        if (o->var == &g_max_fps)
            ApplyMaxFps();
        break;
    default:
        StepRenderScale(step);                     /* stores the value itself */
        return;
    }
    TweakStore(o);
}

static int __cdecl TweaksDraw(int menu)
{
    char value[24], text[64];
    int i;

    for (i = 0; i < TWEAK_COUNT; i++) {
        TweakValueText(&k_tweaks[i], value, sizeof value);
        snprintf(text, sizeof text, "%s: %s", k_tweaks[i].label, value);
        ((MenuRowFn)ADDR_MENU_ROW_TEXT)(menu, text, *(int *)ADDR_MENU_ALPHA);
    }
    if (!*(int *)ADDR_TEXT_READY)
        ((void (__cdecl *)(void))ADDR_TEXT_INIT)();
    ((MenuRowFn)ADDR_MENU_ROW_TEXT)(menu, *(const char **)ADDR_TEXT_BACK, *(int *)ADDR_MENU_ALPHA);
    return 0;
}

static int __cdecl TweaksUpdate(int menu)
{
    int row = *(short *)(menu + MENU_CUR_ROW_OFS);
    int select = *(int *)(menu + MENU_SELECT_OFS);
    int step;

    if (*(int *)(menu + MENU_BUSY_OFS))
        return 0;
    if (row == TWEAK_COUNT) {
        if (select) {
            *(int *)ADDR_MENU_SOUND = *(int *)ADDR_MENU_SOUND_ID;
            ((void (__cdecl *)(void))ADDR_MENU_POP)();
        }
        return 0;
    }
    if (row < 0 || row > TWEAK_COUNT)
        return 0;
    step = *(int *)(menu + MENU_RIGHT_OFS) || select ? 1 : *(int *)(menu + MENU_LEFT_OFS) ? -1 : 0;
    if (step) {
        TweakStep(&k_tweaks[row], step);
        *(int *)ADDR_MENU_SOUND = *(int *)ADDR_MENU_SOUND_ID;
    }
    return 0;
}


/*
 * The FSAA row (first row of Effects Settings) is hidden: the render scale supersamples the whole
 * frame, and the game's own multisampling cannot work together with its shadow / bloom targets.
 * The single call that draws the row is turned into a no-op and the update wrapper shifts the
 * cursor row by one so the remaining rows reach their original handlers.
 */
#define ADDR_FX_FSAA_ROW_CALL 0x0051242Cu  /* call sub_50F740 at the end of the FSAA row code */
#define ADDR_FX_UPDATE        0x00800D8Cu  /* menu table entry 0x402 (Effects Settings), update callback */
#define FX_UPDATE_ORIG        0x0050F0D0u
#define ADDR_FSAA_DESIRED     0x02976C30u  /* dword_2976C30 */

static int __cdecl HiddenRow(int menu, int text, int alpha)
{
    (void)menu; (void)text; (void)alpha;
    return 0;
}

static int __cdecl EffectsMenuUpdate(int menu)
{
    short *row = (short *)(menu + MENU_CUR_ROW_OFS);
    int *row_copy = (int *)(menu + 52);
    short before = *row;
    int r;

    if (*(int *)(menu + MENU_BUSY_OFS))
        return ((MenuUpdateFn)FX_UPDATE_ORIG)(menu);
    *row = (short)(before + 1);
    *row_copy = before + 1;
    r = ((MenuUpdateFn)FX_UPDATE_ORIG)(menu);
    if (*row == before + 1) {
        *row = before;
        *row_copy = before;
    }
    return r;
}

/* "Tweaks" is drawn right before the last row (Quit) of the main menu and the pause menu. */
static int __cdecl MainQuitRow(int menu, const char *text, int alpha)
{
    ((MenuRowFn)ADDR_MENU_ROW_TEXT)(menu, "Tweaks", alpha);     /* same fade as the other rows */
    return ((MenuRowFn)ADDR_MENU_ROW_TEXT)(menu, text, alpha);
}

typedef int (__cdecl *PauseRowFn)(int menu, const char *text);

static int __cdecl PauseQuitRow(int menu, const char *text)
{
    ((PauseRowFn)ADDR_PAUSE_ROW_DRAW)(menu, "Tweaks");
    return ((PauseRowFn)ADDR_PAUSE_ROW_DRAW)(menu, text);
}

static int OpenTweaksRow(int menu)
{
    int row = *(short *)(menu + MENU_CUR_ROW_OFS);
    int last = *(short *)(menu + MENU_MAX_ROW_OFS);

    if (row != last - 1 || row <= 0 || !*(int *)(menu + MENU_SELECT_FLAG_OFS) || *(int *)(menu + MENU_BUSY_OFS))
        return 0;
    *(int *)ADDR_MENU_SOUND = *(int *)ADDR_MENU_SOUND_ID;
    ((OpenMenuFn)ADDR_OPEN_MENU)(MENU_TWEAKS, 0, -1);
    return 1;
}

/* Quit moved down one row: the original update routine still knows it by its old index. */
static int CallShifted(int menu, MenuUpdateFn orig)
{
    short *row = (short *)(menu + MENU_CUR_ROW_OFS);
    int *row_copy = (int *)(menu + 52);
    int last = *(short *)(menu + MENU_MAX_ROW_OFS), shifted = last > 1 && *row == last, r;

    if (shifted) {
        *row = (short)(last - 1);
        *row_copy = last - 1;
    }
    r = orig(menu);
    if (shifted && *row == last - 1) {
        *row = (short)last;
        *row_copy = last;
    }
    return r;
}

static int __cdecl MainMenuUpdate(int menu)
{
    if (OpenTweaksRow(menu))
        return 0;
    return CallShifted(menu, (MenuUpdateFn)MAIN_UPDATE_ORIG);
}

static int __cdecl PauseMenuUpdate(int menu)
{
    if (OpenTweaksRow(menu))
        return 0;
    return CallShifted(menu, (MenuUpdateFn)PAUSE_UPDATE_ORIG);
}

/* Append the render-scale resolutions to the game's mode table (once). */
void __cdecl AppendRenderScaleModes(char *device)
{
    static int done;
    int count, i, k, added = 0;
    int *table, *fresh, w, h;

    if (done || !g_scale_count)
        return;
    count = *(int *)(device + DEVICE_MODE_COUNT_OFS);
    table = *(int **)(device + DEVICE_MODE_TABLE_OFS);
    if (!table || count <= 0 || count > 256)
        return;                                   /* mode table not built yet */
    done = 1;

    fresh = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)(count + MAX_RENDER_SCALES) * 16);
    if (!fresh)
        return;
    memcpy(fresh, table, (size_t)count * 16);
    for (k = 0; k < g_scale_count; k++) {
        w = ((int)(*(int *)(device + DEVICE_DESKTOP_W_OFS) * g_scales[k] + 0.5)) & ~1;
        h = ((int)(*(int *)(device + DEVICE_DESKTOP_H_OFS) * g_scales[k] + 0.5)) & ~1;
        if (w > MAX_RENDER_DIM || h > MAX_RENDER_DIM)
            continue;
        for (i = 0; i < count + added; i++)
            if (fresh[i * 4] == w && fresh[i * 4 + 1] == h)
                break;
        if (i < count + added)
            continue;
        fresh[(count + added) * 4 + 0] = w;
        fresh[(count + added) * 4 + 1] = h;
        fresh[(count + added) * 4 + 2] = fresh[(count - 1) * 4 + 2];
        fresh[(count + added) * 4 + 3] = fresh[(count - 1) * 4 + 3];
        added++;
        if (g_log)
            fprintf(g_log, "render scale mode added: %dx%d (x%.2f)\n", w, h, g_scales[k]);
    }
    *(int **)(device + DEVICE_MODE_TABLE_OFS) = fresh;
    *(int *)(device + DEVICE_MODE_COUNT_OFS) = count + added;
    fflush(g_log);
}

/* Entry stub for sub_6E1B70: add the modes, then run the original function. */
extern void ModeLookupStub(void);
__asm__(
    ".text\n"
    ".globl _ModeLookupStub\n"
    "_ModeLookupStub:\n"
    "    pushl %ecx\n"
    "    pushl %ecx\n"
    "    call _AppendRenderScaleModes\n"
    "    addl $4, %esp\n"
    "    popl %ecx\n"
    "    jmp *_g_lookup_tramp\n"
);

static void SyncModeTable(void)
{
    static int last_hz;
    int count = *(int *)ADDR_MODE_COUNT;
    int *table = *(int **)ADDR_MODE_TABLE;
    int hz = g_refresh_override ? g_refresh_override : *(int *)ADDR_DESKTOP_HZ;
    int i;

    if (!table || count <= 0 || count > 256 || hz < 24 || hz > 1000 || hz == last_hz)
        return;
    for (i = 0; i < count; i++)
        table[i * 4 + 2] = hz;
    last_hz = hz;
    Log("video mode refresh rates updated");
}

#define ADDR_CONTROLLER_STATUS 0x0054AE40u  /* sub_54AE40(slot): below 2 means that slot has no controller */
#define DROP_DEBOUNCE_FRAMES  60

static unsigned DeviceMask(int device)
{
    unsigned mask = 0;
    int raw;

    for (raw = 0; raw < DEVICE_COUNT; raw++)
        if (((MapDeviceFn)ADDR_MAP_DEVICE)(raw, 0) == device)
            mask |= 1u << raw;
    return mask;
}

static void AssignDevice(int slot, int device)
{
    ((AssignDeviceFn)ADDR_ASSIGN_DEVICE)(slot, device);
    ((EnableDeviceFn)ADDR_ENABLE_DEVICE)(device, 1);
}

/*
 * Player 1 follows the device that was used last (keyboard or gamepad). If player 1
 * is already on a gamepad, other gamepads are left to the game so they join as player 2.
 */
static int g_p1_inactive_frames;

/*
 * If player 1 gets dropped (controller removed, remote play glitches, ...) the game keeps
 * their device assigned. Its join code only looks at devices that are not assigned to a
 * slot, so the dropped player could never press a button to come back. After a short
 * debounce the slot is unassigned so the normal join works again.
 */
static int HandleDroppedPlayer1(void)
{
    int p1 = *(int *)ADDR_PLAYER_OBJ;

    if (!p1 || *(char *)(p1 + PLAYER_ACTIVE_OFS) < 0) {
        g_p1_inactive_frames = 0;
        return 0;
    }
    if (g_p1_inactive_frames == 0 && g_log) {
        int status0 = ((int (__cdecl *)(int))ADDR_CONTROLLER_STATUS)(0);
        int status1 = ((int (__cdecl *)(int))ADDR_CONTROLLER_STATUS)(1);

        fprintf(g_log, "player 1 became inactive: slot 0 device %d (assigned %d), slot 1 device %d, controller status %d/%d\n",
                *(int *)ADDR_SLOT_DEVICE, *(int *)ADDR_SLOT_ASSIGNED, *(int *)(ADDR_SLOT_DEVICE + 12), status0, status1);
        fflush(g_log);
    }
    if (++g_p1_inactive_frames == DROP_DEBOUNCE_FRAMES && *(int *)ADDR_SLOT_ASSIGNED) {
        ((AssignDeviceFn)ADDR_ASSIGN_DEVICE)(0, -1);
        Log("player 1 dropped, device unassigned so they can rejoin");
    }
    return 1;
}

#define ADDR_DEVICE_RECORDS  0x00917000u  /* 132-byte record per device, first byte: device is connected */
#define ADDR_ALT_INPUT_MODE  0x0091EF2Cu   /* dword_91EF2C: records are indexed by slot instead of device */
#define DEVICE_RECORD_SIZE   132

static int DeviceConnected(int device)
{
    if (device < 0 || device >= FIRST_KEYBOARD_DEVICE)
        return 1;
    return *(char *)(ADDR_DEVICE_RECORDS + device * DEVICE_RECORD_SIZE) != 0;
}

/* Log every change of the input / player state so lost-input reports can be traced. */
static void TraceInputState(void)
{
    static int last[12], lines;
    int now[12], i, p, same = 1;

    now[0] = *(int *)ADDR_SLOT_DEVICE;
    now[1] = *(int *)ADDR_SLOT_ASSIGNED;
    now[2] = *(int *)(ADDR_SLOT_DEVICE + 12);
    now[3] = *(int *)(ADDR_SLOT_ASSIGNED + 12);
    now[4] = DeviceConnected(now[0]);
    now[5] = DeviceConnected(now[2]);
    for (i = 0; i < 2; i++) {
        p = *(int *)(ADDR_PLAYER_OBJ + 4 * i);
        now[6 + i] = p ? (*(char *)(p + PLAYER_ACTIVE_OFS) < 0) : -1;
    }
    now[8] = *(int *)ADDR_MENU_DEPTH;
    now[9] = DeviceConnected(0);
    now[10] = DeviceConnected(1);
    now[11] = 0;
    for (i = 0; i < 12; i++)
        if (now[i] != last[i])
            same = 0;
    if (same || !g_log || lines >= 300)
        return;
    memcpy(last, now, sizeof last);
    lines++;
    fprintf(g_log, "input state: slot0 dev %d set %d conn %d | slot1 dev %d set %d conn %d | p1 active %d p2 active %d | menu depth %d | pad0 conn %d pad1 conn %d\n",
            now[0], now[1], now[4], now[2], now[3], now[5], now[6], now[7], now[8], now[9], now[10]);
    fflush(g_log);
}

/*
 * Player 2's gamepad can go away (Remote Play guest leaves, pad unplugged). The game keeps
 * the device assigned to slot 1, so nobody could join as player 2 again. Once the device has been
 * reported disconnected for a short while the slot is released.
 */
static int g_p2_gone_frames;

static void HandleDisconnectedPlayer2(void)
{
    int device = *(int *)(ADDR_SLOT_DEVICE + 12);

    if (!*(int *)(ADDR_SLOT_ASSIGNED + 12) || device < 0 || device >= FIRST_KEYBOARD_DEVICE ||
        *(int *)ADDR_ALT_INPUT_MODE || DeviceConnected(device)) {
        g_p2_gone_frames = 0;
        return;
    }
    if (++g_p2_gone_frames == DROP_DEBOUNCE_FRAMES) {
        ((AssignDeviceFn)ADDR_ASSIGN_DEVICE)(1, -1);
        if (g_log) {
            fprintf(g_log, "player 2 device %d disconnected, slot 1 released\n", device);
            fflush(g_log);
        }
    }
}

#define ADDR_JOIN_DETECT_CALL 0x004280D3u  /* call sub_6D9120 in the join code sub_427EC0 */

/*
 * The game lets any gamepad button press join as another player, while a keyboard has to use
 * F1 / F2. Gamepads are held to the same standard: only Start joins. Devices that pressed
 * something else are left out of the scan so other devices can still be found.
 */
static int __cdecl JoinDetect(int mask, int kinds, int *type, int *code, int skip_taken)
{
    int local_type, local_code, dev, tries;

    if (!g_join_start)
        return ((DetectDeviceFn)ADDR_DETECT_DEVICE)(mask, kinds, type, code, skip_taken);
    for (tries = 0; tries < DEVICE_COUNT; tries++) {
        dev = ((DetectDeviceFn)ADDR_DETECT_DEVICE)(mask, kinds, &local_type, &local_code, skip_taken);
        if (dev < 0)
            return -1;
        if (dev >= FIRST_KEYBOARD_DEVICE || local_code == g_start_button) {
            if (g_log && g_join_logged < 30 && dev < FIRST_KEYBOARD_DEVICE) {
                fprintf(g_log, "join accepted: gamepad %d Start (button code %d)\n", dev, local_code);
                fflush(g_log);
                g_join_logged++;
            }
            if (type)
                *type = local_type;
            if (code)
                *code = local_code;
            return dev;
        }
        if (g_log && g_join_logged < 30) {
            fprintf(g_log, "join ignored: gamepad %d pressed button code %d (Start is %d)\n", dev, local_code, g_start_button);
            fflush(g_log);
            g_join_logged++;
        }
        mask |= 1 << dev;
    }
    return -1;
}

#define ADDR_PAUSE_OWNER  0x00802AF8u  /* dword_802AF8: slot whose input drives the pause menu */
#define ADDR_PAUSED_1     0x0093B328u  /* dword_93B328 / dword_93B32C: game is paused by a player */
#define ADDR_PAUSED_2     0x0093B32Cu

#define ADDR_DEVICE_REFRESH_CALL 0x006DAEABu  /* call sub_6DA970 in sub_6DAEA0 */
#define ADDR_DEVICE_REFRESH      0x006DA970u  /* thiscall sub_6DA970(this, slots): re-seats devices of the players */
#define ADDR_DROP_PLAYER         0x00427CD0u  /* sub_427CD0(player, 1, 0): the pause menu's "Drop Out" */

typedef void (__attribute__((thiscall)) *DeviceRefreshFn)(void *self, int slots);
static int g_p2_drop_pending;

static int PlayerActive(int slot)
{
    int p = *(int *)(ADDR_PLAYER_OBJ + 4 * slot);

    return p && *(char *)(p + PLAYER_ACTIVE_OFS) < 0;
}

/*
 * When a gamepad disappears the game re-seats the devices of the players and hands the
 * orphaned player a keyboard layout, so a Remote Play guest who leaves turns into a keyboard
 * player 2. Player 2 should drop out instead: keep slot 1 unassigned and drop the player
 * the way the pause menu's "Drop Out" does.
 */
static void __attribute__((thiscall)) DeviceRefreshHook(void *self, int slots)
{
    int set_before = *(int *)(ADDR_SLOT_ASSIGNED + 12);
    int dev_before = *(int *)(ADDR_SLOT_DEVICE + 12);

    ((DeviceRefreshFn)ADDR_DEVICE_REFRESH)(self, slots);

    if (*(int *)(ADDR_SLOT_ASSIGNED + 12) && (!set_before ||
        (dev_before >= 0 && dev_before < FIRST_KEYBOARD_DEVICE && !DeviceConnected(dev_before) &&
         *(int *)(ADDR_SLOT_DEVICE + 12) >= FIRST_KEYBOARD_DEVICE))) {
        ((AssignDeviceFn)ADDR_ASSIGN_DEVICE)(1, -1);
        if (set_before)
            g_p2_drop_pending = 1;
        Log("device refresh: player 2 not given a keyboard");
    }
}

/*
 * The pause menu reads its input from the slot that paused. When player 2 drops out (gamepad
 * lost) the game parks player 2 on a keyboard layout nobody uses and leaves them as the pause
 * owner, so the menu can no longer be driven by player 1. Release player 2's slot once their
 * pad is gone and hand the pause menu to player 1.
 */
static int g_p2_pad = -1;

static void HandleLostPlayer2Pad(void)
{
    int slot1_set = *(int *)(ADDR_SLOT_ASSIGNED + 12);
    int device = *(int *)(ADDR_SLOT_DEVICE + 12);

    if (slot1_set && device >= 0 && device < FIRST_KEYBOARD_DEVICE && DeviceConnected(device)) {
        g_p2_pad = device;
    } else if (g_p2_pad >= 0 && !DeviceConnected(g_p2_pad)) {
        if (slot1_set)
            ((AssignDeviceFn)ADDR_ASSIGN_DEVICE)(1, -1);
        g_p2_pad = -1;
        g_p2_drop_pending = 1;
        Log("player 2 gamepad lost, slot 1 released");
    }

    if (g_p2_drop_pending) {
        g_p2_drop_pending = 0;
        if (PlayerActive(1)) {
            ((void (__cdecl *)(int, int, int, int))ADDR_DROP_PLAYER)(1, 1, 0, 0);
            Log("player 2 dropped out");
        }
    }

    if ((*(int *)ADDR_PAUSED_1 || *(int *)ADDR_PAUSED_2) && *(int *)ADDR_PAUSE_OWNER == 1 &&
        !*(int *)(ADDR_SLOT_ASSIGNED + 12) && *(int *)ADDR_SLOT_ASSIGNED) {
        *(int *)ADDR_PAUSE_OWNER = 0;
        Log("pause menu handed to player 1");
    }
}

static unsigned g_prev_pressed;

static void HotSwitchInput(void)
{
    int raw, device, ctx;
    int slot0_dev = *(int *)ADDR_SLOT_DEVICE;
    int slot1_dev = *(int *)(ADDR_SLOT_DEVICE + 12);
    int slot0_set = *(int *)ADDR_SLOT_ASSIGNED;
    int slot1_set = *(int *)(ADDR_SLOT_ASSIGNED + 12);
    int player2 = *(int *)(ADDR_PLAYER_OBJ + 4);
    unsigned mask = 0, pressed = 0, fresh;

    TraceInputState();
    HandleLostPlayer2Pad();
    HandleDisconnectedPlayer2();
    if (HandleDroppedPlayer1())
        return;                                  /* let the game's join code take the next press */

    if (slot0_set && !DeviceConnected(slot0_dev))
        slot0_set = 0;                           /* player 1's gamepad is gone: any live device may take over */
    if (slot0_set)
        mask |= DeviceMask(slot0_dev);           /* only look at other devices */
    if (player2 && *(char *)(player2 + PLAYER_ACTIVE_OFS) < 0 && slot1_set)
        mask |= DeviceMask(slot1_dev);           /* player 2 is playing: leave their device alone */

    /*
     * Collect every raw device with a pressed input, then only react to the ones that were
     * not pressed on the previous frame. A key that stays down (a stuck key after a Remote
     * Play guest drops, a held key on another keyboard layout) would otherwise take player 1
     * back from the gamepad every frame.
     */
    ctx = ((InputSaveFn)ADDR_INPUT_SAVE)();
    ((InputSelectFn)ADDR_INPUT_SELECT)(0);
    for (raw = 0; raw < DEVICE_COUNT; raw++) {
        int found = ((DetectDeviceFn)ADDR_DETECT_DEVICE)((int)pressed, DETECT_KINDS, NULL, NULL, 0);

        if (found < 0)
            break;
        pressed |= 1u << found;
    }
    ((InputSelectFn)ADDR_INPUT_SELECT)(ctx);
    fresh = pressed & ~g_prev_pressed & ~mask;
    g_prev_pressed = pressed;
    if (!fresh)
        return;
    for (raw = 0; raw < DEVICE_COUNT && !(fresh & (1u << raw)); raw++)
        ;

    device = ((MapDeviceFn)ADDR_MAP_DEVICE)(raw, 0);
    if (!DeviceConnected(device))
        return;                                  /* stale input from a pad that is no longer there */
    if (g_log && g_detect_logged < 40) {
        fprintf(g_log, "input detected: raw %d device %d (slot 0 uses %d, slot 1 uses %d)\n",
                raw, device, slot0_dev, slot1_dev);
        fflush(g_log);
        g_detect_logged++;
    }

    if (slot0_set && slot0_dev < FIRST_KEYBOARD_DEVICE && device < FIRST_KEYBOARD_DEVICE) {
        /*
         * Player 1 is on a gamepad and another gamepad was pressed: that is a second
         * player. The game's own join code (sub_427EC0) picks up any pressed device
         * that is not assigned to a slot yet, assigns it and joins player 2, so
         * assigning it here would hide it from that check. Leave it alone.
         */
        return;
    }

    if (*(int *)ADDR_SLOT_ASSIGNED && slot0_dev == device)
        return;
    AssignDevice(0, device);
    Log("player 1 input device switched");
}

#define MAX_CLOCKS 64
static struct { void *clock; float acc; } g_clocks[MAX_CLOCKS];

static float *ClockAccumulator(void *clock)
{
    int i;

    for (i = 0; i < MAX_CLOCKS && g_clocks[i].clock; i++)
        if (g_clocks[i].clock == clock)
            return &g_clocks[i].acc;
    if (i == MAX_CLOCKS)
        return NULL;
    g_clocks[i].clock = clock;
    return &g_clocks[i].acc;
}

/* sub_548FE0 replacement; same behaviour except the integer counter at +12. */
static int __cdecl ClockUpdate(float *clock)
{
    float dt = *(float *)ADDR_FRAME_DT;
    int *counter = (int *)(clock + 3);
    float *acc;
    int n;

    clock[1] = clock[0];
    clock[0] += dt;
    clock[2] += dt;
    while (clock[2] >= 1.0f)
        clock[2] -= 1.0f;

    if (*counter == -1)
        return -1;
    if (!g_fix_counters) {
        (*counter)++;
        return *counter;
    }
    acc = ClockAccumulator(clock);
    if (!acc) {
        (*counter)++;
        return *counter;
    }
    *acc += dt * COUNTER_HZ;
    n = (int)(*acc + 0.05f);        /* tolerance so exactly 60 fps never skips or doubles a tick */
    if (n < 0)
        n = 0;
    *acc -= (float)n;
    *counter += n;
    return *counter;
}

static void LogFps(void)
{
    static LARGE_INTEGER freq, last, start;
    static int frames, lines;
    static double worst;
    LARGE_INTEGER now;
    double dt, span;

    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        last = start = now;
        return;
    }
    dt = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
    last = now;
    frames++;
    if (dt > worst)
        worst = dt;
    span = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
    if (span >= 1.0 && lines < 120) {
        if (g_log) {
            fprintf(g_log, "fps %.1f, worst frame %.1f ms\n", frames / span, worst);
            fflush(g_log);
        }
        lines++;
        frames = 0;
        worst = 0;
        start = now;
    }
}

typedef void (__cdecl *PhysFn)(int player, int *a2, float a3, float a4, float *a5);
static PhysFn g_phys_orig;
static float g_dt_phys;
static float g_phys_acc;

/* Called once per frame: how much 60 Hz physics time passed. */
static void UpdatePhysicsStep(void)
{
    float dt = *(float *)ADDR_FRAME_DT;
    int ticks;

    if (!g_fix_physics) {
        g_dt_phys = dt;                          /* the patched code then sees the real frame time */
        g_phys_acc = 0.0f;
        return;
    }
    g_phys_acc += dt;
    ticks = (int)(g_phys_acc * 60.0f + 0.05f);   /* tolerance: exactly 60 fps is one tick every frame */
    if (ticks > 0) {
        g_dt_phys = (float)ticks / 60.0f;
        g_phys_acc -= g_dt_phys;
        if (g_phys_acc < 0.0f)
            g_phys_acc = 0.0f;
    } else {
        g_dt_phys = 0.0f;
    }
}

static void __cdecl PhysWrapper(int player, int *a2, float a3, float a4, float *a5)
{
    float saved = *(float *)ADDR_FRAME_DT;

    *(float *)ADDR_FRAME_DT = g_dt_phys;
    g_phys_orig(player, a2, a3, a4, a5);
    *(float *)ADDR_FRAME_DT = saved;
}

/*
 * The main menu and pause menu entries are added to the menu table by the game's own init
 * code, after the exe has started, so the "Tweaks" row is hooked in from the frame hook once
 * they show up.
 */
static int g_front_menus_done;

static void InstallFrontMenus(void)
{
    uint32_t fn[2] = { (uint32_t)(uintptr_t)MainMenuUpdate, (uint32_t)(uintptr_t)PauseMenuUpdate };

    if (g_front_menus_done || *(uint32_t *)ADDR_MAIN_UPDATE != MAIN_UPDATE_ORIG ||
        *(uint32_t *)ADDR_PAUSE_UPDATE != PAUSE_UPDATE_ORIG)
        return;
    g_front_menus_done = 1;
    Log(WritePatch(ADDR_MAIN_UPDATE, &fn[0], 4) && WritePatch(ADDR_PAUSE_UPDATE, &fn[1], 4) ?
        "tweaks row added to main and pause menu" : "tweaks row FAILED");
}

/*
 * Pressing Start during a cutscene skips it: every playing cutscene animation (dword_951C3C[],
 * count dword_951C80) is moved to its last moment, so the game's own end-of-cutscene handling
 * (fade, returning control, loading the next level) runs on the next update.
 */
#define ADDR_START_MASK      0x00802274u  /* dword_802274: the Start button bit in a slot's button mask */
#define ADDR_CUTSCENE_ACTIVE 0x00951BE0u  /* dword_951BE0: a cutscene is playing */
#define ADDR_CUTSCENE_ANIMS  0x00951C3Cu  /* dword_951C3C[4]: playing cutscene animations */
#define ADDR_CUTSCENE_COUNT  0x00951C80u
#define ADDR_CUTSCENE_INSTANCE 0x00951BE8u  /* dword_951BE8: the running cutscene instance, +4 = its animation */
#define ANIM_FLAGS_OFS       136
#define ANIM_FINISHED        0x1000u

static DWORD g_skip_tick;

#define GAME_CUTSCENE_LIST_OFS 10928  /* game + 10928: { instance array, count } of the level's cutscene instances */

/* Is Start held on any slot, or on a gamepad the device scan reports with the Start button? */
static int StartPressed(void)
{
    int start = 0, i, ctx, dev, type, code;

    for (i = 0; i < 2; i++)
        start |= (*(int *)(ADDR_SLOT_INPUT + 96 * i + 4) & *(int *)ADDR_START_MASK) != 0;
    if (start)
        return 1;
    ctx = ((InputSaveFn)ADDR_INPUT_SAVE)();
    ((InputSelectFn)ADDR_INPUT_SELECT)(0);
    dev = ((DetectDeviceFn)ADDR_DETECT_DEVICE)(0, DETECT_KINDS, &type, &code, 0);
    ((InputSelectFn)ADDR_INPUT_SELECT)(ctx);
    return dev >= 0 && dev < FIRST_KEYBOARD_DEVICE && code == g_start_button;
}

/* Mark every playing cutscene animation of the level as finished. */
static int FinishCutsceneAnims(int game)
{
    int **list = game ? *(int ***)(game + GAME_CUTSCENE_LIST_OFS) : NULL;
    int n, i, marked = 0;
    uint8_t *anim;

    if (list && list[0] && (int)list[1] > 0 && (int)list[1] < 64) {
        n = (int)list[1];
        for (i = 0; i < n; i++) {
            uint8_t *inst = (uint8_t *)list[0][i];

            anim = inst ? *(uint8_t **)(inst + 4) : NULL;
            if (anim && (*(uint32_t *)(anim + ANIM_FLAGS_OFS) & 2)) {
                *(uint32_t *)(anim + ANIM_FLAGS_OFS) |= ANIM_FINISHED;
                marked++;
            }
        }
    }
    {
        uint8_t *inst = *(uint8_t **)ADDR_CUTSCENE_INSTANCE;

        anim = inst ? *(uint8_t **)(inst + 4) : NULL;
        if (anim) {
            *(uint32_t *)(anim + ANIM_FLAGS_OFS) |= ANIM_FINISHED;
            marked++;
        }
    }
    return marked;
}

static void LogCutsceneState(const char *tag)
{
    uint8_t *inst = *(uint8_t **)ADDR_CUTSCENE_INSTANCE;
    uint8_t *anim = inst ? *(uint8_t **)(inst + 4) : NULL;

    char buf[300];

    snprintf(buf, sizeof buf, "cutscene %s: inst %p flags %08x next %d str '%.20s' | anim flags %08x time %.2f step %.3f | hold %d snd %d count %d",
        tag, inst, inst ? *(uint32_t *)(inst + 80) : 0, inst ? *(short *)(inst + 236) : -9,
        inst ? (char *)(inst + 244) : "",
        anim ? *(uint32_t *)(anim + ANIM_FLAGS_OFS) : 0, anim ? *(float *)(anim + 144) : 0.f,
        anim ? *(float *)(anim + 152) : 0.f, *(int *)0x951C58, *(int *)0x803F78,
        *(int *)ADDR_CUTSCENE_COUNT);
    Log(buf);
}

static int g_skip_active;   /* a skip is running: FrameHook steps the game until the cutscene is over */
#define SKIP_IDLE_STEPS 4    /* steps without a cutscene before the skip is over */

/*
 * Start skips the cutscene that is playing, and the parts that follow it straight away (the game
 * is stepped with a black screen until the next part starts), so one press goes to gameplay.
 */
#define SKIP_FOLLOW_MS       3000   /* after a skip, parts starting within this time are skipped too */
#define SKIP_QUIET_MS        200    /* nothing pending and no wipe for this long: no further part follows */
#define ADDR_WIPE_PROGRESS   0x00950780u  /* flt_950780: screen wipe, 1 = covered, falls to 0 while a scene is wiped in */
#define ADDR_WIPE_SPEED      0x00950784u
#define ADDR_LEVEL_REQUEST   0x0087658Cu
#define ADDR_NEXT_LEVEL      0x00951B80u
static DWORD g_skip_arm_until;

static void CutsceneSkip(int cur, int titles)
{
    static int prev_start;
    DWORD now = GetTickCount();
    int start, edge, active;

    g_cutscene_skipping = 0;
    if (!g_skip_cutscenes || !cur || cur == titles) {
        prev_start = g_skip_active = 0;
        g_skip_arm_until = 0;
        return;
    }
    start = StartPressed();
    edge = start && !prev_start;
    prev_start = start;
    active = *(int *)ADDR_CUTSCENE_ACTIVE;
    if (g_skip_arm_until && !g_skip_active && !active) {
        /*
         * Between two cutscene parts (or after the last one): the game wipes the next scene in
         * from black; that is skipped, and once nothing is pending or fading the wait is over.
         */
        static DWORD quiet_since;
        float *wipe = (float *)ADDR_WIPE_PROGRESS;
        int pending = *(int *)ADDR_LEVEL_REQUEST || *(int *)ADDR_NEXT_LEVEL;

        if (!pending && *wipe > 0.0f && *(float *)ADDR_WIPE_SPEED < 0.0f)
            *wipe = 0.0f;
        if (now >= g_skip_arm_until) {
            g_skip_arm_until = 0;
        } else if (!pending && *wipe <= 0.0f) {
            if (!quiet_since)
                quiet_since = now;
            if (now - quiet_since > SKIP_QUIET_MS) {
                g_skip_arm_until = 0;
                quiet_since = 0;
            }
        } else {
            quiet_since = 0;
        }
    }
    if (edge && !g_skip_active) {
        if (active) {
            g_skip_tick = g_skip_first_tick = now;
            g_skip_active = 1;
            LogCutsceneState("start");
            Log("cutscene skipped");
        }
    }
    if (active && !g_skip_active && now < g_skip_arm_until) {
        char buf[80];

        snprintf(buf, sizeof buf, "cutscene skip: part skipped %lu ms after the last one", now - g_skip_done_tick);
        Log(buf);
        g_skip_tick = now;
        g_skip_active = 1;
    }
    g_cutscene_skipping = g_skip_active;
}

/*
 * Runs the skip: the game is stepped back to back inside this one frame (no frames are shown)
 * with every playing cutscene animation marked finished, so each shot hands over to the next
 * until the cutscene's own ending has run. It stops the moment the game is out of cutscene mode
 * for a few steps, so gameplay never runs sped up.
 */
static void RunCutsceneSkip(int game, int *out)
{
    float *dt = (float *)ADDR_FRAME_DT, saved = *dt;
    DWORD t0 = GetTickCount();
    int k, idle = 0;

    for (k = 0; k < 96 && GetTickCount() - t0 < 300; k++) {
        if (*(int *)ADDR_CUTSCENE_ACTIVE) {
            uint8_t *inst = *(uint8_t **)ADDR_CUTSCENE_INSTANCE;

            idle = 0;
            (void)inst;
            FinishCutsceneAnims(game);
            *dt = 0.1f;
        } else {
            *dt = saved;
            idle++;
        }
        ((FrameFn)ADDR_FRAME_FUNC)(game, out);
        if (idle >= SKIP_IDLE_STEPS) {
            g_skip_active = 0;
            g_skip_done_tick = GetTickCount();
            g_skip_arm_until = g_skip_done_tick + SKIP_FOLLOW_MS;
            {
                char buf[64];

                snprintf(buf, sizeof buf, "cutscene skip done (after %d steps)", k);
                Log(buf);
            }
            break;
        }
    }
    *dt = saved;
    if (g_skip_active && GetTickCount() - g_skip_tick > 3000) {
        g_skip_active = 0;
        LogCutsceneState("gave up");
        Log("cutscene skip gave up");
    }
    g_cutscene_skipping = g_skip_active || GetTickCount() < g_skip_arm_until;
}

static void __cdecl FrameHook(int game, int *out)
{
    int cur = game ? *(int *)(game + GAME_CUR_LEVEL_OFS) : 0;
    int titles = *(int *)ADDR_TITLES_LEVEL;

    if (g_skip_title && cur && cur == titles) {
        /* menu stack holds only the "press any button" screen: press it for the player */
        if (*(int *)ADDR_MENU_DEPTH == 1 && ++g_title_frames >= TITLE_SETTLE_FRAMES) {
            ((OpenMenuFn)ADDR_OPEN_MENU)(MENU_MAIN, *(int *)ADDR_MENU_FLAG != 0, -1);
            *(char *)ADDR_PLAYER_ACTIVE = 1; /* options only draw once player 1 counts as having pressed a button */
            if (*(float *)ADDR_TITLE_TIME < TITLE_ANIM_END)
                *(float *)ADDR_TITLE_TIME = TITLE_ANIM_END; /* no logo animation / option fade-in wait */
            g_title_frames = 0;
            Log("press-any-button screen reached, opened main menu");
        }
    } else {
        g_title_frames = 0;
    }

    InstallFrontMenus();
    UpdatePromptStyle();
    if (g_trace_passes) {
        static int frames;

        if (frames++ % 20 == 0) {
            char path[MAX_PATH + 8];
            FILE *f;

            snprintf(path, sizeof path, "%s.skip", g_ini_path);
            f = fopen(path, "r");
            g_skip_scaled = -1;
            if (f) {
                if (fscanf(f, "%d", &g_skip_scaled) != 1)
                    g_skip_scaled = -1;
                fclose(f);
            }
        }
    }
    UpdatePhysicsStep();
    g_zero_byte = g_unlock_fsaa ? 0 : *(uint8_t *)ADDR_USE_HIRES;
    if (g_hot_switch && game)
        HotSwitchInput();
    if (g_log_fps)
        LogFps();
    SyncModeTable();
    InstallRenderScaleHooks();
    if (g_trace_rt) {
        static int frames;
        if (cur && cur != titles && ++frames == 400)
            g_trace_budget = 6000;
    }
    if (g_dump_textures) {
        static int frames;

        if (cur && cur != titles && ++frames == 600)
            DumpTextures();
    }

    CutsceneSkip(cur, titles);
    if (g_skip_active) {
        RunCutsceneSkip(game, out);
    } else if (g_skip_arm_until && GetTickCount() < g_skip_arm_until && !*(int *)ADDR_CUTSCENE_ACTIVE) {
        /* the next part of the cutscene is loading: it must not be shown */
        ((FrameFn)ADDR_FRAME_FUNC)(game, out);
        g_cutscene_skipping = 1;
    } else {
        ((FrameFn)ADDR_FRAME_FUNC)(game, out);
    }
}

static int Matches(uintptr_t addr, const void *bytes, size_t len)
{
    return memcmp((const void *)addr, bytes, len) == 0;
}

/*
 * The join prompts ("Press F1 / F2 or any button to start") come from the game's text data and
 * live in heap memory. While gamepads need Start to join they are rewritten in place (same
 * length or shorter) and restored when the option is turned off.
 */
#define JOIN_TEXT_COUNT 2
static const char *k_join_orig[JOIN_TEXT_COUNT] = {
    "Press F1 or any button to start",
    "Press F2 or any button to start",
};
static const char *k_join_start[JOIN_TEXT_COUNT] = {
    "Press F1 or START to start",
    "Press F2 or START to start",
};
#define JOIN_TEXT_MAX 8
static char *g_join_addr[JOIN_TEXT_MAX];
static int g_join_kind[JOIN_TEXT_MAX];
static int g_join_found;

static void ScanJoinText(void)
{
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000, self = (uintptr_t)GetModuleHandleA("SagaTweaks.asi");
    size_t self_size = 0;
    int k;

    if (self) {
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(self + ((IMAGE_DOS_HEADER *)self)->e_lfanew);

        self_size = nt->OptionalHeader.SizeOfImage;
    }
    g_join_found = 0;
    while (addr < 0x7FFF0000u && VirtualQuery((void *)addr, &mbi, sizeof mbi)) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress;
        size_t size = mbi.RegionSize;

        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE)) &&
            !(base >= self && base < self + self_size) && size > 64) {
            static char chunk[0x10000 + 64];
            size_t off = 0, i;
            SIZE_T got;

            while (off < size) {
                size_t want = size - off > 0x10000 ? 0x10000 : size - off;

                if (!ReadProcessMemory(GetCurrentProcess(), (void *)(base + off), chunk, want, &got) || got < 40)
                    break;
                for (i = 0; i + 40 < got; i++) {
                    if (chunk[i] != 'P' || chunk[i + 1] != 'r' || chunk[i + 5] != ' ')
                        continue;
                    for (k = 0; k < JOIN_TEXT_COUNT; k++) {
                        if ((!memcmp(chunk + i, k_join_orig[k], strlen(k_join_orig[k]) + 1) ||
                             !memcmp(chunk + i, k_join_start[k], strlen(k_join_start[k]) + 1)) &&
                            g_join_found < JOIN_TEXT_MAX) {
                            char *at = (char *)(base + off + i);
                            int d, dup = 0;

                            for (d = 0; d < g_join_found; d++)
                                dup |= g_join_addr[d] == at;
                            if (!dup) {
                                g_join_addr[g_join_found] = at;
                                g_join_kind[g_join_found++] = k;
                            }
                        }
                    }
                }
                off += got > 64 ? got - 64 : got;     /* overlap so a string on a boundary is not missed */
            }
        }
        addr = base + size;
    }
}

static void ApplyJoinText(void)
{
    int i;

    for (i = 0; i < g_join_found; i++) {
        const char *want = g_join_start_text ? k_join_start[g_join_kind[i]] : k_join_orig[g_join_kind[i]];
        size_t n = strlen(want) + 1;

        char now[40];
        SIZE_T got;

        if (ReadProcessMemory(GetCurrentProcess(), g_join_addr[i], now, n, &got) && got == n && memcmp(now, want, n))
            WriteProcessMemory(GetCurrentProcess(), g_join_addr[i], want, n, &got);
    }
}

static DWORD WINAPI JoinTextThread(LPVOID param)
{
    int tries = 0;

    (void)param;
    for (;;) {
        Sleep(g_join_found ? 1000 : 3000);
        if (!g_join_found && tries++ < 40) {
            ScanJoinText();
            if (g_join_found && g_log) {
                fprintf(g_log, "join prompt text found (%d copies)\n", g_join_found);
                fflush(g_log);
            }
        }
        g_join_start_text = g_join_start;
        if (g_join_found)
            ApplyJoinText();
    }
    return 0;
}

static DWORD WINAPI EarlyHookThread(LPVOID param)
{
    (void)param;
    while (!g_hooked_vtable) {
        InstallRenderScaleHooks();
        Sleep(1);
    }
    return 0;
}

static DWORD WINAPI PatchThread(LPVOID param)
{
    int tries;
    uint8_t jmp[6];
    uint8_t call[5];
    int32_t rel;
    (void)param;

    /* wait for SteamStub to decrypt .text */
    for (tries = 0; tries < 120000; tries++) {
        if (Matches(ADDR_LOGO_BRANCH, k_logo_branch_orig, sizeof k_logo_branch_orig) &&
            Matches(ADDR_FRAME_CALL, k_frame_call_orig, sizeof k_frame_call_orig) &&
            Matches(ADDR_LIMIT_CMP1, k_limit_cmp_orig, sizeof k_limit_cmp_orig) &&
            Matches(ADDR_LIMIT_LOAD1, k_limit_load_orig, sizeof k_limit_load_orig) &&
            Matches(ADDR_LIMIT_CMP2, k_limit_cmp_orig, sizeof k_limit_cmp_orig) &&
            Matches(ADDR_LIMIT_LOAD2, k_limit_load_orig, sizeof k_limit_load_orig) &&
            Matches(ADDR_REFRESH_LOAD, k_refresh_orig, sizeof k_refresh_orig) &&
            Matches(ADDR_CLOCK_UPDATE, k_clock_orig, sizeof k_clock_orig) &&
            Matches(ADDR_FSAA_LOCK_UI, k_hires_addr_orig, sizeof k_hires_addr_orig) &&
            Matches(ADDR_FSAA_LOCK_INPUT, k_hires_addr_orig, sizeof k_hires_addr_orig) &&
            Matches(ADDR_FSAA_LOCK_CONFIG, k_hires_addr_orig, sizeof k_hires_addr_orig) &&
            Matches(ADDR_FSAA_DOWNGRADE, (const uint8_t[]){ 0x74, 0x39 }, 2) &&
            Matches(ADDR_FSAA_MENU_1, k_fsaa_cmp_orig, sizeof k_fsaa_cmp_orig) &&
            Matches(ADDR_FSAA_MENU_2, k_fsaa_cmp_orig, sizeof k_fsaa_cmp_orig) &&
            Matches(ADDR_FSAA_MENU_3, k_fsaa_cmp_orig, sizeof k_fsaa_cmp_orig) &&
            Matches(ADDR_MODE_LOOKUP, k_modelookup_orig, sizeof k_modelookup_orig) &&
            Matches(ADDR_RT_H_1, k_rt_h_orig, sizeof k_rt_h_orig) &&
            Matches(ADDR_RT_W_1, k_rt_w_orig, sizeof k_rt_w_orig) &&
            Matches(ADDR_RT_H_2, k_rt_h_orig, sizeof k_rt_h_orig) &&
            Matches(ADDR_RT_W_2, k_rt_w_orig, sizeof k_rt_w_orig) &&
            Matches(ADDR_RT_H_3, k_rt_h_orig, sizeof k_rt_h_orig) &&
            Matches(ADDR_RT_W_3, k_rt_w_orig, sizeof k_rt_w_orig) &&
            Matches(ADDR_TWEAKS_DRAW, (const uint8_t[]){ 0x80, 0x94, 0x50, 0x00 }, 4) &&
            Matches(ADDR_TWEAKS_UPDATE, (const uint8_t[]){ 0x90, 0x94, 0x50, 0x00 }, 4) &&
            Matches(ADDR_LABEL_BUILDER, (const uint8_t[]){ 0x83, 0xEC, 0x2C, 0xA1, 0x70, 0x95, 0x82, 0x00 }, 8) &&
            Matches(ADDR_FX_FSAA_ROW_CALL, (const uint8_t[]){ 0xE8 }, 1) &&
            Matches(ADDR_FX_UPDATE, (const uint8_t[]){ 0xD0, 0xF0, 0x50, 0x00 }, 4) &&
            Matches(ADDR_MAIN_QUIT_CALL, (const uint8_t[]){ 0xE8 }, 1) &&
            Matches(ADDR_PAUSE_QUIT_CALL_1, (const uint8_t[]){ 0xE8 }, 1) &&
            Matches(ADDR_PAUSE_QUIT_CALL_2, (const uint8_t[]){ 0xE8 }, 1) &&
            Matches(ADDR_PAUSE_QUIT_CALL_3, (const uint8_t[]){ 0xE8 }, 1) &&
            Matches(ADDR_JOIN_DETECT_CALL, (const uint8_t[]){ 0xE8 }, 1) &&
            Matches(ADDR_DEVICE_REFRESH_CALL, (const uint8_t[]){ 0xE8 }, 1) &&
            Matches(ADDR_VID_ROW_INSERT, (const uint8_t[]){ 0xB9, 0xC8, 0x65, 0x97, 0x02 }, 5) &&
            Matches(ADDR_VID_MENU_UPDATE, (const uint8_t[]){ 0x80, 0xEF, 0x50, 0x00 }, 4) &&
            Matches(ADDR_PHYS_VERTICAL, k_phys_orig, sizeof k_phys_orig) &&
            Matches(ADDR_POS_VY_1, k_posvy_orig, sizeof k_posvy_orig) &&
            Matches(ADDR_POS_VY_2, k_posvy_orig, sizeof k_posvy_orig) &&
            Matches(ADDR_POS_VY_3, k_posvy_orig, sizeof k_posvy_orig) &&
            Matches(ADDR_POS_VY_4, k_posvy_orig, sizeof k_posvy_orig))
            break;
        Sleep(1);
    }
    if (tries == 120000) {
        Log("expected code not found, not patching (different exe version?)");
        return 0;
    }

    if (g_skip_logos) {
    /* jz rel32 -> jmp rel32 + nop */
    rel = (int32_t)(ADDR_LOGO_SKIP_TGT - (ADDR_LOGO_BRANCH + 5));
    jmp[0] = 0xE9;
    memcpy(jmp + 1, &rel, 4);
    jmp[5] = 0x90;
    if (WritePatch(ADDR_LOGO_BRANCH, jmp, sizeof jmp))
        Log("logo skip patched");
    else
        Log("logo skip patch FAILED");
    }

    {
        uint32_t d = (uint32_t)(uintptr_t)&g_limit_d;
        uint32_t f = (uint32_t)(uintptr_t)&g_limit_f;

        ApplyMaxFps();
        if (WritePatch(ADDR_LIMIT_CMP1 + 2, &d, 4) && WritePatch(ADDR_LIMIT_CMP2 + 2, &d, 4) &&
            WritePatch(ADDR_LIMIT_LOAD1 + 2, &f, 4) && WritePatch(ADDR_LIMIT_LOAD2 + 2, &f, 4))
            Log(g_max_fps > 0 ? "frame cap patched" : "frame cap removed");
        else
            Log("frame cap patch FAILED");
    }

    {
        uint8_t patch[9];

        rel = (int32_t)((uintptr_t)RefreshStub - (ADDR_REFRESH_LOAD + 5));
        patch[0] = 0xE9;
        memcpy(patch + 1, &rel, 4);
        memset(patch + 5, 0x90, 4);
        if (WritePatch(ADDR_REFRESH_LOAD, patch, sizeof patch))
            Log("refresh rate patched");
        else
            Log("refresh rate patch FAILED");
    }

    {
        uint32_t z = (uint32_t)(uintptr_t)&g_zero_byte;

        Log(WritePatch(ADDR_FSAA_LOCK_UI, &z, 4) && WritePatch(ADDR_FSAA_LOCK_INPUT, &z, 4) &&
            WritePatch(ADDR_FSAA_LOCK_CONFIG, &z, 4) ? "FSAA unlocked" : "FSAA unlock FAILED");
    }

    {
        uint8_t call[5];
        int ok;

        call[0] = 0xE8;
        rel = (int32_t)((uintptr_t)ScaledHeightStub - (ADDR_RT_H_1 + 5));
        memcpy(call + 1, &rel, 4);
        ok = WritePatch(ADDR_RT_H_1, call, 5);
        rel = (int32_t)((uintptr_t)ScaledHeightStub - (ADDR_RT_H_2 + 5));
        memcpy(call + 1, &rel, 4);
        ok = ok && WritePatch(ADDR_RT_H_2, call, 5);
        rel = (int32_t)((uintptr_t)ScaledWidthStub - (ADDR_RT_W_1 + 5));
        memcpy(call + 1, &rel, 4);
        ok = ok && WritePatch(ADDR_RT_W_1, call, 5);
        rel = (int32_t)((uintptr_t)ScaledWidthStub - (ADDR_RT_W_2 + 5));
        memcpy(call + 1, &rel, 4);
        ok = ok && WritePatch(ADDR_RT_W_2, call, 5);
        rel = (int32_t)((uintptr_t)ScaledHeightStub - (ADDR_RT_H_3 + 5));
        memcpy(call + 1, &rel, 4);
        ok = ok && WritePatch(ADDR_RT_H_3, call, 5);
        rel = (int32_t)((uintptr_t)ScaledWidthStub - (ADDR_RT_W_3 + 5));
        memcpy(call + 1, &rel, 4);
        ok = ok && WritePatch(ADDR_RT_W_3, call, 5);
        Log(ok ? "render scale patched" : "render scale patch FAILED");
    }

    {
        uint32_t update = (uint32_t)(uintptr_t)VideoMenuUpdate;
        uint8_t call[5] = { 0xE8 };

        rel = (int32_t)((uintptr_t)VideoRowStub - (ADDR_VID_ROW_INSERT + 5));
        memcpy(call + 1, &rel, 4);
        Log(WritePatch(ADDR_VID_ROW_INSERT, call, 5) && WritePatch(ADDR_VID_MENU_UPDATE, &update, 4) ?
            "render scale menu row added" : "render scale menu row FAILED");
    }

    {
        uint8_t call[5] = { 0xE8 };

        rel = (int32_t)((uintptr_t)JoinDetect - (ADDR_JOIN_DETECT_CALL + 5));
        memcpy(call + 1, &rel, 4);
        Log(WritePatch(ADDR_JOIN_DETECT_CALL, call, 5) ? "join detection hooked" : "join detection hook FAILED");
    }

    {
        uint32_t draw = (uint32_t)(uintptr_t)TweaksDraw, update = (uint32_t)(uintptr_t)TweaksUpdate;

        Log(WritePatch(ADDR_TWEAKS_DRAW, &draw, 4) && WritePatch(ADDR_TWEAKS_UPDATE, &update, 4) ?
            "tweaks page added" : "tweaks page FAILED");
    }

    {
        uint8_t call[5] = { 0xE8 };
        int ok;

        rel = (int32_t)((uintptr_t)MainQuitRow - (ADDR_MAIN_QUIT_CALL + 5));
        memcpy(call + 1, &rel, 4);
        ok = WritePatch(ADDR_MAIN_QUIT_CALL, call, 5);
        rel = (int32_t)((uintptr_t)PauseQuitRow - (ADDR_PAUSE_QUIT_CALL_1 + 5));
        memcpy(call + 1, &rel, 4);
        ok = ok && WritePatch(ADDR_PAUSE_QUIT_CALL_1, call, 5);
        rel = (int32_t)((uintptr_t)PauseQuitRow - (ADDR_PAUSE_QUIT_CALL_2 + 5));
        memcpy(call + 1, &rel, 4);
        ok = ok && WritePatch(ADDR_PAUSE_QUIT_CALL_2, call, 5);
        rel = (int32_t)((uintptr_t)PauseQuitRow - (ADDR_PAUSE_QUIT_CALL_3 + 5));
        memcpy(call + 1, &rel, 4);
        ok = ok && WritePatch(ADDR_PAUSE_QUIT_CALL_3, call, 5);
        Log(ok ? "tweaks row drawn before Quit" : "tweaks row draw hook FAILED");
    }

    {
        uint8_t call[5] = { 0xE8 };
        uint32_t update = (uint32_t)(uintptr_t)EffectsMenuUpdate;

        rel = (int32_t)((uintptr_t)HiddenRow - (ADDR_FX_FSAA_ROW_CALL + 5));
        memcpy(call + 1, &rel, 4);
        Log(WritePatch(ADDR_FX_FSAA_ROW_CALL, call, 5) && WritePatch(ADDR_FX_UPDATE, &update, 4) ?
            "FSAA row hidden" : "FSAA row hide FAILED");
    }

    CreateThread(NULL, 0, JoinTextThread, NULL, 0, NULL);
    CreateThread(NULL, 0, EarlyHookThread, NULL, 0, NULL);

    {
        uint8_t call[5] = { 0xE8 };

        rel = (int32_t)((uintptr_t)DeviceRefreshHook - (ADDR_DEVICE_REFRESH_CALL + 5));
        memcpy(call + 1, &rel, 4);
        Log(WritePatch(ADDR_DEVICE_REFRESH_CALL, call, 5) ? "device refresh hooked" : "device refresh hook FAILED");
    }

    {
        uint8_t jmp[8];

        jmp[0] = 0xE9;
        rel = (int32_t)((uintptr_t)LabelBuilderHook - (ADDR_LABEL_BUILDER + 5));
        memcpy(jmp + 1, &rel, 4);
        memset(jmp + 5, 0x90, 3);
        Log(WritePatch(ADDR_LABEL_BUILDER, jmp, 8) ? "button label hook installed" : "button label hook FAILED");
    }

    if (g_scale_count) {
        uint8_t *tramp = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        uint8_t patch[5];

        if (tramp) {
            memcpy(tramp, k_modelookup_orig, sizeof k_modelookup_orig);
            tramp[5] = 0xE9;
            rel = (int32_t)(ADDR_MODE_LOOKUP_2 - ((uintptr_t)tramp + 10));
            memcpy(tramp + 6, &rel, 4);
            g_lookup_tramp = tramp;

            patch[0] = 0xE9;
            rel = (int32_t)((uintptr_t)ModeLookupStub - (ADDR_MODE_LOOKUP + 5));
            memcpy(patch + 1, &rel, 4);
            Log(WritePatch(ADDR_MODE_LOOKUP, patch, sizeof patch) ? "render scale hook installed" : "render scale hook FAILED");
        }
    }

    if (g_fsaa_effects) {
        uint32_t z = (uint32_t)(uintptr_t)&g_zero_dword;
        uint8_t jmp_short = 0xEB;

        Log(WritePatch(ADDR_FSAA_DOWNGRADE, &jmp_short, 1) && WritePatch(ADDR_FSAA_MENU_1, &z, 4) &&
            WritePatch(ADDR_FSAA_MENU_2, &z, 4) && WritePatch(ADDR_FSAA_MENU_3, &z, 4) ?
            "shadows and bloom kept with FSAA" : "FSAA effects patch FAILED");
    }

    {
        uint8_t *tramp = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        uint32_t dtp = (uint32_t)(uintptr_t)&g_dt_phys;
        uint8_t patch[9];
        int ok;

        if (tramp) {
            memcpy(tramp, k_phys_orig, sizeof k_phys_orig);             /* displaced prologue */
            tramp[9] = 0xE9;
            rel = (int32_t)(ADDR_PHYS_RESUME - ((uintptr_t)tramp + 14));
            memcpy(tramp + 10, &rel, 4);
            g_phys_orig = (PhysFn)tramp;

            patch[0] = 0xE9;
            rel = (int32_t)((uintptr_t)PhysWrapper - (ADDR_PHYS_VERTICAL + 5));
            memcpy(patch + 1, &rel, 4);
            memset(patch + 5, 0x90, 4);
            ok = WritePatch(ADDR_PHYS_VERTICAL, patch, sizeof patch);
            ok = ok && WritePatch(ADDR_POS_VY_1, &dtp, 4) && WritePatch(ADDR_POS_VY_2, &dtp, 4) &&
                 WritePatch(ADDR_POS_VY_3, &dtp, 4) && WritePatch(ADDR_POS_VY_4, &dtp, 4);
            Log(ok ? "vertical physics fixed to 60 Hz steps" : "vertical physics patch FAILED");
        }
    }

    {
        uint8_t patch[6];

        rel = (int32_t)((uintptr_t)ClockUpdate - (ADDR_CLOCK_UPDATE + 5));
        patch[0] = 0xE9;
        memcpy(patch + 1, &rel, 4);
        patch[5] = 0x90;
        Log(WritePatch(ADDR_CLOCK_UPDATE, patch, sizeof patch) ? "frame counters patched" : "frame counters patch FAILED");
    }

    rel = (int32_t)((uintptr_t)FrameHook - (ADDR_FRAME_CALL + 5));
    call[0] = 0xE8;
    memcpy(call + 1, &rel, 4);
    if (WritePatch(ADDR_FRAME_CALL, call, sizeof call))
        Log("title skip hook installed");
    else
        Log("title skip hook FAILED");

    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH];
        char *slash;

        DisableThreadLibraryCalls(inst);
        if (GetModuleFileNameA(inst, path, MAX_PATH) && (slash = strrchr(path, '\\'))) {
            char ini[MAX_PATH];
            strcpy(slash + 1, "SagaTweaks.log");
            g_log = fopen(path, "w");
            strcpy(ini, path);
            strcpy(strrchr(ini, '\\') + 1, "SagaTweaks.ini");
            strcpy(g_ini_path, ini);
            g_skip_logos = GetPrivateProfileIntA("SagaTweaks", "SkipLogos", 1, ini);
            g_skip_title = GetPrivateProfileIntA("SagaTweaks", "SkipTitle", 1, ini);
            g_hot_switch = GetPrivateProfileIntA("SagaTweaks", "HotSwitchInput", 1, ini);
            g_join_start = GetPrivateProfileIntA("SagaTweaks", "JoinWithStart", 1, ini);
            g_start_button = GetPrivateProfileIntA("SagaTweaks", "StartButton", 7, ini);
            g_max_fps = GetPrivateProfileIntA("SagaTweaks", "MaxFPS", 0, ini);
            g_log_fps = GetPrivateProfileIntA("SagaTweaks", "LogFPS", 0, ini);
            g_refresh_override = GetPrivateProfileIntA("SagaTweaks", "RefreshRate", 0, ini);
            g_fix_counters = GetPrivateProfileIntA("SagaTweaks", "FixFrameCounters", 1, ini);
            g_trace_rt = GetPrivateProfileIntA("SagaTweaks", "TraceRT", 0, ini);
            g_dump_textures = GetPrivateProfileIntA("SagaTweaks", "DumpTextures", 0, ini);
            g_skip_pass = GetPrivateProfileIntA("SagaTweaks", "SkipPass", -1, ini);
            g_trace_passes = GetPrivateProfileIntA("SagaTweaks", "TracePasses", 0, ini);
            g_skip_scaled = GetPrivateProfileIntA("SagaTweaks", "SkipScaledPass", -1, ini);
            g_prompt_glyphs = GetPrivateProfileIntA("SagaTweaks", "PromptGlyphs", 1, ini);
            {
                int all = GetPrivateProfileIntA("SagaTweaks", "ButtonPrompts", 0, ini);

                g_prompt_style[0] = GetPrivateProfileIntA("SagaTweaks", "ButtonPromptsP1", all, ini);
                g_prompt_style[1] = GetPrivateProfileIntA("SagaTweaks", "ButtonPromptsP2", all, ini);
            }
            g_fix_physics = GetPrivateProfileIntA("SagaTweaks", "FixJumpPhysics", 1, ini);
            {
                char buf[32];

                GetPrivateProfileStringA("SagaTweaks", "RenderScale", "1.0", buf, sizeof buf, ini);
                g_render_scale = atof(buf);
                if (g_render_scale < 0.25 || g_render_scale > 4.0)
                    g_render_scale = 1.0;
            }
            {
                char list[128], *tok, *save;

                GetPrivateProfileStringA("SagaTweaks", "RenderScales", "", list, sizeof list, ini);
                for (tok = strtok_s(list, ", ", &save); tok && g_scale_count < MAX_RENDER_SCALES;
                     tok = strtok_s(NULL, ", ", &save)) {
                    double v = atof(tok);

                    if (v > 1.0 && v <= 4.0)
                        g_scales[g_scale_count++] = v;
                }
            }
            g_skip_cutscenes = GetPrivateProfileIntA("SagaTweaks", "SkipCutscenes", 1, ini);
        }
        CloseHandle(CreateThread(NULL, 0, PatchThread, NULL, 0, NULL));
    }
    return TRUE;
}
