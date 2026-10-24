#pragma once
#include "common.h"
#include "game.h"

extern int g_skip_cutscenes;
extern int g_trace_skip;
extern DWORD g_skip_first_tick;
extern int g_cutscene_skipping;
void CutsceneFrame(int game, int *out, int cur, int titles);
