/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "common.h"
#include "game.h"

extern int g_dump_textures;
extern IDirect3DTexture9 *g_tex_list[MAX_DUMP_TEX];
extern int g_tex_count;
void DecodeDxt(const uint8_t *b, int kind, uint8_t *px);
void WriteTga(const char *path, int w, int h, const uint8_t *rgba);
void DumpTextures(void);
