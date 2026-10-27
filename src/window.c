/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Display: VSync and window mode (fullscreen / windowed / borderless windowed).
 */
#include "all.h"

static const uint8_t k_interval_orig[6] = { 0x89, 0x9E, 0x6C, 0x01, 0x00, 0x00 };

#define D3DPRESENT_INTERVAL_IMMEDIATE_ 0x80000000u
#define WINDOWED_STYLE (0x00CF0000u | WS_VISIBLE)   /* the style the game gives its own windowed window */
#define POPUP_STYLE    (WS_POPUP | WS_VISIBLE)
#define STARTUP_MS     500                          /* let the first device settle before the first reset (time, not frames: 30 frames are 30 ms at 1000 fps) */

int g_vsync = 1;
int g_window_mode;

const char *const g_window_mode_names[WINDOW_MODES] = { "Fullscreen", "Windowed", "Borderless" };

DWORD g_present_interval = 1;   /* read by IntervalStub */

/* Replaces "mov [esi+16Ch], ebx" (PresentationInterval = 1); flags are the ones the next jz tests. */
extern void IntervalStub(void);

__asm__(
    ".text\n"
    ".globl _IntervalStub\n"
    "_IntervalStub:\n"
    "    pushl %eax\n"
    "    movl _g_present_interval, %eax\n"
    "    movl %eax, 0x16C(%esi)\n"
    "    popl %eax\n"
    "    pushl $0x006E3A1F\n"
    "    ret\n"
);

/*
 * Render scale by backbuffer size: the present parameters of a windowed device get the video mode's
 * size times the scale, so the game itself runs at the larger size (every effect, depth buffer and HUD agrees on it)
 * and Present stretches the backbuffer to the window. sub_6E3940 copies the mode's width and height into the
 * parameters (+138h, +13Ch); the copy is replaced by a call that scales them. Flags are kept: a jz further down
 * still tests the compare before it.
 */
double g_bb_scale = 1.0;

#define ADDR_BACKBUFFER_SIZE  0x006E3AD7u
#define BACKBUFFER_SIZE_NEXT  0x006E3AE8u
#define DEVICE_WINDOWED_FLAG  0x158   /* present parameters: Windowed */
#define MAX_BACKBUFFER        8192    /* D3D9 hardware limit that is safe on every GPU of the last decade */

static const uint8_t k_bbsize_orig[17] = {
    0x8B, 0x08, 0x89, 0x8E, 0x38, 0x01, 0x00, 0x00, 0x8B, 0x50, 0x04, 0x89, 0x96, 0x3C, 0x01, 0x00, 0x00
};

/* The size before and after scaling, while a scaled backbuffer is in use (0 = not scaled). */
static int g_plain_w, g_plain_h, g_scaled_w, g_scaled_h;

void __cdecl BackbufferSize(char *dev, int *wh)
{
    double s = g_bb_scale;

    g_scaled_w = g_scaled_h = 0;
    if (s == 1.0 || !*(int *)(dev + DEVICE_WINDOWED_FLAG))
        return;
    g_plain_w = wh[0];
    g_plain_h = wh[1];
    if (wh[0] * s > MAX_BACKBUFFER)
        s = (double)MAX_BACKBUFFER / wh[0];
    if (wh[1] * s > MAX_BACKBUFFER)
        s = (double)MAX_BACKBUFFER / wh[1];
    wh[0] = (int)(wh[0] * s + 0.5) & ~1;
    wh[1] = (int)(wh[1] * s + 0.5) & ~1;
    g_scaled_w = wh[0];
    g_scaled_h = wh[1];
}

/*
 * The game treats the backbuffer size as the window's size: sub_6E1B70 looks up the video mode for it and, for a
 * windowed device, writes that size back into the mode table (so Video Settings showed the multiplied size and every
 * reset multiplied it again), and sub_6E3940 stores it as the saved resolution. These three calls get the unscaled
 * size instead.
 */
#define ADDR_MODE_FIND_CALL   0x006E3FC7u  /* call sub_6E1B70 (thiscall: mode index for a size) */
#define ADDR_MODE_FIND_FN     0x006E1B70u
#define ADDR_SAVE_W_CALL      0x006E409Au  /* call sub_6DAEF0 (thiscall: saved width) */
#define ADDR_SAVE_W_FN        0x006DAEF0u
#define ADDR_SAVE_H_CALL      0x006E40ABu  /* call sub_6DAF00 (thiscall: saved height) */
#define ADDR_SAVE_H_FN        0x006DAF00u

static int Plain(int v, int scaled, int plain)
{
    return scaled && v == scaled ? plain : v;
}

typedef int (__attribute__((thiscall)) *ModeFindFn)(void *, int, int, int);
typedef void (__attribute__((thiscall)) *SaveFn)(void *, int);

static int __attribute__((thiscall)) ModeFind(void *self, int w, int h, int hz)
{
    return ((ModeFindFn)ADDR_MODE_FIND_FN)(self, Plain(w, g_scaled_w, g_plain_w), Plain(h, g_scaled_h, g_plain_h), hz);
}

