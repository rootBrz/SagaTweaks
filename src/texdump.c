/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Debug aid: dumps the game's textures / render targets to TGA files (DumpTextures=1).
 */
#include "all.h"

   /* 256x128 DXT5 atlas with the Xbox glyphs */
int g_dump_textures;
IDirect3DTexture9 *g_tex_list[MAX_DUMP_TEX];

int g_tex_count;

static void Rgb565(uint16_t c, uint8_t *o)
{
    o[0] = (uint8_t)((c >> 11 & 31) * 255 / 31);
    o[1] = (uint8_t)((c >> 5 & 63) * 255 / 63);
    o[2] = (uint8_t)((c & 31) * 255 / 31);
}

void DecodeDxt(const uint8_t *b, int kind, uint8_t *px /* 16 * rgba */)
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

void WriteTga(const char *path, int w, int h, const uint8_t *rgba)
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

void DumpTextures(void)
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
