/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Aspect ratio: the layout follows the real shape of the backbuffer, whatever it is (4:3, 16:10, 16:9, 21:9, 32:9, a window).
 *
 * The game knows one setting, dword_826494: 0 4:3, 1 16:9, 2 16:10, 3 "From Screen Res" (class dev + 0x62C and ratio dev + 0x630
 * from the backbuffer; every code that switches on the setting handles 3 with the real ratio: sub_6DF290, sub_40A520, the logo
 * fade, Bink videos). The setting is kept at 3. The pieces that did not use the ratio are replaced:
 *  - sub_565470 (called every frame, sets the main camera) used 9/16 for every widescreen ratio but 16:10, and the width of every
 *    glyph (sub_50E6D0) was the 16:9 value 0.75: CameraAspect takes both from AspectValue().
 * UI (HUD icons, text, character selector) is placed in camera units at a fixed size, which is the same number of pixels at the same
 * screen height, so with the 16:9 constants (vertical view, text scale) it is exactly the 16:9 layout, centered; only the world
 * widens. 16:9 is the base at every ratio. A 4:3 screen keeps the game's own 4:3 layout.
 * Two options widen more: CutsceneFOV (a cutscene keeps its 16:9 vertical view instead of cropping top and bottom) and HudWide (the corner
 * HUD moves out to the edges of a screen wider than 16:9).
 */
#include "all.h"

int g_probe_camera;   /* ProbeCamera: log the main camera and the projection it produced while a cutscene plays */
int g_probe_effects;   /* ProbeEffects: log the flying sprite effects (picked-up studs) every frame */
int g_cutscene_fov = 1;   /* CutsceneFOV: a cutscene keeps its 16:9 vertical view on wider screens */
int g_hud_wide;   /* HudWide: the corner HUD (portraits, hearts, counters) sits at the edges of a screen wider than 16:9 */
int g_fix_aspect = 1;   /* FixAspect: 0 leaves the game's own aspect handling untouched */
double g_force_aspect;   /* debug: lay out for this width / height whatever the backbuffer is (ForceAspect) */

#define ADDR_ASPECT_SETTING 0x00826494u   /* dword_826494 */
#define ADDR_ASPECT_MODE    0x0297DBD0u   /* dword_297DBD0: class the menus read */
#define ADDR_PENDING        0x00921D04u   /* dword_921D04: the Aspect row's value in Video Settings */
#define DEV_ASPECT_CLASS    (ADDR_DEVICE_OBJECT + 0x62C)   /* 0 4:3, nonzero widescreen */
#define DEV_ASPECT_FLOAT    (ADDR_DEVICE_OBJECT + 0x630)   /* width / height of the current mode */
#define DEV_BACKBUFFER      (ADDR_DEVICE_OBJECT + 0x138)   /* present parameters: backbuffer width, height */
#define ADDR_CAMERA_PTR     0x00925164u
#define ADDR_FOV_FLAG       0x00921359u
#define ADDR_FOV_USER       0x00921360u
#define ADDR_CAMERA_FN      0x00565470u   /* sub_565470(wide) */
#define ADDR_TEXT_SCALE_FN  0x0050E6D0u   /* sub_50E6D0(x, y): glyph scale */
#define ADDR_FOV_FN         0x005D6CC0u   /* sub_5D6CC0(float) */
#define ADDR_WIDE           0x008668D7u   /* byte_8668D7: widescreen layout */
#define ADDR_TEXT_WIDTH     0x00921D00u   /* flt_921D00: width of the text line just measured */
#define ADDR_TEXT_FN        0x0050C780u   /* sub_50C780(text, x, y, z, ...): every text line the game draws */
#define ADDR_CHKSTK         0x006E6D60u
#define WIDE_V              0.5625        /* 9 / 16: the camera aspect the widescreen field of view and HUD were made for */
#define ADDR_HUD_PASS      0x004B9110u
#define HUD_PASS_SIZE      0x1075u
#define HUD_SIDE_MIN 0.4f   /* an anchor nearer the middle than this is a centered element and stays */
#define WIDE_MIN            1.4667        /* the game's own boundary between its 4:3 and widescreen layouts */

