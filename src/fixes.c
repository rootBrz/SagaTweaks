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

/*
 * Speed Blur could not be kept off: the start-up code (sub_6DBB30) calls sub_6DB060(1) right after it has read pcconfig.txt, which
 * sets SpeedBlurEnabled (settings + 1110, byte_8264C6) whatever the file said. The call (push 1; mov ecx, settings; call) is removed.
 * That code runs before the window exists and may be over by the time the patch lands, so the value is also read back from the
 * file, found the way sub_6DB990 finds it: the path at settings + 4, else pcconfig.txt in the directory at off_800478 + 172.
 */
#define ADDR_BLUR_FORCE   0x006DC0B3u
#define ADDR_BLUR_ENABLED 0x008264C6u
#define ADDR_CONFIG_PATH  0x00826074u   /* settings + 4 */
#define ADDR_USER_DIR_PTR 0x00800478u
#define USER_DIR_OFS      172
static const uint8_t k_blur_force_orig[12] = { 0x6A, 0x01, 0xB9, 0x70, 0x60, 0x82, 0x00, 0xE8, 0xA1, 0xEF, 0xFF, 0xFF };

static void RestoreBlur(void)
{
    const char *base = *(const char *const *)ADDR_USER_DIR_PTR;
    char path[MAX_PATH + 16], line[128];
    FILE *f = fopen((const char *)ADDR_CONFIG_PATH, "r");
    int value;

    if (!f && base && snprintf(path, sizeof path, "%.*spcconfig.txt", MAX_PATH, base + USER_DIR_OFS) > 0)
        f = fopen(path, "r");
    if (!f)
        return;   /* the game has not got to the file yet: the removed call is enough */
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "SpeedBlurEnabled %d", &value) == 1)
            *(uint8_t *)ADDR_BLUR_ENABLED = value > 0;
    fclose(f);
}

/*
 * Jawas made a menu sound every few seconds: their update (sub_4B3B80, also the type at word_7F1A30) ends with sub_547CB0(character,
 * 0, 0.0), the idle sound routine with sound 0 of the table, where the common update sub_4B6B80 passes the sound and the pause of
 * the character's type (type + 0x24: word +0xE8, -1 = none; byte +0x11E). The call now does what the common update does.
 */
#define ADDR_JAWA_IDLE_CALL 0x004B3D2Cu
#define ADDR_IDLE_SOUND_FN  0x00547CB0u   /* sub_547CB0(character, sound, pause) */
#define TYPE_DATA_OFS       0x24
#define TYPE_IDLE_SOUND_OFS 0xE8
#define TYPE_IDLE_PAUSE_OFS 0x11E
static const uint8_t k_jawa_idle_orig[5] = { 0xE8, 0x7F, 0x3F, 0x09, 0x00 };

static void __cdecl TypeIdleSound(char *c, int sound, float pause)
{
    const char *data = *(const char **)(*(const char **)(c + CHAR_TYPE_OFS) + TYPE_DATA_OFS);

    (void)sound;
    (void)pause;
    if (*(const int16_t *)(data + TYPE_IDLE_SOUND_OFS) != -1)
        ((void (__cdecl *)(char *, int, float))ADDR_IDLE_SOUND_FN)(c, *(const int16_t *)(data + TYPE_IDLE_SOUND_OFS),
                                                                    (float)*(const uint8_t *)(data + TYPE_IDLE_PAUSE_OFS));
}

int FixesReady(void)
{
    return Matches(ADDR_BLUR_FORCE, k_blur_force_orig, sizeof k_blur_force_orig) &&
           Matches(ADDR_JAWA_IDLE_CALL, k_jawa_idle_orig, sizeof k_jawa_idle_orig);
}

void FixesInstall(void)
{
    static const uint8_t nops[sizeof k_blur_force_orig] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    int ok = WritePatch(ADDR_BLUR_FORCE, nops, sizeof nops);

    RestoreBlur();
    ok &= PatchCall(ADDR_JAWA_IDLE_CALL, TypeIdleSound);
    Log(ok ? "speed blur setting and Jawa idle sound fixed" : "speed blur / Jawa idle sound patch FAILED");
}

void FixesFrame(void)
{
    if (g_fix_hover_attack)
        HoverAttackFix();
}
