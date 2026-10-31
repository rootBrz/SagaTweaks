/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * SagaTweaks.asi - quality-of-life patches for LEGO Star Wars: The Complete Saga
 * (Steam build, LEGOStarWarsSaga.exe, 5,472,256 bytes).
 *
 *   startup.c    skip boot logos and the title screen
 *   cutscene.c   skip cutscenes with Start
 *   framerate.c  FPS cap, refresh rate, clock / jump-physics fixes
 *   window.c     VSync and window mode (fullscreen / windowed / borderless)
 *   aspect.c     camera, text and HUD for any aspect ratio; 21:9 and 32:9 in the Aspect row
 *   render.c     render scale (scaled targets, depth twin, virtual backbuffer), D3D9 hooks
 *   prompts.c    Xbox / PlayStation button glyphs, per player
 *   input.c      keyboard / gamepad hot-switching, Start-only joining, drop-out recovery
 *   menus.c      Render Scale row, Tweaks page, hidden FSAA row
 *   config.c     SagaTweaks.ini
 *   util.c       logging and patching primitives
 *   texdump.c    debug texture dumper
 *
 * The exe is SteamStub-packed, so .text is encrypted until the stub hands over to the real
 * entry point. The patch thread waits for the expected code bytes to show up before touching
 * anything. All addresses are for the unpacked image (base 0x400000, no ASLR).
 *
 * Entry point and per-frame orchestration: each module installs its own patches
 * (<Module>Ready checks the expected game bytes, <Module>Install patches) and gets a
 * <Module>Frame call from the hooked main-loop frame call.
 */
#include "all.h"

static const uint8_t k_frame_call_orig[5] = { 0xE8, 0x8C, 0xE8, 0xFF, 0xFF };

static void __cdecl FrameHook(int game, int *out)
{
    int cur = game ? *(int *)(game + GAME_CUR_LEVEL_OFS) : 0;
    int titles = *(int *)ADDR_TITLES_LEVEL;

    StartupFrame(cur, titles);
    MenusFrame();
    PromptsFrame();
    FramerateFrame();
    WindowFrame();
    AspectFrame();
    InputFrame(game);
    RenderFrame(cur, titles);
    CutsceneFrame(game, out, cur, titles);   /* runs the game's own frame (or the cutscene skip) */
}

#define PATCH_WAIT_TRIES 120000   /* ~2 minutes at 1 ms */

static DWORD WINAPI PatchThread(LPVOID param)
{
    int tries;

    (void)param; /* wait for SteamStub to decrypt .text: the expected bytes must be there */
    for (tries = 0; tries < PATCH_WAIT_TRIES; tries++) {
        if (StartupReady() && FramerateReady() && WindowReady() && AspectReady() && RenderReady() && MenusReady() && InputReady() &&
            PromptsReady() && Matches(ADDR_FRAME_CALL, k_frame_call_orig, sizeof k_frame_call_orig))
            break;
        Sleep(1);
    }
    if (tries == PATCH_WAIT_TRIES) {
        Log("expected code not found, not patching (different exe version?)");
        return 0;
    }

    StartupInstall();
    FramerateInstall();
    WindowInstall();
    AspectInstall();
    RenderInstall();
    MenusInstall();
    InputInstall();
    PromptsInstall();
    Log(PatchCall(ADDR_FRAME_CALL, FrameHook) ? "frame hook installed" : "frame hook FAILED");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH];
        char *slash;

        DisableThreadLibraryCalls(inst);
        if (GetModuleFileNameA(inst, path, MAX_PATH) && (slash = strrchr(path, '\\'))) {
            strcpy(slash + 1, "SagaTweaks.log");
            g_log = fopen(path, "w");
            Log("SagaTweaks " SAGA_VERSION);
            strcpy(slash + 1, "SagaTweaks.ini");
            strcpy(g_ini_path, path);
            ConfigLoad(g_ini_path);
        }
        CloseHandle(CreateThread(NULL, 0, PatchThread, NULL, 0, NULL));
    }
    return TRUE;
}
