/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Loose-file overrides. For reads, try mods\<mod>\<path> in the configured priority order
 * (A–Z by default). Only immediate mod folders are searched, so optional variants nested inside a
 * mod stay optional. Besides the central open, hook the whole-file reader,
 * size query and WAV loader: their archive fast paths bypass the central open.
 */
#include "all.h"

typedef int (__cdecl *FileOpenFn)(const char *name, int mode, int context, int flags);
typedef int (__cdecl *FileReadFn)(const char *name, char *out, int capacity);
typedef int (__cdecl *FileSizeFn)(const char *name);
typedef int (__attribute__((thiscall)) *WavOpenFn)(void *self, int archive, const char *name, int format);

static const uint8_t k_file_open_orig[6] = { 0x81, 0xEC, 0x04, 0x01, 0x00, 0x00 };
static const uint8_t k_file_read_orig[5] = { 0xA1, 0x88, 0xEE, 0x91, 0x00 };
static const uint8_t k_file_size_orig[6] = { 0x51, 0xA1, 0x90, 0xEE, 0x91, 0x00 };
static const uint8_t k_wav_open_orig[8] = { 0x83, 0xEC, 0x20, 0x53, 0x8B, 0x5C, 0x24, 0x28 };
static FileOpenFn g_file_open;
static FileReadFn g_file_read;
static FileSizeFn g_file_size;
static WavOpenFn g_wav_open;
static char g_mods_dir[MAX_PATH];
static char (*g_mod_names)[MAX_PATH];
static int g_mod_count;
static CRITICAL_SECTION g_mods_lock;

static int IsFile(const char *path)
{
    DWORD attr = GetFileAttributesA(path);

    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static int ModPath(char out[256], const char *dir, const char *name)
{
    int n = snprintf(out, 256, "%s\\%s", dir, name);

    return n >= 0 && n < 256 && IsFile(out);
}

static int ModNameIndex(const char *name)
{
    int i;

    for (i = 0; i < g_mod_count; i++) {
        if (!_stricmp(g_mod_names[i], name))
            return i;
    }
    return -1;
}

static int ModNameCompare(const void *a, const void *b)
{
    return _stricmp((const char *)a, (const char *)b);
}

static void ModsSavePriority(void)
{
    size_t size = (size_t)g_mod_count * MAX_PATH + 1;
    char *priority = malloc(size);
    int i;

    if (!priority) {
        Log("mod priority save FAILED");
        return;
    }
    priority[0] = 0;
    for (i = 0; i < g_mod_count; i++) {
        if (i)
            strcat(priority, "|");
        strcat(priority, g_mod_names[i]);
    }
    WritePrivateProfileStringA("SagaTweaks", "ModPriority", priority, g_ini_path);
    free(priority);
}

static void ModsReadPriority(void)
{
    size_t size = (size_t)g_mod_count * MAX_PATH + 1;
    char *priority = malloc(size), *token;
    int pos = 0;

    if (!priority)
        return;
    GetPrivateProfileStringA("SagaTweaks", "ModPriority", "", priority, (DWORD)size, g_ini_path);
    token = strtok(priority, "|");
    while (token) {
        int i = ModNameIndex(token);

        if (i >= pos) {
            char tmp[MAX_PATH];

            strcpy(tmp, g_mod_names[pos]);
            memmove(g_mod_names[pos], g_mod_names[i], MAX_PATH);
            memmove(g_mod_names[i], tmp, MAX_PATH);
            pos++;
        }
        token = strtok(NULL, "|");
    }
    free(priority);
}

static void ModsRefresh(void)
{
    WIN32_FIND_DATAA data;
    char pattern[MAX_PATH];
    char (*names)[MAX_PATH] = NULL;
    HANDLE find;
    int count = 0, n = 0;

    n = snprintf(pattern, sizeof pattern, "%s\\*", g_mods_dir);
    if (n < 0 || n >= (int)sizeof pattern)
        return;
    find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        free(g_mod_names);
        g_mod_names = NULL;
        g_mod_count = 0;
        return;
    }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && strcmp(data.cFileName, ".") && strcmp(data.cFileName, ".."))
            count++;
    } while (FindNextFileA(find, &data));
    FindClose(find);
    if (count) {
        names = malloc((size_t)count * MAX_PATH);
        if (!names) {
            Log("mod folder scan FAILED");
            return;
        }
    }
    find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        free(names);
        return;
    }
    n = 0;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && strcmp(data.cFileName, ".") && strcmp(data.cFileName, "..") &&
            n < count)
            strcpy(names[n++], data.cFileName);
    } while (FindNextFileA(find, &data));
    FindClose(find);
    count = n;
    if (count > 1)
        qsort(names, count, MAX_PATH, ModNameCompare);
    free(g_mod_names);
    g_mod_names = names;
    g_mod_count = count;
    ModsReadPriority();
}

