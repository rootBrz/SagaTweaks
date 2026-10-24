#pragma once
#include "common.h"
#include "game.h"

extern int g_max_fps;
extern int g_refresh_override;
extern int g_fix_counters;
extern int g_fix_physics;
extern int g_log_fps;
void ApplyMaxFps(void);
int FramerateReady(void);
void FramerateInstall(void);
void FramerateFrame(void);
