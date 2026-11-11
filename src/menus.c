/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Menus: Render Scale row in Video Settings, the Tweaks and Mods pages, hidden FSAA and Aspect rows,
 * and the Tweaks and Mods entries in the main and pause menu.
 */
#include "all.h"

/*
 * Native "Render Scale", "Window Mode" and "VSync" rows on the Video Settings page (menu id 0x401),
 * placed just above the Back / Accept row. The page's draw routine (sub_511EA0) draws its rows with sub_50F740, which
 * also counts them for the cursor range; a call is patched in right before the Back row to draw
 * three more. The rows sit at index 4-6 and push Back to index 7, so the update wrapper handles
 * the new row itself and shows the original update routine Back as row 4 again. The stock Aspect row (index 2) is not drawn
 * (HiddenRow replaces its draw call, the cursor range follows the rows drawn), so the stock row after it is index 2 and the update
 * wrapper does what the stock routine would do for it itself. Changing the
 * value resets the device with a new backbuffer size (RenderScaleSet) and stores the choice in the ini.
 */
#define VID_MENU_UPDATE_ORIG  0x0050EF80u

static const uint8_t k_video_update_orig[8] = { 0x8B, 0x44, 0x24, 0x04, 0x83, 0x78, 0x68, 0x00 };
static MenuUpdateFn g_video_update_orig;

#define ADDR_VID_ROW_INSERT   0x005120E6u  /* mov ecx, offset device: first instruction of the Back row code */

#define ADDR_MENU_ALPHA       0x00921D60u  /* dword_921D60[0]: alpha of a normal row */

#define ADDR_MENU_ROW_TEXT    0x0050F740u  /* sub_50F740(menu, text, alpha): draws and counts one row */


#define ADDR_MENU_SOUND       0x00800940u

#define ADDR_MENU_SOUND_ID    0x00800950u
#define ADDR_MENU_CANCEL_SOUND_ID 0x00800958u

#define MENU_BACK_OFS         0x68  /* dword: Back / cancel pressed (the stock pages pop the menu on it) */

#define MENU_CUR_ROW_OFS      10    /* word */

#define TOGGLE_ROW            2     /* the page's last stock row (original index 3); the Aspect row before it is hidden */

#define ORIG_TOGGLE_ROW       3

#define ORIG_BACK_ROW         4

/* Row indices: the stock Aspect row is hidden (FixAspect on) or drawn (off), which moves the rows after it. */
#define SCALE_ROW             (3 + !g_fix_aspect)

#define WINDOW_ROW            (4 + !g_fix_aspect)

#define VSYNC_ROW             (5 + !g_fix_aspect)

#define BACK_ROW              (6 + !g_fix_aspect)

#define ADDR_VID_ASPECT_ROW_CALL 0x00512033u  /* call sub_50F740 in sub_511EA0: draws the Aspect row */

#define ADDR_VID_TOGGLE       0x00921D14u     /* dword_921D14: the toggle row's value */

#define ADDR_VID_TOGGLE_OK    0x008264C9u     /* byte_8264C9: the toggle row can be changed */

#define ADDR_MENU_ERROR_SOUND_ID 0x0080095Cu  /* dword_80095C */

#define MENU_LEFT_OFS         0x5C

#define MENU_RIGHT_OFS        0x60

#define MENU_SELECT_OFS       0x64

static const double k_scale_steps[] = { 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 3.0 };

#define SCALE_STEPS ((int)(sizeof k_scale_steps / sizeof k_scale_steps[0]))

