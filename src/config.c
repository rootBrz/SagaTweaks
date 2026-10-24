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
    INI_INT(g_refresh_override, "RefreshRate", 0);
    INI_INT(g_fix_counters, "FixFrameCounters", 1);
    INI_INT(g_fix_physics, "FixJumpPhysics", 1);
    INI_INT(g_prompt_glyphs, "PromptGlyphs", 1);
    all = GetPrivateProfileIntA("SagaTweaks", "ButtonPrompts", 0, ini);
    g_prompt_style[0] = GetPrivateProfileIntA("SagaTweaks", "ButtonPromptsP1", all, ini);
    g_prompt_style[1] = GetPrivateProfileIntA("SagaTweaks", "ButtonPromptsP2", all, ini);

    GetPrivateProfileStringA("SagaTweaks", "RenderScale", "1.0", buf, sizeof buf, ini);
    g_render_scale = atof(buf);
    if (g_render_scale < 0.25 || g_render_scale > 4.0)
        g_render_scale = 1.0;
    {
        char list[128], *tok, *save;

        GetPrivateProfileStringA("SagaTweaks", "RenderScales", "", list, sizeof list, ini);
        for (tok = strtok_s(list, ", ", &save); tok && g_scale_count < MAX_RENDER_SCALES;
             tok = strtok_s(NULL, ", ", &save)) {
            double v = atof(tok);

            if (v > 1.0 && v <= 4.0)
                g_scales[g_scale_count++] = v;
        }
    }

    /* debugging aids */
    INI_INT(g_log_fps, "LogFPS", 0);
    INI_INT(g_trace_rt, "TraceRT", 0);
    INI_INT(g_dump_textures, "DumpTextures", 0);
    INI_INT(g_skip_pass, "SkipPass", -1);
    INI_INT(g_trace_passes, "TracePasses", 0);
    INI_INT(g_skip_scaled, "SkipScaledPass", -1);
#undef INI_INT
}