/* Width / height the picture is laid out for. With FixAspect off: what the game's own setting says (sub_6DF290). */
double AspectValue(void)
{
    const int *pp = (const int *)DEV_BACKBUFFER;
    double real = pp[0] > 0 && pp[1] > 0 ? (double)pp[0] / pp[1] : 0.0;

    if (!g_fix_aspect) {
        switch (*(const int *)ADDR_ASPECT_SETTING) {
        case 1:  return 16.0 / 9.0;
        case 2:  return 1.6;
        case 3:  return *(const float *)DEV_ASPECT_FLOAT;
        default: return 4.0 / 3.0;
        }
    }
    if (g_force_aspect > 0.0)
        return g_force_aspect;
    return real > 1.0 && real < 8.0 ? real : 16.0 / 9.0;
}

/* Debug: the sprite effect records (276 bytes, sub_560B60 / sub_561400): flags +216, progress +188 / +192, positions at floats 34 (end), 37 (start), 40 (current), scales at 43..46. */
static void ProbeEffects(void)
{
    static unsigned frame;
    char line[320];
    int i;

    frame++;
    for (i = 0; i < 128; i++) {
        const float *r = (const float *)(0x00947BC0u + 276u * i);
        const uint8_t *b = (const uint8_t *)r;
        uint32_t flags = *(const uint32_t *)(b + 216);

        if (!b[248] || !(flags & 0x20000))
            continue;
        snprintf(line, sizeof line,
                 "fx %u #%d flags %05X t %.3f/%.3f  end %.3f %.3f %.3f  start %.3f %.3f %.3f  cur %.3f %.3f %.3f  scale %.3f %.3f %.3f %.3f  aspect %.3f",
                 frame, i, flags, r[47], r[48], r[34], r[35], r[36], r[37], r[38], r[39], r[40], r[41], r[42],
                 r[43], r[44], r[45], r[46], AspectValue());
        Log(line);
    }
}

/* Debug: the main camera (fov cam[16], h / w cam[17]) next to what sub_5ED540 last built from a camera (flt_973728, flt_97372C, y scale flt_97313C, x scale flt_966378). */
static void ProbeCamera(void)
{
    static unsigned frame;
    char line[256];
    const float *cam = (const float *)*(uintptr_t *)ADDR_CAMERA_PTR;
    int inst = *(const int *)0x00951BE8u;

    if (++frame % 20 || !cam)
        return;
    snprintf(line, sizeof line,
             "cam %u cutscene %d inst %08X flags %08X  cam fov %.4f asp %.4f  used fov %.4f asp %.4f  yscale %.4f xscale %.4f  aspect %.4f",
             frame, *(const int *)0x00951BE0u, inst, inst ? *(const unsigned *)(inst + 80) : 0u, cam[16], cam[17],
             *(const float *)0x00973728u, *(const float *)0x0097372Cu, *(const float *)0x0097313Cu, *(const float *)0x00966378u,
             AspectValue());
    Log(line);
}

/*
 * Menu pages that take the HUD's place (sub_4BAE40 skips sub_4B9110 for them) draw the two corner portraits themselves at
 * x = +-flt_7FC31C, the HUD's portrait anchor, with the texts under them: pages 8 (sub_497000), 12 (sub_4265A0), 13 (sub_496650),
 * 14 (sub_4967B0) and the character select pages 15..17 (sub_47B160, sub_47B450 and the Free Play one, sub_47BB70, which also fly the
 * chosen characters to and from the corner). The end of level screen is one of them. Their loads of the constant read this copy, which
 * HudWide moves outward like the HUD's portraits. The page table is at 0x7FACEC: 28 bytes per page, id at +4, draw function at +12.
 */
