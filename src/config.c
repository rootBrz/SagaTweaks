/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * SagaTweaks.ini loading.
 */
#include "all.h"

/* Reads SagaTweaks.ini. Every key is optional. */
void ConfigLoad(const char *ini)
{
    char buf[32];
    int all;

#define INI_INT(var, key, def) var = GetPrivateProfileIntA("SagaTweaks", key, def, ini)
    INI_INT(g_skip_logos, "SkipLogos", 1);
    INI_INT(g_skip_title, "SkipTitle", 1);
    INI_INT(g_skip_cutscenes, "SkipCutscenes", 1);
    INI_INT(g_hot_switch, "HotSwitchInput", 1);
    INI_INT(g_start_button, "StartButton", 7);
    INI_INT(g_max_fps, "MaxFPS", 0);
    INI_INT(g_vsync, "VSync", 1);
    INI_INT(g_window_mode, "WindowMode", WINDOW_FULLSCREEN);
    if (g_window_mode < 0 || g_window_mode >= WINDOW_MODES)
        g_window_mode = WINDOW_FULLSCREEN;
    INI_INT(g_refresh_override, "RefreshRate", 0);
    INI_INT(g_fix_counters, "FixFrameCounters", 1);
    INI_INT(g_fix_physics, "FixJumpPhysics", 1);
    INI_INT(g_logic_hz, "LogicHz", 0);
    INI_INT(g_fix_contact, "FixContactVelocity", 1);
    INI_INT(g_fix_gameplay, "FixGameplayRates", 1);
    INI_INT(g_prompt_glyphs, "PromptGlyphs", 1);
    all = GetPrivateProfileIntA("SagaTweaks", "ButtonPrompts", 0, ini);
    g_prompt_style[0] = GetPrivateProfileIntA("SagaTweaks", "ButtonPromptsP1", all, ini);
    g_prompt_style[1] = GetPrivateProfileIntA("SagaTweaks", "ButtonPromptsP2", all, ini);
    if (g_prompt_style[0] < 0 || g_prompt_style[0] > 2)     /* indexes the name table of the Tweaks page */
        g_prompt_style[0] = 0;
    if (g_prompt_style[1] < 0 || g_prompt_style[1] > 2)
        g_prompt_style[1] = 0;
    if (g_max_fps < 0)
        g_max_fps = 0;
    if (g_refresh_override < 0)
        g_refresh_override = 0;

    GetPrivateProfileStringA("SagaTweaks", "RenderScale", "1.0", buf, sizeof buf, ini);
    g_bb_scale = atof(buf);
    if (g_bb_scale < 0.25)
        g_bb_scale = 1.0;
    else if (g_bb_scale > MAX_RENDER_SCALE)
        g_bb_scale = MAX_RENDER_SCALE;
    if (g_bb_scale != 1.0 && g_window_mode == WINDOW_FULLSCREEN)   /* a scaled backbuffer needs a windowed device */
        g_window_mode = WINDOW_BORDERLESS;

    /* debugging aids */
    INI_INT(g_log_fps, "LogFPS", 0);
    INI_INT(g_log_input, "LogInput", 0);
    INI_INT(g_probe_sweep, "ProbeSweep", 0);
    INI_INT(g_probe_hero, "ProbeHero", 0);
    INI_INT(g_probe_buttons, "ProbeButtons", 0);
    INI_INT(g_probe_lever, "ProbeLever", 0);
    INI_INT(g_probe_anim, "ProbeAnim", 0);
    INI_INT(g_live_ini, "LiveIni", 0);
    INI_INT(g_probe_consts, "ProbeConsts", 0);
    INI_INT(g_probe_draw, "ProbeDraw", 0);
    INI_INT(g_fix_title_aspect, "FixTitleAspect", 1);
    INI_INT(g_fix_aspect, "FixAspect", 1);
    INI_INT(g_probe_effects, "ProbeEffects", 0);
    GetPrivateProfileStringA("SagaTweaks", "ForceAspect", "0", buf, sizeof buf, ini);
    g_force_aspect = atof(buf);
    if (g_force_aspect < 1.0 || g_force_aspect > 8.0)
        g_force_aspect = 0.0;
    INI_INT(g_probe_draw_x, "ProbeDrawX", 0);
    INI_INT(g_probe_draw_z, "ProbeDrawZ", 0);
    INI_INT(g_probe_draw_r, "ProbeDrawR", 0);
    INI_INT(g_dump_textures, "DumpTextures", 0);
#undef INI_INT

    {
        char line[256];

        snprintf(line, sizeof line,
                 "ini: MaxFPS=%d LogicHz=%d VSync=%d FixFrameCounters=%d FixJumpPhysics=%d FixContactVelocity=%d "
                 "FixGameplayRates=%d RenderScale=%.2f WindowMode=%d",
                 g_max_fps, g_logic_hz, g_vsync, g_fix_counters, g_fix_physics, g_fix_contact, g_fix_gameplay,
                 g_bb_scale, g_window_mode);
        Log(line);
    }
}
