/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "common.h"
#include "game.h"

int ModsReady(void);
void ModsInstall(void);
int ModsCount(void);
const char *ModsName(int index);
int ModsMove(int index, int step);
