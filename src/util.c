/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Shared helpers: log, memory patching, call/jmp/pointer patches, trampolines, vtable hooks.
 */
#include "all.h"

FILE *g_log;

char g_ini_path[MAX_PATH];

void Log(const char *msg)
{
    if (g_log) {
        fprintf(g_log, "%s\n", msg);
        fflush(g_log);
    }
}

DWORD TickNonZero(void)
{
    DWORD t = GetTickCount();

    return t ? t : 1;
}

int WritePatch(uintptr_t addr, const void *bytes, size_t len)
{
    DWORD old;

    if (!VirtualProtect((void *)addr, len, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy((void *)addr, bytes, len);
    VirtualProtect((void *)addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)addr, len);
    return 1;
}

void PatchVtableEntry(void **vtable, int index, void *hook, void **orig)
{
    DWORD old;

    if (!VirtualProtect(&vtable[index], sizeof(void *), PAGE_READWRITE, &old))
        return;
    *orig = vtable[index];
    vtable[index] = hook;
    VirtualProtect(&vtable[index], sizeof(void *), old, &old);
}

int Matches(uintptr_t addr, const void *bytes, size_t len)
{
    return memcmp((const void *)addr, bytes, len) == 0;
}
/* Overwrite the code at addr with a rel32 call to target. */
int PatchCall(uintptr_t addr, const void *target)
{
    uint8_t b[5] = { 0xE8 };
    int32_t rel = (int32_t)((uintptr_t)target - (addr + 5));

    memcpy(b + 1, &rel, 4);
    return WritePatch(addr, b, sizeof b);
}

/* Overwrite the code at addr with a rel32 jmp to target, padding with nops up to len bytes. */
int PatchJmp(uintptr_t addr, const void *target, size_t len)
{
    uint8_t b[16] = { 0xE9 };
    int32_t rel = (int32_t)((uintptr_t)target - (addr + 5));

    memcpy(b + 1, &rel, 4);
    memset(b + 5, 0x90, len - 5);
    return WritePatch(addr, b, len);
}

/* Overwrite a 32-bit pointer / operand. */
int PatchPtr(uintptr_t addr, const void *value)
{
    uint32_t v = (uint32_t)(uintptr_t)value;

    return WritePatch(addr, &v, 4);
}

/* A trampoline holding the displaced bytes of a hooked function followed by a jmp back. */
void *MakeTrampoline(const void *displaced, size_t len, uintptr_t resume)
{
    uint8_t *t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    int32_t rel;

    if (!t)
        return NULL;
    memcpy(t, displaced, len);
    t[len] = 0xE9;
    rel = (int32_t)(resume - ((uintptr_t)t + len + 5));
    memcpy(t + len + 1, &rel, 4);
    return t;
}