static int ScaleStepIndex(void)
{
    int i, best = 0;
    double d, bestd = 1e9;

    for (i = 0; i < SCALE_STEPS; i++) {
        d = fabs(k_scale_steps[i] - RenderScaleValue());
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

    snprintf(text, sizeof text, "Render Scale: %d%%", (int)(RenderScaleValue() * 100.0 + 0.5));
    ((MenuRowFn)ADDR_MENU_ROW_TEXT)(menu, text, *(int *)ADDR_MENU_ALPHA);
    snprintf(text, sizeof text, "Window Mode: %s", g_window_mode_names[g_window_mode]);
    ((MenuRowFn)ADDR_MENU_ROW_TEXT)(menu, text, *(int *)ADDR_MENU_ALPHA);
    snprintf(text, sizeof text, "VSync: %s", g_vsync ? "On" : "Off");
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

    snprintf(buf, sizeof buf, "%.2f", k_scale_steps[idx]);
    WritePrivateProfileStringA("SagaTweaks", "RenderScale", buf, g_ini_path);
    RenderScaleSet(k_scale_steps[idx]);
    *(int *)ADDR_MENU_SOUND = *(int *)ADDR_MENU_SOUND_ID;
    Log("render scale changed from the menu");
}

/* Window mode and VSync: store the choice and let the game reset its device. */
static void StepDisplay(int row, int step)
{
    char buf[16];

    if (row == WINDOW_ROW) {
        g_window_mode = WindowModeStep(g_window_mode, step);
        snprintf(buf, sizeof buf, "%d", g_window_mode);
        WritePrivateProfileStringA("SagaTweaks", "WindowMode", buf, g_ini_path);
    } else {
        g_vsync = !g_vsync;
        snprintf(buf, sizeof buf, "%d", g_vsync);
        WritePrivateProfileStringA("SagaTweaks", "VSync", buf, g_ini_path);
    }
    WindowApply();
    *(int *)ADDR_MENU_SOUND = *(int *)ADDR_MENU_SOUND_ID;
}

static int __cdecl VideoMenuUpdate(int menu)
{
    short *row = (short *)(menu + MENU_CUR_ROW_OFS);
    int result;

    if (*(int *)(menu + MENU_BACK_OFS))
        return g_video_update_orig(menu);

    if (g_fix_aspect && *row == TOGGLE_ROW) {                     /* the stock row after the hidden Aspect row (sub_50EF80, case 3) */
        if (*(int *)(menu + MENU_LEFT_OFS) || *(int *)(menu + MENU_RIGHT_OFS) || *(int *)(menu + MENU_SELECT_OFS)) {
            if (*(uint8_t *)ADDR_VID_TOGGLE_OK) {
                *(int *)ADDR_MENU_SOUND = *(int *)ADDR_MENU_SOUND_ID;
                *(int *)ADDR_VID_TOGGLE = 1 - *(int *)ADDR_VID_TOGGLE;
            } else {
                *(int *)ADDR_MENU_SOUND = *(int *)ADDR_MENU_ERROR_SOUND_ID;
            }
        }
        return 0;
    }
    if (*row >= SCALE_ROW && *row < BACK_ROW) {
        int step = *(int *)(menu + MENU_RIGHT_OFS) || *(int *)(menu + MENU_SELECT_OFS) ? 1 :
                   *(int *)(menu + MENU_LEFT_OFS) ? -1 : 0;

        if (step) {
            if (*row == SCALE_ROW)
                StepRenderScale(step);
            else
                StepDisplay(*row, step);
        }
        return 0;
    }
    if (*row == BACK_ROW) {
        *row = ORIG_BACK_ROW;                     /* the original routine knows Back as row 4 */
        result = g_video_update_orig(menu);
        if (*row == ORIG_BACK_ROW)
            *row = BACK_ROW;
        return result;
    }
    return g_video_update_orig(menu);
}

/*
 * "Tweaks" and "Mods" pages: they share menu table entry 0x406, a placeholder with empty
 * callbacks. The row used to open it selects which content the callbacks draw and update.
 */
#define TWEAKS_DRAW_ORIG      0x00509480u
#define TWEAKS_UPDATE_ORIG    0x00509490u

#define MENU_TWEAKS           0x406

#define MAIN_UPDATE_ORIG      0x00494E20u

static const uint8_t k_main_update_orig[6] = { 0x51, 0xA1, 0x94, 0x64, 0x82, 0x00 };
static MenuUpdateFn g_main_update_orig;

#define PAUSE_UPDATE_ORIG     0x00495900u

static const uint8_t k_pause_update_orig[5] = { 0xA1, 0x94, 0x64, 0x82, 0x00 };
static MenuUpdateFn g_pause_update_orig;

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
}
 TweakOpt;

