/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "common.h"
#include "game.h"

extern int g_skip_logos;
extern int g_skip_title;
int StartupReady(void);
void StartupInstall(void);
void StartupFrame(int cur, int titles);