static void __attribute__((thiscall)) SaveWidth(void *self, int w)
{
    ((SaveFn)ADDR_SAVE_W_FN)(self, Plain(w, g_scaled_w, g_plain_w));
}

static void __attribute__((thiscall)) SaveHeight(void *self, int h)
{
    ((SaveFn)ADDR_SAVE_H_FN)(self, Plain(h, g_scaled_h, g_plain_h));
}

static int CallTo(uintptr_t site, uintptr_t fn)
{
    const uint8_t *b = (const uint8_t *)site;

    return b[0] == 0xE8 && site + 5 + *(const int32_t *)(b + 1) == fn;
}

extern void BackbufferSizeStub(void);

__asm__(
    ".text\n"
    ".globl _BackbufferSizeStub\n"
    "_BackbufferSizeStub:\n"
    "    pushfl\n"
    "    movl (%eax),%ecx\n"
    "    movl 4(%eax),%edx\n"
    "    pushl %eax\n"
    "    pushl %edx\n"
    "    pushl %ecx\n"
    "    pushl %esp\n"
    "    pushl %esi\n"
    "    call _BackbufferSize\n"
    "    addl $8,%esp\n"
    "    popl %ecx\n"
    "    popl %edx\n"
    "    popl %eax\n"
    "    movl %ecx,0x138(%esi)\n"
    "    movl %edx,0x13C(%esi)\n"
    "    popfl\n"
    "    pushl $0x006E3AE8\n"
    "    ret\n"
);

double RenderScaleValue(void)
{
    return g_bb_scale;
}

/* A scaled backbuffer needs a windowed device: fullscreen is not offered while the scale is not 100%. */
static int FullscreenAllowed(void)
{
    return g_bb_scale == 1.0;
}

/* Next window mode in the menu's direction, skipping the ones that do not work with the current scale. */
int WindowModeStep(int mode, int step)
{
    do
        mode = (mode + step + WINDOW_MODES) % WINDOW_MODES;
    while (mode == WINDOW_FULLSCREEN && !FullscreenAllowed());
    return mode;
}

/* A new render scale: the device is reset with the new backbuffer size (borderless instead of fullscreen when scaled). */
void RenderScaleSet(double v)
{
    char buf[16];

    g_bb_scale = v;
    if (!FullscreenAllowed() && g_window_mode == WINDOW_FULLSCREEN) {
        g_window_mode = WINDOW_BORDERLESS;
        snprintf(buf, sizeof buf, "%d", g_window_mode);
        WritePrivateProfileStringA("SagaTweaks", "WindowMode", buf, g_ini_path);
    }
    WindowApply();
}

/*
 * The first device is created by sub_6E42D0 (called from sub_6DF080), and its "windowed" flag starts at 0: the game
 * always opens in exclusive fullscreen, whatever the mode is set to, and the mode was only applied by a reset
 * afterwards. A borderless mode is now set up before that first creation, so the window opens that way.
 */
#define ADDR_FIRST_DEVICE_CALL 0x006DF08Fu
#define ADDR_FIRST_DEVICE_FN   0x006E42D0u

static int g_started_borderless;

