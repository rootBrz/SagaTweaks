/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "common.h"
#include "game.h"

extern double g_scales[MAX_RENDER_SCALES];
extern int g_scale_count;
extern double g_render_scale;
extern int g_trace_rt;
extern int g_skip_pass;
extern int g_skip_scaled;
extern int g_trace_passes;
int RenderReady(void);
void RenderInstall(void);
void RenderFrame(int cur, int titles);
extern int g_probe_consts;