#define ADDR_PORTRAIT_X 0x007FC31Cu
static const uintptr_t k_corner_loads[] = {   /* fld ds:flt_7FC31C */
    0x00457FE9u,   /* sub_457F70: a label and its icon placed from the left portrait's x */
    0x00485042u,   /* sub_485030: a pair of corner portraits (from sub_486200) */
    0x004973E8u, 0x00497455u,                                                                             /* page 8 */
    0x00426632u, 0x0042669Cu, 0x0042682Eu, 0x00426AE1u,                                                   /* page 12 */
    0x004966BBu, 0x0049671Fu,                                                                             /* page 13 */
    0x00496810u, 0x00496876u, 0x00496A05u, 0x00496A6Eu, 0x00496ADFu, 0x00496B51u, 0x00496BB8u, 0x00496C27u,   /* page 14 */
    0x0047B2D1u, 0x0047B343u, 0x0047B5AFu, 0x0047B621u, 0x0047B78Du,
    0x0047BE01u, 0x0047BEEFu, 0x0047BFADu, 0x0047BFC6u, 0x0047C7A9u, 0x0047C7FDu,
};
#define CORNER_LOADS ((int)(sizeof k_corner_loads / sizeof k_corner_loads[0]))
static const uint8_t k_corner_orig[6] = { 0xD9, 0x05, 0x1C, 0xC3, 0x7F, 0x00 };
static float g_select_corner;

/* The button hints of those screens (Accept / Back, sub_4B79A0, used by 9 screens) sit in the bottom corners at x = +-flt_7FC350. */
#define ADDR_HINT_X     0x007FC350u
#define ADDR_HINT_LOAD  0x004B7B16u   /* fld ds:flt_7FC350 */
static const uint8_t k_hint_orig[6] = { 0xD9, 0x05, 0x50, 0xC3, 0x7F, 0x00 };
static float g_hint_corner;

/*
 * sub_457F70 slides its icon in from x = -1.1, just outside a 16:9 screen: sin(t) * (end - -1.1) - 1.1, with the two 1.1 as pooled
 * doubles. On a wider screen that point is inside the picture and the icon popped up there. The two operands read these copies, which
 * hold the same distance outside the real edge (this follows the screen's shape, not HudWide).
 */
#define ADDR_SLIDE_FROM_1 0x004582CAu   /* fsub ds:dbl_7586D8 (-1.1) */
#define ADDR_SLIDE_FROM_2 0x004582D8u   /* fsub ds:dbl_751CF8 (1.1) */
#define SLIDE_FROM 1.100000023841858
static const uint8_t k_slide_orig_1[6] = { 0xDC, 0x25, 0xD8, 0x86, 0x75, 0x00 };
static const uint8_t k_slide_orig_2[6] = { 0xDC, 0x25, 0xF8, 0x1C, 0x75, 0x00 };
static double g_slide_from_neg = -SLIDE_FROM, g_slide_from_pos = SLIDE_FROM;

static float SideShift(float x);

void AspectFrame(void)
{
    static const int screen = 3;

    g_select_corner = SideShift(*(const float *)ADDR_PORTRAIT_X);
    g_hint_corner = SideShift(*(const float *)ADDR_HINT_X);
    {
        double d = g_fix_aspect ? AspectValue() / (16.0 / 9.0) - 1.0 : 0.0;

        g_slide_from_pos = SLIDE_FROM + (d > 0.0 ? d : 0.0);
        g_slide_from_neg = -g_slide_from_pos;
    }
    if (g_probe_camera)
        ProbeCamera();
    if (g_probe_effects)
        ProbeEffects();

    if (!g_fix_aspect)
        return;
    if (*(const int *)ADDR_ASPECT_SETTING != screen)
        *(int *)ADDR_ASPECT_SETTING = screen;
    if (*(const int *)ADDR_PENDING != screen)
        *(int *)ADDR_PENDING = screen;            /* the row shows From Screen Res; it has nothing left to choose */
    if (g_force_aspect > 0.0) {
        int wide = g_force_aspect >= WIDE_MIN;

        *(float *)DEV_ASPECT_FLOAT = (float)g_force_aspect;
        *(int *)DEV_ASPECT_CLASS = wide;
        *(int *)ADDR_ASPECT_MODE = wide;
    }
}

static void TextScale(float x)
{
    ((void (__cdecl *)(float, float))ADDR_TEXT_SCALE_FN)(x, 1.0f);
}

