/*
 * Menus: Render Scale row in Video Settings, the Tweaks page, hidden FSAA row, and the
 * Tweaks entry in the main and pause menu.
 */
#include "all.h"

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


#define ADDR_MENU_SOUND       0x00800940u

#define ADDR_MENU_SOUND_ID    0x00800950u
#define ADDR_MENU_CANCEL_SOUND_ID 0x00800958u

#define MENU_BACK_OFS         0x68  /* dword: Back / cancel pressed (the stock pages pop the menu on it) */

#define MENU_CUR_ROW_OFS      10    /* word */

#define SCALE_ROW             4

#define BACK_ROW              5

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

    if (*(int *)(menu + MENU_BACK_OFS))
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

    if (*(int *)(menu + MENU_BACK_OFS)) {   /* B / Escape: leave the page like the stock options pages */
        *(int *)ADDR_MENU_SOUND = *(int *)ADDR_MENU_CANCEL_SOUND_ID;
        ((void (__cdecl *)(void))ADDR_MENU_POP)();
        return 0;
    }
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

    if (*(int *)(menu + MENU_BACK_OFS))
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

static int __cdecl PauseQuitRow(int menu, const char *text)
{
    ((PauseRowFn)ADDR_PAUSE_ROW_DRAW)(menu, "Tweaks");
    return ((PauseRowFn)ADDR_PAUSE_ROW_DRAW)(menu, text);
}

static int OpenTweaksRow(int menu)
{
    int row = *(short *)(menu + MENU_CUR_ROW_OFS);
    int last = *(short *)(menu + MENU_MAX_ROW_OFS);

    if (row != last - 1 || row <= 0 || !*(int *)(menu + MENU_SELECT_FLAG_OFS) || *(int *)(menu + MENU_BACK_OFS))
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
int MenusReady(void)
{
    return Matches(ADDR_TWEAKS_DRAW, (const uint8_t[]){ 0x80, 0x94, 0x50, 0x00 }, 4) &&
           Matches(ADDR_TWEAKS_UPDATE, (const uint8_t[]){ 0x90, 0x94, 0x50, 0x00 }, 4) &&
           Matches(ADDR_FX_FSAA_ROW_CALL, (const uint8_t[]){ 0xE8 }, 1) &&
           Matches(ADDR_FX_UPDATE, (const uint8_t[]){ 0xD0, 0xF0, 0x50, 0x00 }, 4) &&
           Matches(ADDR_MAIN_QUIT_CALL, (const uint8_t[]){ 0xE8 }, 1) &&
           Matches(ADDR_PAUSE_QUIT_CALL_1, (const uint8_t[]){ 0xE8 }, 1) &&
           Matches(ADDR_PAUSE_QUIT_CALL_2, (const uint8_t[]){ 0xE8 }, 1) &&
           Matches(ADDR_PAUSE_QUIT_CALL_3, (const uint8_t[]){ 0xE8 }, 1) &&
           Matches(ADDR_VID_ROW_INSERT, (const uint8_t[]){ 0xB9, 0xC8, 0x65, 0x97, 0x02 }, 5) &&
           Matches(ADDR_VID_MENU_UPDATE, (const uint8_t[]){ 0x80, 0xEF, 0x50, 0x00 }, 4);
}

void MenusInstall(void)
{
    /* Render Scale row on the Video Settings page, before Back */
    Log(PatchCall(ADDR_VID_ROW_INSERT, VideoRowStub) && PatchPtr(ADDR_VID_MENU_UPDATE, VideoMenuUpdate) ?
        "render scale menu row added" : "render scale menu row FAILED");

    /* the spare menu table entry becomes the Tweaks page */
    Log(PatchPtr(ADDR_TWEAKS_DRAW, TweaksDraw) && PatchPtr(ADDR_TWEAKS_UPDATE, TweaksUpdate) ?
        "tweaks page added" : "tweaks page FAILED");

    /* "Tweaks" is drawn just before Quit in the main and pause menu */
    Log(PatchCall(ADDR_MAIN_QUIT_CALL, MainQuitRow) && PatchCall(ADDR_PAUSE_QUIT_CALL_1, PauseQuitRow) &&
        PatchCall(ADDR_PAUSE_QUIT_CALL_2, PauseQuitRow) && PatchCall(ADDR_PAUSE_QUIT_CALL_3, PauseQuitRow) ?
        "tweaks row drawn before Quit" : "tweaks row draw hook FAILED");

    /* FSAA row of Effects Settings is gone */
    Log(PatchCall(ADDR_FX_FSAA_ROW_CALL, HiddenRow) && PatchPtr(ADDR_FX_UPDATE, EffectsMenuUpdate) ?
        "FSAA row hidden" : "FSAA row hide FAILED");
}

void MenusFrame(void)
{
    InstallFrontMenus();
}
