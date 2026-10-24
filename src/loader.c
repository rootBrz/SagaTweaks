/*
 * Minimal ASI loader. Built as dinput8.dll: forwards DirectInput8Create to the
 * real system dinput8 and loads every *.asi found in the game directory (and
 * in .\scripts and .\plugins) when the process starts.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>

typedef HRESULT (WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);

static HMODULE g_real;
static DirectInput8Create_t g_create;

static int LoadReal(void)
{
    char path[MAX_PATH];

    if (g_create)
        return 1;
    GetSystemDirectoryA(path, MAX_PATH);
    lstrcatA(path, "\\dinput8.dll");
    g_real = LoadLibraryA(path);
    if (!g_real)
        return 0;
    g_create = (DirectInput8Create_t)GetProcAddress(g_real, "DirectInput8Create");
    return g_create != NULL;
}

HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID *out, LPUNKNOWN outer)
{
    if (!LoadReal())
        return E_FAIL;
    return g_create(inst, ver, riid, out, outer);
}

static void LoadAsiFrom(const char *dir)
{
    char pattern[MAX_PATH], full[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(pattern, sizeof pattern, "%s*.asi", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        snprintf(full, sizeof full, "%s%s", dir, fd.cFileName);
        LoadLibraryA(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void LoadAllAsi(void)
{
    char root[MAX_PATH], sub[MAX_PATH];
    char *slash;

    if (!GetModuleFileNameA(NULL, root, MAX_PATH))
        return;
    slash = strrchr(root, '\\');
    if (!slash)
        return;
    slash[1] = '\0';

    LoadAsiFrom(root);
    snprintf(sub, sizeof sub, "%sscripts\\", root);
    LoadAsiFrom(sub);
    snprintf(sub, sizeof sub, "%splugins\\", root);
    LoadAsiFrom(sub);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        LoadAllAsi();
    }
    return TRUE;
}