/*
 * sub_565470 with the ratio taken from AspectValue(). Camera: +0x44 is the y / x scale ratio, +0x40 the field of view in radians.
 * Widescreen keeps the vertical view of 4:3 shrunk halfway towards the new ratio (the original's (9/16 + 3/4) / 2 at 16:9).
 */
static void __cdecl CameraAspect(int wide)
{
    float *cam = (float *)*(uintptr_t *)ADDR_CAMERA_PTR;
    float user = *(const float *)ADDR_FOV_USER;
    int custom = *(const uint8_t *)ADDR_FOV_FLAG != 0;
    /* v: camera aspect (height / width); fv: the one the widescreen field of view derives from; text: glyph x scale */
    float v, fv, text, fov = 0.75f;

    if (g_fix_aspect) {
        v = (float)(1.0 / AspectValue());
        fv = WIDE_V;   /* widescreen: the vertical view of 16:9 at every ratio (the view widens or narrows sideways), so the HUD keeps its 16:9 size */
        text = v / 0.75f;   /* glyph width is in screen-width units: (4/3) / ratio is the same pixels as the 16:9 value 0.75 at 16:9 */
    } else {   /* sub_565470 as the game has it */
        v = wide ? (*(const int *)ADDR_ASPECT_SETTING == 2 ? 0.625f : 0.5625f) : 0.75f;
        fv = v;
        text = wide ? 0.75f : 1.0f;
    }
    cam[17] = v;
    /* The title ("press any button") runs with the 4:3 layout flag (wide = 0, fov 0.75); the menu then sets wide and the fov drops
     * to the widescreen one, so the logo jumped 1.156 times bigger. The title takes the widescreen fov too. */
    if (g_fix_aspect && g_fix_title_aspect && !wide) {
        int game = *(const int *)0x00802C54u;

        wide = game && *(const int *)(game + GAME_CUR_LEVEL_OFS) == *(const int *)ADDR_TITLES_LEVEL;
    }
    if (wide)
        fov = custom && user == 0.0f ? fv : (float)((fv + 0.75) * 0.5);
    if (custom && user != 0.0f) {
        float rad = (float)((double (__cdecl *)(float))ADDR_FOV_FN)(user);   /* focal length in, radians out (x87 return) */

        fov = wide ? (float)(rad * (fv / 0.75)) : rad;
    }
    cam[16] = fov;
    TextScale(text);
}

/*
 * Text is placed in screen-width units (glyph width: CameraAspect's text scale; line anchor: x = 0 is the middle, the edges lie at
 * about +-1), while the icons next to it are in camera units that the 16:9 constants keep centered: on a wider screen the anchors
 * would drift away from their icons. Every text line goes through sub_50C780, whose x anchor is pulled in to the 16:9 width (line
 * contents are laid out from the anchor, so they keep their spacing).
 */
static int g_in_hud;   /* the per-player HUD (sub_4B9110) is being drawn */

/* HudWide: an anchor on a side of the screen moves outward by the width the screen has beyond 16:9 (x = +-1 is the 16:9 side). */
static float SideShift(float x)
{
    double d = AspectValue() / (16.0 / 9.0) - 1.0;

    if (!g_hud_wide || !g_fix_aspect || d <= 0.0 || fabsf(x) < HUD_SIDE_MIN)
        return x;
    return (float)(x + (x > 0.0f ? d : -d));
}

float __cdecl HudShift(float x)
{
    return g_in_hud ? SideShift(x) : x;
}

/* x points at the x argument of sub_50C780 on entry, the return address is two words below it. Text drawn by a callee of the HUD
 * (the join prompt inside sub_4B88B0) is placed from an anchor that was already moved, so only a call made by sub_4B9110 itself moves. */
void __cdecl TextAnchor(float *x)
{
    uintptr_t ret = *(const uintptr_t *)((const char *)x - 8);

    if (g_fix_aspect && *(const uint8_t *)ADDR_WIDE) {
        if (ret >= ADDR_HUD_PASS && ret < ADDR_HUD_PASS + HUD_PASS_SIZE)
            *x = HudShift(*x);
        *x = (float)(*x * (16.0 / 9.0) / AspectValue());
    }
}