static void __attribute__((thiscall)) FirstDevice(char *dev)
{
    HWND wnd = *(HWND *)ADDR_GAME_WINDOW;

    if (g_window_mode == WINDOW_BORDERLESS && wnd) {
        int *rect = (int *)ADDR_WINDOW_RECT;   /* nWidth, nHeight, X, Y */

        rect[0] = GetSystemMetrics(SM_CXSCREEN);
        rect[1] = GetSystemMetrics(SM_CYSCREEN);
        rect[2] = rect[3] = 0;
        dev[DEVICE_WINDOWED_OFS] = 1;
        SetWindowLongA(wnd, GWL_STYLE, POPUP_STYLE);
        SetWindowPos(wnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        g_started_borderless = 1;
        Log("first device created borderless");
    }
    ((void (__attribute__((thiscall)) *)(void *))ADDR_FIRST_DEVICE_FN)(dev);
}

int WindowReady(void)
{
    return Matches(ADDR_PRESENT_INTERVAL, k_interval_orig, sizeof k_interval_orig) &&
           CallTo(ADDR_FIRST_DEVICE_CALL, ADDR_FIRST_DEVICE_FN) &&
           Matches(ADDR_BACKBUFFER_SIZE, k_bbsize_orig, sizeof k_bbsize_orig) &&
           CallTo(ADDR_MODE_FIND_CALL, ADDR_MODE_FIND_FN) && CallTo(ADDR_SAVE_W_CALL, ADDR_SAVE_W_FN) &&
           CallTo(ADDR_SAVE_H_CALL, ADDR_SAVE_H_FN);
}

void WindowInstall(void)
{
    g_present_interval = g_vsync ? 1 : D3DPRESENT_INTERVAL_IMMEDIATE_;
    Log(PatchCall(ADDR_FIRST_DEVICE_CALL, FirstDevice) ? "first device hooked" : "first device hook FAILED");
    Log(PatchJmp(ADDR_PRESENT_INTERVAL, IntervalStub, 6) ? "vsync patched" : "vsync patch FAILED");
    {
        int ok = PatchJmp(ADDR_BACKBUFFER_SIZE, BackbufferSizeStub, 17);

        ok &= PatchCall(ADDR_MODE_FIND_CALL, ModeFind) && PatchCall(ADDR_SAVE_W_CALL, SaveWidth) &&
              PatchCall(ADDR_SAVE_H_CALL, SaveHeight);
        Log(ok ? "render scale by backbuffer size installed" : "render scale by backbuffer size FAILED");
    }
}

/*
 * Sizes the window for the chosen mode and asks the game to apply it with its own device
 * reset (sub_6E3940, what Accept in Video Settings calls). The reset moves the window to
 * nWidth/nHeight/X/Y when the device is windowed; X = Y = -1 centres it.
 */
void WindowApply(void)
{
    char *dev = (char *)ADDR_DEVICE_OBJECT;
    HWND wnd = *(HWND *)ADDR_GAME_WINDOW;
    int *rect = (int *)ADDR_WINDOW_RECT;   /* nWidth, nHeight, X, Y */
    int desk_w, desk_h;
    int wmode = g_window_mode;
    int windowed = wmode != WINDOW_FULLSCREEN;

    g_present_interval = g_vsync ? 1 : D3DPRESENT_INTERVAL_IMMEDIATE_;
    if (!*(void **)(dev + 4) || !wnd)
        return;
    desk_w = *(int *)(dev + DEVICE_DESKTOP_W_OFS);
    desk_h = *(int *)(dev + DEVICE_DESKTOP_H_OFS);
    if (wmode != WINDOW_FULLSCREEN && (desk_w <= 0 || desk_h <= 0))
        return;
    /*
     * The style change comes first: it sends WM_SIZE at once (on Windows; Wine defers it), and the game's handler
     * stores the old window's size in nWidth / nHeight, which would replace the size set below and give a small
     * window in the top left corner after the reset.
     */
    SetWindowLongA(wnd, GWL_STYLE, wmode == WINDOW_WINDOWED ? WINDOWED_STYLE : POPUP_STYLE);
    SetWindowPos(wnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    if (wmode == WINDOW_WINDOWED) {
        int index = *(int *)(dev + DEVICE_MODE_INDEX_OFS), count = *(int *)(dev + DEVICE_MODE_COUNT_OFS);
        int *mode = *(int **)(dev + DEVICE_MODE_TABLE_OFS);

        if (!mode || index < 0 || index >= count)
            return;
        mode += 4 * index;                     /* 16-byte entries: width, height, ... */
        if (mode[0] <= 0 || mode[1] <= 0)
            return;
        int w = mode[0], h = mode[1];
        RECT r;

        if (h > desk_h * 9 / 10) {                 /* keep the frame on the desktop */
            w = w * (desk_h * 9 / 10) / h;
            h = desk_h * 9 / 10;
        }
        if (w > desk_w - 16) {
            h = h * (desk_w - 16) / w;
            w = desk_w - 16;
        }
        r = (RECT){ 0, 0, w, h };
        AdjustWindowRect(&r, WINDOWED_STYLE, FALSE);
        rect[0] = r.right - r.left;
        rect[1] = r.bottom - r.top;
        rect[2] = rect[3] = -1;
    } else if (wmode == WINDOW_BORDERLESS) {
        rect[0] = desk_w;
        rect[1] = desk_h;
        rect[2] = rect[3] = 0;
    }
    dev[DEVICE_WINDOWED_OFS] = (char)windowed;
    ((ApplyDeviceFn)ADDR_DEVICE_RESET)(dev, 1);
    /*
     * Leaving exclusive fullscreen, D3D9 puts back the window style it saved when the game went fullscreen (on Windows;
     * Wine does not), which undoes the style set above: the first switch to borderless left a framed window. The
     * window is then set up again.
     */
    if (wmode != WINDOW_FULLSCREEN) {
        DWORD style = wmode == WINDOW_WINDOWED ? WINDOWED_STYLE : POPUP_STYLE;

        if ((DWORD)GetWindowLongA(wnd, GWL_STYLE) != style) {
            int x = wmode == WINDOW_WINDOWED ? (desk_w - rect[0]) / 2 : 0;
            int y = wmode == WINDOW_WINDOWED ? (desk_h - rect[1]) / 2 : 0;

            SetWindowLongA(wnd, GWL_STYLE, style);
            SetWindowPos(wnd, NULL, x, y, rect[0], rect[1], SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            Log("window style restored after the device reset");
        }
    }
    Log("display mode applied");
}

/* Applies a non-default ini setting once the game's first device exists. */
void WindowFrame(void)
{
    static DWORD since;
    static int done;

    if (done || !*(void **)(ADDR_DEVICE_OBJECT + 4))
        return;
    if (!since)
        since = TickNonZero();
    if (GetTickCount() - since < STARTUP_MS)
        return;
    done = 1;
    if (g_started_borderless)
        return;                                 /* already created that way */
    if (g_window_mode != WINDOW_FULLSCREEN || !g_vsync)
        WindowApply();
}
