/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Button prompts: Xbox / PlayStation glyph atlas, font patching and per-player style selection.
 */
#include "all.h"

IDirect3DTexture9 *g_buttons_tex;

SetTextureFn g_orig_set_texture;

static IDirect3DTexture9 *g_combined_tex; /* 256x256: Xbox glyphs on top, PlayStation glyphs below */
static int g_atlas_failed;

static int g_atlas_ready; /* font patched, combined atlas in use */
int g_prompt_glyphs = 1; /* PromptGlyphs=0 turns the whole feature off */
int g_prompt_style[2]; /* per player: 0 auto, 1 Xbox, 2 PlayStation */
int g_label_device; /* device the button label being built is for */
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

HRESULT STDMETHODCALLTYPE HookSetTexture(IDirect3DDevice9 *dev, DWORD stage, IDirect3DBaseTexture9 *tex)
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
    static DWORD last;
    IDirect3DDevice9 *dev = *(IDirect3DDevice9 **)(ADDR_DEVICE_OBJECT + 4);

    if (!last || GetTickCount() - last >= 2000) {     /* every 2 s, not every 120 frames (8 times a second at 1000 fps) */
        int i;

        last = GetTickCount() | 1;

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
int PromptsReady(void)
{
    return Matches(ADDR_LABEL_BUILDER, (const uint8_t[]){ 0x83, 0xEC, 0x2C, 0xA1, 0x70, 0x95, 0x82, 0x00 }, 8);
}

void PromptsInstall(void)
{
    Log(PatchJmp(ADDR_LABEL_BUILDER, LabelBuilderHook, 8) ? "button label hook installed" : "button label hook FAILED");
}

void PromptsFrame(void)
{
    UpdatePromptStyle();
}