extern void TextHook(void);

__asm__(
    ".text\n"
    ".globl _TextHook\n"
    "_TextHook:\n"
    "    leal 8(%esp),%eax\n"
    "    pushl %eax\n"
    "    call _TextAnchor\n"
    "    addl $4,%esp\n"
    "    movl $0x141C,%eax\n"
    "    movl $0x006E6D60,%ecx\n"
    "    call *%ecx\n"
    "    pushl $0x0050C78A\n"
    "    ret\n"
);

/*
 * The coin / stud counters place their icons from the measured text width (flt_921D00, in the same screen-width units as the glyphs,
 * so it shrinks with the ratio) in camera units, which do not shrink: the icons would sit on the digits. They read the width at four
 * places (sub_4B80E0, sub_45B290); each now gets it scaled back to its 16:9 value.
 */
float __cdecl TextWidth16(void)
{
    return (float)(*(const float *)ADDR_TEXT_WIDTH * (g_fix_aspect ? AspectValue() / (16.0 / 9.0) : 1.0));
}

extern void TextWidthStub(void);

__asm__(
    ".text\n"
    ".globl _TextWidthStub\n"
    "_TextWidthStub:\n"
    "    pushl %eax\n"
    "    pushl %ecx\n"
    "    pushl %edx\n"
    "    call _TextWidth16\n"
    "    popl %edx\n"
    "    popl %ecx\n"
    "    popl %eax\n"
    "    ret\n"
);

static const uintptr_t k_width_reads[] = { 0x0045B847u, 0x0045B8CBu, 0x004B81E5u, 0x004B8285u };   /* fld dword [flt_921D00] */
static const uint8_t k_width_orig[6] = { 0xD9, 0x05, 0x00, 0x1D, 0x92, 0x00 };

static const uint8_t k_text_orig[5] = { 0xB8, 0x1C, 0x14, 0x00, 0x00 };

static int TextReady(void)
{
    const uint8_t *b = (const uint8_t *)ADDR_TEXT_FN;
    int i;

    for (i = 0; i < 4; i++)
        if (!Matches(k_width_reads[i], k_width_orig, sizeof k_width_orig))
            return 0;

    return Matches(ADDR_TEXT_FN, k_text_orig, sizeof k_text_orig) && b[5] == 0xE8 &&
           ADDR_TEXT_FN + 10 + *(const int32_t *)(b + 6) == ADDR_CHKSTK;
}

/*
 * Flying studs (sprite effects, sub_561400 draws them from sub_4BAE40): a picked-up stud starts at its screen position, stored as a
 * fraction of the screen (record float 40, +-1 at the screen's sides: the log of ProbeEffects shows it fixed for the whole flight) and
 * flies to the counter's icon. The effect is drawn through the HUD mapping (HUD x * flt_87B938), which is the 16:9 one at every ratio,
 * so on a wider screen the start was pulled toward the middle and the stud began smaller than the one on the ground (its start size,
 * float 46, is a fraction of the screen's width too) and then jumped as it flew. The start x and the start size are scaled by
 * ratio / (16/9) once per record (a record whose value the game rewrote is scaled again; one that is unchanged is left alone).
 * y and z are not: y is a fraction of the screen's height and z is the HUD plane's depth, both independent of the ratio.
 */
#define ADDR_FX_DRAW 0x00561400u
static const uintptr_t k_fx_calls[] = { 0x004BB916u, 0x004BBA52u };

