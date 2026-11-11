/* SPDX-License-Identifier: GPL-3.0-only */
/* Include the implementation to exercise its internal path resolver without game code. */
#include <assert.h>
#include "../src/mods.c"

FILE *g_log;

void Log(const char *msg) { (void)msg; }
int Matches(uintptr_t addr, const void *bytes, size_t len)
{
    (void)addr; (void)bytes; (void)len;
    return 1;
}
int PatchJmp(uintptr_t addr, const void *target, size_t len)
{
    (void)addr; (void)target; (void)len;
    return 0;
}
void *MakeTrampoline(const void *displaced, size_t len, uintptr_t resume)
{
    (void)displaced; (void)len; (void)resume;
    return NULL;
}

static char g_opened[256];
static int g_context;

static int __cdecl CaptureOpen(const char *name, int mode, int context, int flags)
{
    (void)mode; (void)flags;
    strcpy(g_opened, name);
    g_context = context;
    return 17;
}

static void Join(char out[MAX_PATH], const char *base, const char *child)
{
    assert(strlen(base) + strlen(child) + 2 <= MAX_PATH);
    strcpy(out, base);
    strcat(out, "\\");
    strcat(out, child);
}

static void MakeDir(const char *base, const char *child)
{
    char path[MAX_PATH];

    Join(path, base, child);
    assert(CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
}

static void MakeFile(const char *base, const char *child)
{
    char path[MAX_PATH];
    HANDLE file;

    Join(path, base, child);
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    CloseHandle(file);
}

int main(void)
{
    char temp[MAX_PATH], base[MAX_PATH], priority_path[MAX_PATH], out[256], saved[1024];
    const char *name = "CHARS\\CLONE.TXT";

    assert(GetTempPathA(sizeof temp, temp));
    assert(GetTempFileNameA(temp, "saga", 0, base));
    assert(DeleteFileA(base));
    assert(CreateDirectoryA(base, NULL));
    Join(g_mods_dir, base, "mods");
    InitializeCriticalSection(&g_mods_lock);
    MakeDir(base, "mods");
    MakeDir(g_mods_dir, "Z Mod");
    MakeDir(g_mods_dir, "A Mod");
    MakeDir(g_mods_dir, "A Mod\\CHARS");
    MakeDir(g_mods_dir, "Z Mod\\CHARS");
    MakeFile(g_mods_dir, "A Mod\\CHARS\\CLONE.TXT");
    MakeFile(g_mods_dir, "Z Mod\\CHARS\\CLONE.TXT");
    ModsRefresh();

    assert(ModsCount() == 2);
    assert(!strcmp(ModsName(0), "A Mod"));
    assert(FindOverride(name, out));
    assert(strstr(out, "A Mod"));
    assert(ModsMove(1, -1) == 0);
    assert(!strcmp(ModsName(0), "Z Mod"));
    assert(FindOverride(name, out));
    assert(strstr(out, "Z Mod"));
    Join(priority_path, g_mods_dir, "priority.ini");
    {
        FILE *priority = fopen(priority_path, "r");

        assert(priority);
        assert(fgets(saved, sizeof saved, priority));
        assert(!strcmp(saved, "Z Mod\n"));
        assert(fgets(saved, sizeof saved, priority));
        assert(!strcmp(saved, "A Mod\n"));
        assert(!fgets(saved, sizeof saved, priority));
        fclose(priority);
    }
    ModsRefresh();
    assert(!strcmp(ModsName(0), "Z Mod"));
    assert(ModsMove(1, -1) == 0);
    assert(!strcmp(ModsName(0), "A Mod"));

    MakeDir(g_mods_dir, "CHARS");
    MakeFile(g_mods_dir, name);
    MakeFile(g_mods_dir, "CHARS\\ROOTONLY.TXT");
    assert(FindOverride(name, out));
    assert(strstr(out, "A Mod"));
    assert(!FindOverride("CHARS\\ROOTONLY.TXT", out));

    g_file_open = CaptureOpen;
    assert(FileOpenHook(name, 0, 73, 0) == 17);
    assert(g_context == 0);
    assert(!strcmp(g_opened, out));
    assert(FileOpenHook(name, 1, 73, 0) == 17);
    assert(g_context == 73);
    assert(!strcmp(g_opened, name));
    assert(FileOpenHook("CHARS\\MISSING.TXT", 0, 73, 0) == 17);
    assert(!strcmp(g_opened, "CHARS\\MISSING.TXT"));
    return 0;
}
