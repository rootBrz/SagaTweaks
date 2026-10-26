/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "common.h"
#include "game.h"

extern int g_max_fps;
extern int g_refresh_override;
extern int g_fix_counters;
extern int g_fix_physics;
extern int g_log_fps;
extern int g_probe_sweep;
extern int g_probe_hero;
extern int g_fix_gameplay;
extern int g_logic_hz;
extern int g_fix_contact;
extern int g_live_ini;
void ApplyMaxFps(void);
int FramerateReady(void);
void FramerateInstall(void);
void FramerateFrame(void);
void FramerateGameFrame(int game, int *out);
