/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Display: VSync and window mode (fullscreen / windowed / borderless windowed).
 */
#include "all.h"

static const uint8_t k_interval_orig[6] = { 0x89, 0x9E, 0x6C, 0x01, 0x00, 0x00 };

#define D3DPRESENT_INTERVAL_IMMEDIATE_ 0x80000000u
#define WINDOWED_STYLE (0x00CF0000u | WS_VISIBLE)   /* the style the game gives its own windowed window */
#define POPUP_STYLE    (WS_POPUP | WS_VISIBLE)
#define STARTUP_FRAMES 30                           /* let the first device settle before the first reset */

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

int WindowReady(void)
{
    return Matches(ADDR_PRESENT_INTERVAL, k_interval_orig, sizeof k_interval_orig);
}

void WindowInstall(void)
{
    g_present_interval = g_vsync ? 1 : D3DPRESENT_INTERVAL_IMMEDIATE_;
    Log(PatchJmp(ADDR_PRESENT_INTERVAL, IntervalStub, 6) ? "vsync patched" : "vsync patch FAILED");
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
    int desk_w = *(int *)(dev + DEVICE_DESKTOP_W_OFS), desk_h = *(int *)(dev + DEVICE_DESKTOP_H_OFS);
    int *mode = (int *)(*(char **)(dev + DEVICE_MODE_TABLE_OFS) + 16 * *(int *)(dev + DEVICE_MODE_INDEX_OFS));
    int windowed = g_window_mode != WINDOW_FULLSCREEN;

    g_present_interval = g_vsync ? 1 : D3DPRESENT_INTERVAL_IMMEDIATE_;
    if (!*(void **)(dev + 4) || !wnd)
        return;
    if (g_window_mode == WINDOW_WINDOWED) {
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
    } else if (g_window_mode == WINDOW_BORDERLESS) {
        rect[0] = desk_w;
        rect[1] = desk_h;
        rect[2] = rect[3] = 0;
    }
    SetWindowLongA(wnd, GWL_STYLE, g_window_mode == WINDOW_WINDOWED ? WINDOWED_STYLE : POPUP_STYLE);
    SetWindowPos(wnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    dev[DEVICE_WINDOWED_OFS] = (char)windowed;
    ((ApplyDeviceFn)ADDR_DEVICE_RESET)(dev, 1);
    Log("display mode applied");
}

/* Applies a non-default ini setting once the game's first device exists. */
void WindowFrame(void)
{
    static int frames;

    if (frames > STARTUP_FRAMES)
        return;
    if (!*(void **)(ADDR_DEVICE_OBJECT + 4))
        return;
    if (++frames == STARTUP_FRAMES && (g_window_mode != WINDOW_FULLSCREEN || !g_vsync))
        WindowApply();
}
