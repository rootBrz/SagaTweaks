/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "common.h"
#include "game.h"

int RenderReady(void);
void RenderInstall(void);
void RenderFrame(int cur, int titles);
extern int g_probe_consts;