int ModsCount(void)
{
    return g_mod_count;
}

const char *ModsName(int index)
{
    return index >= 0 && index < g_mod_count ? g_mod_names[index] : "";
}

int ModsMove(int index, int step)
{
    int target = index + step;
    char tmp[MAX_PATH];

    if (index < 0 || index >= g_mod_count || target < 0 || target >= g_mod_count || !step)
        return index;
    EnterCriticalSection(&g_mods_lock);
    strcpy(tmp, g_mod_names[index]);
    memmove(g_mod_names[index], g_mod_names[target], MAX_PATH);
    memmove(g_mod_names[target], tmp, MAX_PATH);
    ModsSavePriority();
    LeaveCriticalSection(&g_mods_lock);
    return target;
}

static int FindOverride(const char *name, char out[256])
{
    char dir[MAX_PATH], candidate[256];
    int i, found = 0;

    EnterCriticalSection(&g_mods_lock);
    for (i = 0; i < g_mod_count; i++) {
        int n = snprintf(dir, sizeof dir, "%s\\%s", g_mods_dir, g_mod_names[i]);

        if (n >= 0 && n < (int)sizeof dir && ModPath(candidate, dir, name)) {
            strcpy(out, candidate);
            found = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_mods_lock);
    return found;
}

static int OverridePath(const char *name, char path[256])
{
    char line[640];

    if (!FindOverride(name, path))
        return 0;
    snprintf(line, sizeof line, "mod override: %s -> %s", name, path);
    Log(line);
    return 1;
}

static int __cdecl FileOpenHook(const char *name, int mode, int context, int flags)
{
    char path[256];

    if (mode == 0 && name && OverridePath(name, path))
        return g_file_open(path, mode, 0, flags);
    return g_file_open(name, mode, context, flags);
}

static int __cdecl FileReadHook(const char *name, char *out, int capacity)
{
    char path[256];

    if (name && OverridePath(name, path))
        return g_file_read(path, out, capacity);
    return g_file_read(name, out, capacity);
}

static int __cdecl FileSizeHook(const char *name)
{
    char path[256];

    if (name && OverridePath(name, path))
        return g_file_size(path);
    return g_file_size(name);
}

static int __attribute__((thiscall)) WavOpenHook(void *self, int archive, const char *name, int format)
{
    char path[256];

    if (name && OverridePath(name, path))
        return g_wav_open(self, 0, path, format);
    return g_wav_open(self, archive, name, format);
}

int ModsReady(void)
{
    return Matches(ADDR_FILE_OPEN, k_file_open_orig, sizeof k_file_open_orig) &&
           Matches(ADDR_FILE_READ, k_file_read_orig, sizeof k_file_read_orig) &&
           Matches(ADDR_FILE_SIZE, k_file_size_orig, sizeof k_file_size_orig) &&
           Matches(ADDR_WAV_OPEN, k_wav_open_orig, sizeof k_wav_open_orig);
}

void ModsInstall(void)
{
    char path[MAX_PATH];
    char *slash;

    if (!GetModuleFileNameA(NULL, path, sizeof path) || !(slash = strrchr(path, '\\'))) {
        Log("mod overrides FAILED: game path unavailable");
        return;
    }
    strcpy(slash + 1, "mods");
    strcpy(g_mods_dir, path);
    InitializeCriticalSection(&g_mods_lock);
    ModsRefresh();
    g_file_open = MakeTrampoline((const void *)ADDR_FILE_OPEN, sizeof k_file_open_orig,
                                 ADDR_FILE_OPEN + sizeof k_file_open_orig);
    g_file_read = MakeTrampoline((const void *)ADDR_FILE_READ, sizeof k_file_read_orig,
                                 ADDR_FILE_READ + sizeof k_file_read_orig);
    g_file_size = MakeTrampoline((const void *)ADDR_FILE_SIZE, sizeof k_file_size_orig,
                                 ADDR_FILE_SIZE + sizeof k_file_size_orig);
    g_wav_open = MakeTrampoline((const void *)ADDR_WAV_OPEN, sizeof k_wav_open_orig,
                                ADDR_WAV_OPEN + sizeof k_wav_open_orig);
    if (!g_file_open || !g_file_read || !g_file_size || !g_wav_open) {
        Log("mod overrides FAILED: trampoline allocation");
        return;
    }
    if (PatchJmp(ADDR_FILE_OPEN, FileOpenHook, sizeof k_file_open_orig) &&
        PatchJmp(ADDR_FILE_READ, FileReadHook, sizeof k_file_read_orig) &&
        PatchJmp(ADDR_FILE_SIZE, FileSizeHook, sizeof k_file_size_orig) &&
        PatchJmp(ADDR_WAV_OPEN, WavOpenHook, sizeof k_wav_open_orig))
        Log("mod file overrides installed");
    else
        Log("mod file overrides FAILED");
}