static const char *const k_prompt_names[] = { "Auto", "Xbox", "PlayStation" };

static const int k_fps_list[] = { 0, 30, 60, 90, 120, 144, 165, 240 };

static const TweakOpt k_tweaks[] = {
    { "Skip Logos (restart)",        "SkipLogos",        &g_skip_logos,    OPT_BOOL,  1, NULL, 0, NULL, NULL, NULL },
    { "Skip Title Screen",           "SkipTitle",        &g_skip_title,    OPT_BOOL,  0, NULL, 0, NULL, NULL, NULL },
    { "Skip Cutscenes (Start)",      "SkipCutscenes",    &g_skip_cutscenes, OPT_BOOL, 0, NULL, 0, NULL, NULL, NULL },
    { "Input Hot-Switch",            "HotSwitchInput",   &g_hot_switch,    OPT_BOOL,  0, NULL, 0, NULL, NULL, NULL },
    { "Player 1 Button Prompts",     "ButtonPromptsP1",  &g_prompt_style[0], OPT_NAMED, 0, NULL, 3, NULL, NULL, k_prompt_names },
    { "Player 2 Button Prompts",     "ButtonPromptsP2",  &g_prompt_style[1], OPT_NAMED, 0, NULL, 3, NULL, NULL, k_prompt_names },
    { "Aspect Ratio Fix",            "FixAspect",        &g_fix_aspect,    OPT_BOOL,  0, NULL, 0, NULL, NULL, NULL },
    { "Ultrawide Cutscene FOV",      "CutsceneFOV",      &g_cutscene_fov,  OPT_BOOL,  0, NULL, 0, NULL, NULL, NULL },
    { "Ultrawide HUD",               "HudWide",          &g_hud_wide,      OPT_BOOL,  0, NULL, 0, NULL, NULL, NULL },
    { "FPS Limit",                   "MaxFPS",           &g_max_fps,       OPT_LIST,  0, k_fps_list, 8, "Unlimited", " FPS", NULL },
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
        snprintf(out, size, "%d%%", (int)(RenderScaleValue() * 100.0 + 0.5));
        break;
    }
}

