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
 */
#include "all.h"

int g_probe_effects;   /* ProbeEffects: log the flying sprite effects (picked-up studs) every frame */
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

void AspectFrame(void)
{
    static const int screen = 3;

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
        ((double (__cdecl *)(float))ADDR_FOV_FN)(user);
        fov = wide ? (float)(user * (fv / 0.75)) : user;
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
void __cdecl TextAnchor(float *x)
{
    if (g_fix_aspect && *(const uint8_t *)ADDR_WIDE)
        *x = (float)(*x * (16.0 / 9.0) / AspectValue());
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
    static float done_x[128], done_s[128];   /* the values written back for each record, 0 = not scaled yet */
    double k = AspectValue() / (16.0 / 9.0);
    int i;

    for (i = 0; i < 128; i++) {
        float *r = (float *)(0x00947BC0u + 276u * i);
        const uint8_t *b = (const uint8_t *)r;
        uint32_t flags = *(const uint32_t *)(b + 216);

        if (!b[248] || (flags & 0x20001) != 0x20001) {
            done_x[i] = done_s[i] = 0.0f;
            continue;
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
    }
    return TextReady() && Matches(ADDR_CAMERA_FN, k_camera_orig, sizeof k_camera_orig);
}

void AspectInstall(void)
{
    int i, ok = PatchJmp(ADDR_CAMERA_FN, CameraAspect, 5);

    ok &= PatchJmp(ADDR_TEXT_FN, TextHook, 10);
    ok &= PatchCall(k_fx_calls[0], EffectsDraw) && PatchCall(k_fx_calls[1], EffectsDraw);
    for (i = 0; i < 4; i++)
        ok &= PatchJmp(k_width_reads[i], TextWidthStub, 6) && WritePatch(k_width_reads[i], "\xE8", 1);
    Log(ok ? "aspect ratio installed" : "aspect ratio install FAILED");
}
