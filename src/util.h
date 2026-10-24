#pragma once
#include "common.h"
#include "game.h"

extern FILE *g_log;
extern char g_ini_path[MAX_PATH];
void Log(const char *msg);
int WritePatch(uintptr_t addr, const void *bytes, size_t len);
void PatchVtableEntry(void **vtable, int index, void *hook, void **orig);
int Matches(uintptr_t addr, const void *bytes, size_t len);
int PatchCall(uintptr_t addr, const void *target);
int PatchJmp(uintptr_t addr, const void *target, size_t len);
int PatchPtr(uintptr_t addr, const void *value);
void *MakeTrampoline(const void *displaced, size_t len, uintptr_t resume);
