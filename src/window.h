/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "common.h"
#include "game.h"

enum { WINDOW_FULLSCREEN, WINDOW_WINDOWED, WINDOW_BORDERLESS, WINDOW_MODES };

extern int g_vsync;
extern double g_bb_scale;    /* render scale: the backbuffer is the video mode's size times this */
extern int g_window_mode;
extern const char *const g_window_mode_names[WINDOW_MODES];
int WindowReady(void);
void WindowInstall(void);
void WindowFrame(void);
void WindowApply(void);
double RenderScaleValue(void);
void RenderScaleSet(double v);
int WindowModeStep(int mode, int step);