static void TweakStore(const TweakOpt *o)
{
    char buf[16];

    if (o->type == OPT_SCALE)
        snprintf(buf, sizeof buf, "%.2f", RenderScaleValue());
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

static int g_mods_page;

#define ADDR_MENU_ROW_PITCH   0x00800994u  /* flt_800994: native Extras row pitch */
#define ADDR_SCROLL_FADE_FROM 0x007FACD0u  /* flt_7FACD0: fade start */
#define ADDR_SCROLL_FADE_TO   0x007FACD4u  /* flt_7FACD4: fully hidden */
#define ADDR_ROW_TEXT_SCALE   0x008009A8u  /* flt_8009A8 */
#define MENU_SCROLL_OFS       152
#define MENU_SCALE_OFS        144

static int ModsRowAlpha(int menu)
{
    float y = fabsf(*(float *)(menu + MENU_SCROLL_OFS));
    float from = *(float *)ADDR_SCROLL_FADE_FROM;
    float to = *(float *)ADDR_SCROLL_FADE_TO;
    float alpha = 1.0f;

    if (y > from) {
        if (y >= to)
            return 0;
        alpha = 1.0f - (y - from) / (to - from);
    }
    *(float *)ADDR_ROW_TEXT_SCALE = *(float *)(menu + MENU_SCALE_OFS);
    return (int)(*(int *)ADDR_MENU_ALPHA * alpha);
}

static int __cdecl TweaksDraw(int menu)
{
    char value[24], text[64];
    int count = g_mods_page ? ModsCount() : TWEAK_COUNT;
    int i;

    if (g_mods_page)
        *(float *)(menu + MENU_SCROLL_OFS) =
            -(float)*(short *)(menu + MENU_CUR_ROW_OFS) * *(float *)ADDR_MENU_ROW_PITCH * *(float *)(menu + MENU_SCALE_OFS);
    for (i = 0; i < count; i++) {
        if (g_mods_page) {
            snprintf(text, sizeof text, "Mod %d: %.40s", i + 1, ModsName(i));
        } else {
            TweakValueText(&k_tweaks[i], value, sizeof value);
            snprintf(text, sizeof text, "%s: %s", k_tweaks[i].label, value);
        }
        ((MenuRowFn)ADDR_MENU_ROW_TEXT)(menu, text, g_mods_page ? ModsRowAlpha(menu) : *(int *)ADDR_MENU_ALPHA);
    }
    if (!*(int *)ADDR_TEXT_READY)
        ((void (__cdecl *)(void))ADDR_TEXT_INIT)();
    ((MenuRowFn)ADDR_MENU_ROW_TEXT)(menu, *(const char **)ADDR_TEXT_BACK,
                                    g_mods_page ? ModsRowAlpha(menu) : *(int *)ADDR_MENU_ALPHA);
    return 0;
}

static int __cdecl TweaksUpdate(int menu)
{
    int row = *(short *)(menu + MENU_CUR_ROW_OFS);
    int select = *(int *)(menu + MENU_SELECT_OFS);
    int back_row = g_mods_page ? ModsCount() : TWEAK_COUNT;
    int step;

    if (*(int *)(menu + MENU_BACK_OFS)) {   /* B / Escape: leave the page like the stock options pages */
        *(int *)ADDR_MENU_SOUND = *(int *)ADDR_MENU_CANCEL_SOUND_ID;
        ((void (__cdecl *)(void))ADDR_MENU_POP)();
        return 0;
    }
    if (row == back_row) {
        if (select) {
            *(int *)ADDR_MENU_SOUND = *(int *)ADDR_MENU_SOUND_ID;
            ((void (__cdecl *)(void))ADDR_MENU_POP)();
        }
        return 0;
    }
    if (row < 0 || row > back_row)
        return 0;
    step = *(int *)(menu + MENU_RIGHT_OFS) || select ? 1 : *(int *)(menu + MENU_LEFT_OFS) ? -1 : 0;
    if (step && g_mods_page) {
        int moved = ModsMove(row, step);

        if (moved != row) {
            *(short *)(menu + MENU_CUR_ROW_OFS) = (short)moved;
            *(int *)(menu + 52) = moved;
            *(int *)ADDR_MENU_SOUND = *(int *)ADDR_MENU_SOUND_ID;
        }
    } else if (step) {
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

#define FX_UPDATE_ORIG        0x0050F0D0u

static const uint8_t k_effects_update_orig[5] = { 0x57, 0x8B, 0x7C, 0x24, 0x08 };
static MenuUpdateFn g_effects_update_orig;

#define ADDR_FSAA_DESIRED     0x02976C30u  /* dword_2976C30 */

static int __cdecl HiddenRow(int menu, int text, int alpha)
{
    (void)menu; (void)text; (void)alpha;
    return 0;
}

/* The Aspect row of Video Settings: hidden while FixAspect is on (nothing left to choose), the game's own row when it is off. */
static int __cdecl AspectRow(int menu, int text, int alpha)
{
    return g_fix_aspect ? 0 : ((MenuRowFn)ADDR_MENU_ROW_TEXT)(menu, (const char *)text, alpha);
}

static int __cdecl EffectsMenuUpdate(int menu)
{
    short *row = (short *)(menu + MENU_CUR_ROW_OFS);
    int *row_copy = (int *)(menu + 52);
    short before = *row;
    int r;

    if (*(int *)(menu + MENU_BACK_OFS))
        return g_effects_update_orig(menu);
    *row = (short)(before + 1);
    *row_copy = before + 1;
    r = g_effects_update_orig(menu);
    if (*row == before + 1) {
        *row = before;
        *row_copy = before;
    }
    return r;
}

/* Move Control Setup, Video Settings and Effects Settings out of the main menu into Options.
 * Options draws its rows with sub_574DE0; its stock rows are volume, subtitles and Back (row 2). */
#define ADDR_MAIN_CONTROL_CALL 0x00495218u
#define ADDR_MAIN_VIDEO_CALL   0x00495236u
#define ADDR_MAIN_EFFECTS_CALL 0x00495255u
#define ADDR_OPTIONS_BACK_CALL 0x004947A0u
#define OPTIONS_UPDATE_ORIG    0x00494300u
#define OPTIONS_CONTROL_ROW    2
#define OPTIONS_BACK_ROW       5
#define ORIG_OPTIONS_BACK_ROW  2
#define MAIN_ORIG_QUIT_ROW     6
#define MENU_CONTROL           21
#define MENU_VIDEO             0x401
#define MENU_EFFECTS           0x402
#define ADDR_PAUSE_CONTROL_CALL 0x00495BC0u
#define ADDR_PAUSE_VIDEO_CALL   0x00495BDDu
#define ADDR_PAUSE_EFFECTS_CALL 0x00495BFBu

static const uint8_t k_options_update_orig[6] = { 0x51, 0x68, 0x6C, 0x65, 0x87, 0x00 };
static MenuUpdateFn g_options_update_orig;

static int __cdecl OptionsBackRow(int menu, const char *text)
{
    ((PauseRowFn)ADDR_PAUSE_ROW_DRAW)(menu, "Control Setup");
    ((PauseRowFn)ADDR_PAUSE_ROW_DRAW)(menu, "Video Settings");
    ((PauseRowFn)ADDR_PAUSE_ROW_DRAW)(menu, "Effects Settings");
    return ((PauseRowFn)ADDR_PAUSE_ROW_DRAW)(menu, text);
}

static int __cdecl HiddenPauseRow(int menu, const char *text)
{
    (void)menu; (void)text;
    return 0;
}

static int __cdecl OptionsMenuUpdate(int menu)
{
    short *row = (short *)(menu + MENU_CUR_ROW_OFS);
    int *row_copy = (int *)(menu + 52);
    int r;

    if (*(int *)(menu + MENU_BACK_OFS))
        return g_options_update_orig(menu);
    if (*row >= OPTIONS_CONTROL_ROW && *row < OPTIONS_BACK_ROW) {
        if (*(int *)(menu + MENU_SELECT_OFS)) {
            static const int k_menu_ids[] = { MENU_CONTROL, MENU_VIDEO, MENU_EFFECTS };

            *(int *)ADDR_MENU_SOUND = *(int *)ADDR_MENU_SOUND_ID;
            ((OpenMenuFn)ADDR_OPEN_MENU)(k_menu_ids[*row - OPTIONS_CONTROL_ROW], 0, -1);
        }
        return 0;
    }
    if (*row == OPTIONS_BACK_ROW) {
        *row = ORIG_OPTIONS_BACK_ROW;
        *row_copy = ORIG_OPTIONS_BACK_ROW;
        r = g_options_update_orig(menu);
        if (*row == ORIG_OPTIONS_BACK_ROW) {
            *row = OPTIONS_BACK_ROW;
            *row_copy = OPTIONS_BACK_ROW;
        }
        return r;
    }
    return g_options_update_orig(menu);
}

/*
 * "Tweaks" and "Mods" are drawn right before the last row (Quit) of the main menu and the pause menu.
 * The first inserted row index is taken from the live draw counter, not from the menu's max-row word:
 * the game computes that word once when the menu opens, but the pause menu's optional "drop out" row
 * comes and goes with the second player, which would swap the custom and Quit rows.
 */
#define MENU_DRAW_ROW_OFS 140   /* dword: row counter of the draw pass, bumped by every row call */

static int g_tweaks_row = -1;

static int CustomMenuRows(void)
{
    return 1 + (ModsCount() > 0);
}

static void NoteTweaksRow(int menu)
{
    g_tweaks_row = *(int *)(menu + MENU_DRAW_ROW_OFS);
}

/* Quit is the last row: keep the cursor range in step with the rows really drawn. */
static void FitRowRange(int menu)
{
    short *row = (short *)(menu + MENU_CUR_ROW_OFS);
    short last = (short)(*(int *)(menu + MENU_DRAW_ROW_OFS) - 1);

    *(short *)(menu + MENU_MAX_ROW_OFS) = last;
    if (*row > last) {
        *row = last;
        *(int *)(menu + 52) = last;
    }
}

static int __cdecl MainQuitRow(int menu, const char *text, int alpha)
{
    int r;

    NoteTweaksRow(menu);
    ((MenuRowFn)ADDR_MENU_ROW_TEXT)(menu, "Tweaks", alpha);     /* same fade as the other rows */
    if (ModsCount() > 0)
        ((MenuRowFn)ADDR_MENU_ROW_TEXT)(menu, "Mods", alpha);
    r = ((MenuRowFn)ADDR_MENU_ROW_TEXT)(menu, text, alpha);
    FitRowRange(menu);
    return r;
}

static int __cdecl PauseQuitRow(int menu, const char *text)
{
    int r;

    NoteTweaksRow(menu);
    ((PauseRowFn)ADDR_PAUSE_ROW_DRAW)(menu, "Tweaks");
    if (ModsCount() > 0)
        ((PauseRowFn)ADDR_PAUSE_ROW_DRAW)(menu, "Mods");
    r = ((PauseRowFn)ADDR_PAUSE_ROW_DRAW)(menu, text);
    FitRowRange(menu);
    return r;
}

static int OpenCustomRow(int menu)
{
    int row = *(short *)(menu + MENU_CUR_ROW_OFS);

    if (g_tweaks_row <= 0 || !*(int *)(menu + MENU_SELECT_FLAG_OFS) || *(int *)(menu + MENU_BACK_OFS))
        return 0;
    if (row == g_tweaks_row)
        g_mods_page = 0;
    else if (ModsCount() > 0 && row == g_tweaks_row + 1)
        g_mods_page = 1;
    else
        return 0;
    *(int *)ADDR_MENU_SOUND = *(int *)ADDR_MENU_SOUND_ID;
    ((OpenMenuFn)ADDR_OPEN_MENU)(MENU_TWEAKS, 0, -1);
    return 1;
}

/* Quit follows the visible custom rows: the original update routine still knows its old index. */
static int CallShifted(int menu, MenuUpdateFn orig, int stock_quit)
{
    short *row = (short *)(menu + MENU_CUR_ROW_OFS);
    int *row_copy = (int *)(menu + 52);
    int quit = g_tweaks_row + CustomMenuRows(), shifted = g_tweaks_row > 0 && *row == quit, r;

    if (shifted) {
        *row = (short)stock_quit;
        *row_copy = stock_quit;
    }
    r = orig(menu);
    if (shifted && *row == stock_quit) {
        *row = (short)quit;
        *row_copy = quit;
    }
    return r;
}

static int __cdecl MainMenuUpdate(int menu)
{
    if (OpenCustomRow(menu))
        return 0;
    return CallShifted(menu, g_main_update_orig, MAIN_ORIG_QUIT_ROW);
}

static int __cdecl PauseMenuUpdate(int menu)
{
    short *row = (short *)(menu + MENU_CUR_ROW_OFS);

    if (OpenCustomRow(menu))
        return 0;
    /* Stock rows 2-4 are hidden. Remap every remaining action before Tweaks/Mods:
     * area-specific stock row 5, optional Drop Out stock row 6, then final Quit below. */
    if (*row >= 2 && *row < g_tweaks_row) {
        int *row_copy = (int *)(menu + 52);
        int actual = *row, stock = actual + 3;
        int r;

        *row = (short)stock;
        *row_copy = stock;
        r = g_pause_update_orig(menu);
        if (*row == stock) {
            *row = (short)actual;
            *row_copy = actual;
        }
        return r;
    }
    return CallShifted(menu, g_pause_update_orig, g_tweaks_row + 3);
}

int MenusReady(void)
{
    return Matches(TWEAKS_DRAW_ORIG, (const uint8_t[]){ 0xC3, 0xCC, 0xCC, 0xCC, 0xCC }, 5) &&
           Matches(TWEAKS_UPDATE_ORIG, (const uint8_t[]){ 0xC3, 0xCC, 0xCC, 0xCC, 0xCC }, 5) &&
           Matches(ADDR_FX_FSAA_ROW_CALL, (const uint8_t[]){ 0xE8 }, 1) &&
           Matches(ADDR_VID_ASPECT_ROW_CALL, (const uint8_t[]){ 0xE8 }, 1) &&
           Matches(FX_UPDATE_ORIG, k_effects_update_orig, sizeof k_effects_update_orig) &&
           Matches(ADDR_MAIN_QUIT_CALL, (const uint8_t[]){ 0xE8 }, 1) &&
           Matches(ADDR_PAUSE_QUIT_CALL_1, (const uint8_t[]){ 0xE8 }, 1) &&
           Matches(ADDR_PAUSE_QUIT_CALL_2, (const uint8_t[]){ 0xE8 }, 1) &&
           Matches(ADDR_PAUSE_QUIT_CALL_3, (const uint8_t[]){ 0xE8 }, 1) &&
           Matches(ADDR_MAIN_CONTROL_CALL, (const uint8_t[]){ 0xE8 }, 1) &&
           Matches(ADDR_MAIN_VIDEO_CALL, (const uint8_t[]){ 0xE8 }, 1) &&
           Matches(ADDR_MAIN_EFFECTS_CALL, (const uint8_t[]){ 0xE8 }, 1) &&
           Matches(ADDR_OPTIONS_BACK_CALL, (const uint8_t[]){ 0xE8 }, 1) &&
           Matches(ADDR_PAUSE_CONTROL_CALL, (const uint8_t[]){ 0xE8 }, 1) &&
           Matches(ADDR_PAUSE_VIDEO_CALL, (const uint8_t[]){ 0xE8 }, 1) &&
           Matches(ADDR_PAUSE_EFFECTS_CALL, (const uint8_t[]){ 0xE8 }, 1) &&
           Matches(MAIN_UPDATE_ORIG, k_main_update_orig, sizeof k_main_update_orig) &&
           Matches(PAUSE_UPDATE_ORIG, k_pause_update_orig, sizeof k_pause_update_orig) &&
           Matches(OPTIONS_UPDATE_ORIG, k_options_update_orig, sizeof k_options_update_orig) &&
           Matches(ADDR_VID_ROW_INSERT, (const uint8_t[]){ 0xB9, 0xC8, 0x65, 0x97, 0x02 }, 5) &&
           Matches(VID_MENU_UPDATE_ORIG, k_video_update_orig, sizeof k_video_update_orig);
}

void MenusInstall(void)
{
    g_options_update_orig = MakeTrampoline((const void *)OPTIONS_UPDATE_ORIG, sizeof k_options_update_orig,
                                           OPTIONS_UPDATE_ORIG + sizeof k_options_update_orig);
    Log(g_options_update_orig && PatchJmp(OPTIONS_UPDATE_ORIG, OptionsMenuUpdate, sizeof k_options_update_orig) ?
        "Options handler added" : "Options handler FAILED");

    g_main_update_orig = MakeTrampoline((const void *)MAIN_UPDATE_ORIG, sizeof k_main_update_orig,
                                        MAIN_UPDATE_ORIG + sizeof k_main_update_orig);
    g_pause_update_orig = MakeTrampoline((const void *)PAUSE_UPDATE_ORIG, sizeof k_pause_update_orig,
                                         PAUSE_UPDATE_ORIG + sizeof k_pause_update_orig);
    Log(g_main_update_orig && PatchJmp(MAIN_UPDATE_ORIG, MainMenuUpdate, sizeof k_main_update_orig) ?
        "main menu handler added" : "main menu handler FAILED");
    Log(g_pause_update_orig && PatchJmp(PAUSE_UPDATE_ORIG, PauseMenuUpdate, sizeof k_pause_update_orig) ?
        "pause menu handler added" : "pause menu handler FAILED");

    /* Render Scale row on the Video Settings page, before Back */
    g_video_update_orig = MakeTrampoline((const void *)VID_MENU_UPDATE_ORIG, sizeof k_video_update_orig,
                                         VID_MENU_UPDATE_ORIG + sizeof k_video_update_orig);
    Log(g_video_update_orig && PatchCall(ADDR_VID_ROW_INSERT, VideoRowStub) &&
        PatchJmp(VID_MENU_UPDATE_ORIG, VideoMenuUpdate, sizeof k_video_update_orig) ?
        "render scale menu row added" : "render scale menu row FAILED");

    /* the spare menu callbacks become the Tweaks and Mods pages */
    Log(PatchJmp(TWEAKS_DRAW_ORIG, TweaksDraw, 5) && PatchJmp(TWEAKS_UPDATE_ORIG, TweaksUpdate, 5) ?
        "tweaks page added" : "tweaks page FAILED");

    /* "Tweaks" is drawn just before Quit in the main and pause menu */
    Log(PatchCall(ADDR_MAIN_QUIT_CALL, MainQuitRow) && PatchCall(ADDR_PAUSE_QUIT_CALL_1, PauseQuitRow) &&
        PatchCall(ADDR_PAUSE_QUIT_CALL_2, PauseQuitRow) && PatchCall(ADDR_PAUSE_QUIT_CALL_3, PauseQuitRow) ?
        "tweaks row drawn before Quit" : "tweaks row draw hook FAILED");

    Log(PatchCall(ADDR_MAIN_CONTROL_CALL, HiddenRow) && PatchCall(ADDR_MAIN_VIDEO_CALL, HiddenRow) &&
        PatchCall(ADDR_MAIN_EFFECTS_CALL, HiddenRow) && PatchCall(ADDR_OPTIONS_BACK_CALL, OptionsBackRow) &&
        PatchCall(ADDR_PAUSE_CONTROL_CALL, HiddenPauseRow) && PatchCall(ADDR_PAUSE_VIDEO_CALL, HiddenPauseRow) &&
        PatchCall(ADDR_PAUSE_EFFECTS_CALL, HiddenPauseRow) ?
        "settings moved into Options" : "settings move FAILED");

    /* the Aspect row of Video Settings is gone while FixAspect is on: the layout follows the screen's shape (src/aspect.c) */
    Log(PatchCall(ADDR_VID_ASPECT_ROW_CALL, AspectRow) ? "Aspect row hidden" : "Aspect row hide FAILED");

    /* FSAA row of Effects Settings is gone */
    g_effects_update_orig = MakeTrampoline((const void *)FX_UPDATE_ORIG, sizeof k_effects_update_orig,
                                           FX_UPDATE_ORIG + sizeof k_effects_update_orig);
    Log(g_effects_update_orig && PatchCall(ADDR_FX_FSAA_ROW_CALL, HiddenRow) &&
        PatchJmp(FX_UPDATE_ORIG, EffectsMenuUpdate, sizeof k_effects_update_orig) ?
        "FSAA row hidden" : "FSAA row hide FAILED");
}
