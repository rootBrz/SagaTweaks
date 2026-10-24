/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Startup skips: boot logos and the "Press any button" title screen.
 */
#include "all.h"

static int g_title_frames;

int g_skip_logos = 1;

int g_skip_title = 1;
static const uint8_t k_logo_branch_orig[6] = { 0x0F, 0x84, 0x52, 0x07, 0x00, 0x00 };

int StartupReady(void)
{
    return Matches(ADDR_LOGO_BRANCH, k_logo_branch_orig, sizeof k_logo_branch_orig);
}

/* Boot logos: the sequence already has a skip branch; make it unconditional (jz rel32 -> jmp rel32 + nop). */
void StartupInstall(void)
{
    if (!g_skip_logos)
        return;
    Log(PatchJmp(ADDR_LOGO_BRANCH, (void *)ADDR_LOGO_SKIP_TGT, 6) ? "logo skip patched" : "logo skip patch FAILED");
}

/* Title screen: once the titles level shows only "press any button", press it for the player. */
void StartupFrame(int cur, int titles)
{
    if (g_skip_title && cur && cur == titles) {
        if (*(int *)ADDR_MENU_DEPTH == 1 && ++g_title_frames >= TITLE_SETTLE_FRAMES) {
            ((OpenMenuFn)ADDR_OPEN_MENU)(MENU_MAIN, *(int *)ADDR_MENU_FLAG != 0, -1);
            *(char *)ADDR_PLAYER_ACTIVE = 1; /* options only draw once player 1 counts as having pressed a button */
            if (*(float *)ADDR_TITLE_TIME < TITLE_ANIM_END)
                *(float *)ADDR_TITLE_TIME = TITLE_ANIM_END; /* no logo animation / option fade-in wait */
            g_title_frames = 0;
            Log("press-any-button screen reached, opened main menu");
        }
    } else {
        g_title_frames = 0;
    }
}
