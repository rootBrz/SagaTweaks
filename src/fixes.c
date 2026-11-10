/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Fixes for bugs the game has at any frame rate.
 */
#include "all.h"

int g_fix_hover_attack = 1;

/*
 * Astromech droids (type flag 0x40: R2-D2, R4-P17, R2-Q5) jump by hovering. The jump (sub_562570, mode 4) sets the hover phase,
 * byte +3634, to 1 = rising; the gravity code sub_4ABAC0 moves it on to 2 = hovering at the top of the rise, but only while jump is
 * still held, and from 2 to 3 = falling. A jump released before the top (or ended by a ledge) lands with the phase still 1, and the
 * zap sub_4A6160 takes phase 1 as "in the air": it then wants the air attack animation 60 instead of the zap, so action and special
 * did nothing until a full hover moved the phase on. A droid that stands on the ground outside the jump state gets the phase a
 * finished hover leaves. Found by reading the code; not yet tried in the game.
 */
#define ADDR_CHAR_COUNT  0x0093D500u   /* dword_93D500: characters in the level */
#define ADDR_CHAR_ARRAY  0x0093D504u   /* dword_93D504: their objects, CHAR_SIZE bytes each */
#define CHAR_SIZE        4312
#define CHAR_TYPE_OFS    84            /* type record, +4 = flags */
#define TYPE_HOVER_JUMP  0x40
#define CHAR_GROUND_OFS  641           /* not 0: standing on something */
#define CHAR_STATE_OFS   1973          /* 0 = jumping (dword_802838) */
#define CHAR_HOVER_OFS   3634
#define HOVER_RISING     1
#define HOVER_FALLING    3

static void HoverAttackFix(void)
{
    char *c = *(char **)ADDR_CHAR_ARRAY;
    int n = *(const int *)ADDR_CHAR_COUNT, i;

    for (i = 0; c && i < n; i++, c += CHAR_SIZE) {
        const char *type = *(const char **)(c + CHAR_TYPE_OFS);

        if (type && (type[4] & TYPE_HOVER_JUMP) && c[CHAR_HOVER_OFS] == HOVER_RISING && c[CHAR_GROUND_OFS] && c[CHAR_STATE_OFS] != 0)
            c[CHAR_HOVER_OFS] = HOVER_FALLING;
    }
}

void FixesFrame(void)
{
    if (g_fix_hover_attack)
        HoverAttackFix();
}
