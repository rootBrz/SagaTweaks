/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "common.h"
#include "game.h"

extern double g_force_aspect;   /* ForceAspect: debug, lay out for this ratio on any screen */
int AspectReady(void);
void AspectInstall(void);
void AspectFrame(void);
double AspectValue(void);   /* width / height the game lays out for, whatever the Aspect row says */
