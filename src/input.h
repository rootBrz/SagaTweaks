/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "common.h"
#include "game.h"

extern int g_hot_switch;
extern int g_start_button;
extern int g_log_input;
void HotSwitchInput(void);
int InputReady(void);
void InputInstall(void);
void InputFrame(int game);