static void ScaleEffectStarts(void)
{
    static float done_x[128], done_s[128], done_e[128];   /* the values written back for each record, 0 = not scaled yet */
    double k = AspectValue() / (16.0 / 9.0);
    int i;

    for (i = 0; i < 128; i++) {
        float *r = (float *)(0x00947BC0u + 276u * i);
        const uint8_t *b = (const uint8_t *)r;
        uint32_t flags = *(const uint32_t *)(b + 216);

        if (!b[248] || (flags & 0x20001) != 0x20001) {
            done_x[i] = done_s[i] = done_e[i] = 0.0f;
            continue;
        }
        if ((flags & 8) && r[34] != 0.0f && r[34] != done_e[i]) {   /* the flight's end on the HUD: it follows the corner HUD (HudWide) */
            r[34] = SideShift(r[34]);
            done_e[i] = r[34];
        }
        if (r[40] != 0.0f && r[40] != done_x[i]) {
            r[40] = (float)(r[40] * k);
            done_x[i] = r[40];
        }
        if (r[46] != 0.0f && r[46] != done_s[i]) {   /* the start size is a fraction of the screen's width as well */
            r[46] = (float)(r[46] * k);
            done_s[i] = r[46];
        }
    }
}

static void __cdecl EffectsDraw(void)
{
    if (g_fix_aspect)
        ScaleEffectStarts();
    ((void (__cdecl *)(void))ADDR_FX_DRAW)();
}

/*
 * A cutscene sets its own field of view (sub_40ADE0: the lens at flt_921360 through sub_5D6CC0) and scales it by (4/3) / ratio, with the
 * ratio from sub_40A520: the horizontal view stays that of 4:3 and a wider screen crops the top and bottom of the shot. The ratio is
 * held at 16:9 from there up, so the vertical view stays that of 16:9 and the shot widens sideways, like the gameplay camera.
 */
#define ADDR_RATIO_FN   0x0040A520u   /* sub_40A520: this = dev, x87 return = width / height for the setting */
#define ADDR_RATIO_CALL 0x0040AF3Cu

static double __cdecl CutsceneRatio(void)
{
    double r = ((double (__fastcall *)(void *))ADDR_RATIO_FN)((void *)ADDR_DEVICE_OBJECT);

    return g_fix_aspect && g_cutscene_fov && r > 16.0 / 9.0 ? 16.0 / 9.0 : r;
}


/*
 * The per-player HUD (sub_4B9110) draws every side element at a fixed x = +-const through a handful of sprite calls; each of those calls
 * gets a thunk that passes its x argument (the first or the second stack argument) through HudShift. An element moves as a whole, as
 * its parts are placed relative to the anchor. Text goes through TextAnchor.
 */
