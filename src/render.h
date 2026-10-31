/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "common.h"
#include "game.h"

int RenderReady(void);
void RenderInstall(void);
void RenderFrame(int cur, int titles);
extern int g_probe_consts;
extern int g_probe_draw;
extern int g_fix_title_aspect;
extern int g_probe_draw_x, g_probe_draw_z, g_probe_draw_r;
