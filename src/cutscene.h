/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "common.h"
#include "game.h"

extern int g_skip_cutscenes;
void CutsceneFrame(int game, int *out, int cur, int titles);
