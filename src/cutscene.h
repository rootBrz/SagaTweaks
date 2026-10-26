/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "common.h"
#include "game.h"

extern int g_skip_cutscenes;
extern int g_trace_skip;
extern DWORD g_skip_first_tick;
extern int g_cutscene_skipping;
extern float g_skip_fade;   /* 0 clear .. 1 black, drawn over the frame by the Present hook */
void CutsceneFrame(int game, int *out, int cur, int titles);