#define HUD_THUNK(name, arg_off, target) \
    extern void name(void); \
    __asm__(".text\n.globl _" #name "\n_" #name ":\n" \
            "    pushl %eax\n    pushl %ecx\n    pushl %edx\n" \
            "    pushl " #arg_off "+12(%esp)\n    call _HudShift\n    addl $4,%esp\n    fstps " #arg_off "+12(%esp)\n" \
            "    popl %edx\n    popl %ecx\n    popl %eax\n    pushl $" #target "\n    ret\n")

#define ADDR_HUD_PASS_CALL 0x004BB17Bu   /* call sub_4B9110 in sub_4BAE40 */
#define ADDR_SPRITE_FN     0x0050D910u   /* sub_50D910(tex, x, y, ...) */
#define ADDR_PORTRAIT_FN   0x0055B830u   /* sub_55B830(id, x, y, ...) */
#define ADDR_ICON_FN       0x00552C10u   /* sub_552C10(x, y, ...) */
#define ADDR_HEARTS_FN     0x004B88B0u   /* sub_4B88B0(x, y, ...), player in EAX */

HUD_THUNK(HudSprite, 8, 0x0050D910);     /* ADDR_SPRITE_FN, x is the second argument */
HUD_THUNK(HudPortrait, 8, 0x0055B830);   /* ADDR_PORTRAIT_FN */
HUD_THUNK(HudIcon, 4, 0x00552C10);       /* ADDR_ICON_FN, x is the first argument */
HUD_THUNK(HudHearts, 4, 0x004B88B0);     /* ADDR_HEARTS_FN: the player is in EAX, x is the first stack argument */

static const struct { uintptr_t site, target; void (*thunk)(void); } k_hud_calls[] = {
    { 0x004B9479u, ADDR_PORTRAIT_FN, HudPortrait },
    { 0x004B95B7u, ADDR_SPRITE_FN, HudSprite }, { 0x004B967Eu, ADDR_SPRITE_FN, HudSprite }, { 0x004B99E5u, ADDR_SPRITE_FN, HudSprite },
    { 0x004B9793u, ADDR_ICON_FN, HudIcon }, { 0x004B9807u, ADDR_ICON_FN, HudIcon }, { 0x004B9C57u, ADDR_ICON_FN, HudIcon },
    { 0x004B9924u, ADDR_HEARTS_FN, HudHearts },
};

static void __cdecl HudPass(int dim)
{
    g_in_hud = 1;
    ((void (__cdecl *)(int))ADDR_HUD_PASS)(dim);
    g_in_hud = 0;
}

static const uint8_t k_camera_orig[8] = { 0x51, 0x83, 0x3D, 0x94, 0x64, 0x82, 0x00, 0x02 };

static int CallTo(uintptr_t site, uintptr_t fn)
{
    const uint8_t *b = (const uint8_t *)site;

    return b[0] == 0xE8 && site + 5 + *(const int32_t *)(b + 1) == fn;
}

int AspectReady(void)
{
    {
        int c;

        for (c = 0; c < 2; c++)
            if (!CallTo(k_fx_calls[c], ADDR_FX_DRAW))
                return 0;
        for (c = 0; c < CORNER_LOADS; c++)
            if (!Matches(k_corner_loads[c], k_corner_orig, sizeof k_corner_orig))
                return 0;
        if (!Matches(ADDR_SLIDE_FROM_1, k_slide_orig_1, sizeof k_slide_orig_1) || !Matches(ADDR_SLIDE_FROM_2, k_slide_orig_2, sizeof k_slide_orig_2))
            return 0;
        if (!Matches(ADDR_HINT_LOAD, k_hint_orig, sizeof k_hint_orig))
            return 0;
    }
    {
        size_t h;

        for (h = 0; h < sizeof k_hud_calls / sizeof k_hud_calls[0]; h++)
            if (!CallTo(k_hud_calls[h].site, k_hud_calls[h].target))
                return 0;
        if (!CallTo(ADDR_HUD_PASS_CALL, ADDR_HUD_PASS))
            return 0;
    }
    return CallTo(ADDR_RATIO_CALL, ADDR_RATIO_FN) && TextReady() && Matches(ADDR_CAMERA_FN, k_camera_orig, sizeof k_camera_orig);
}

void AspectInstall(void)
{
    int i, ok = PatchJmp(ADDR_CAMERA_FN, CameraAspect, 5);

    ok &= PatchJmp(ADDR_TEXT_FN, TextHook, 10);
    ok &= PatchCall(ADDR_RATIO_CALL, CutsceneRatio);
    for (i = 0; i < (int)(sizeof k_hud_calls / sizeof k_hud_calls[0]); i++)
        ok &= PatchCall(k_hud_calls[i].site, k_hud_calls[i].thunk);
    ok &= PatchCall(ADDR_HUD_PASS_CALL, HudPass);
    g_select_corner = *(const float *)ADDR_PORTRAIT_X;   /* the patch thread is ahead of the first AspectFrame */
    for (i = 0; i < CORNER_LOADS; i++)
        ok &= PatchPtr(k_corner_loads[i] + 2, &g_select_corner);
    g_hint_corner = *(const float *)ADDR_HINT_X;
    ok &= PatchPtr(ADDR_HINT_LOAD + 2, &g_hint_corner);
    ok &= PatchPtr(ADDR_SLIDE_FROM_1 + 2, &g_slide_from_neg) && PatchPtr(ADDR_SLIDE_FROM_2 + 2, &g_slide_from_pos);
    ok &= PatchCall(k_fx_calls[0], EffectsDraw) && PatchCall(k_fx_calls[1], EffectsDraw);
    for (i = 0; i < 4; i++)
        ok &= PatchJmp(k_width_reads[i], TextWidthStub, 6) && WritePatch(k_width_reads[i], "\xE8", 1);
    Log(ok ? "aspect ratio installed" : "aspect ratio install FAILED");
}
