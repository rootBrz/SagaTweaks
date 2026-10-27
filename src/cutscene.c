/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Cutscene skip: Start brings the running cutscene to its end. The game ends it itself, at normal speed, so its own
 * ending (what follows the cutscene, the transition) runs as after a cutscene played to the end.
 */
#include "all.h"

int g_skip_cutscenes = 1;

/*
 * Fades around a skip. The picture fades to black while the cutscene still plays (FADE_OUT_MS), the skip then ends the
 * cutscene behind black, and gameplay fades in from black (FADE_IN_MS). g_skip_fade (0 = clear, 1 = black) is drawn
 * over the frame by the Present hook. A stock Start press does nothing in a cutscene, so letting the game run during
 * the fade-out is safe.
 */
#define FADE_OUT_MS 250
#define FADE_IN_MS  350

float g_skip_fade;
static DWORD g_fade_out_since;   /* tick count when the fade-out started, 0 = not fading out */
static DWORD g_fade_in_since;
static int g_was_black;
static int g_skipping;           /* the cutscene is being ended; the screen stays black until it is over */

/*
 * Pressing Start during a cutscene skips it: every playing cutscene animation (dword_951C3C[],
 * count dword_951C80) is moved to just before its last moment, so the game's own end-of-cutscene handling
 * (placing the actors, the completion callback, returning control, loading the next level) runs on the next updates.
 */
#define ADDR_START_MASK      0x00802274u  /* dword_802274: the Start button bit in a slot's button mask */

#define ADDR_CUTSCENE_ACTIVE 0x00951BE0u  /* dword_951BE0: a cutscene is playing */

#define ADDR_CUTSCENE_INSTANCE 0x00951BE8u  /* dword_951BE8: the running cutscene instance, +4 = its animation */

#define ANIM_FLAGS_OFS       136

#define ANIM_FINISHED        0x1000u

#define GAME_CUTSCENE_LIST_OFS 10928  /* game + 10928: { instance array, count } of the level's cutscene instances */

/* Is Start held on any slot, or on a gamepad the device scan reports with the Start button? */
static int StartPressed(void)
{
    int start = 0, i, ctx, dev, type, code;

    for (i = 0; i < 2; i++)
        start |= (*(int *)(ADDR_SLOT_INPUT + 96 * i + 4) & *(int *)ADDR_START_MASK) != 0;
    if (start)
        return 1;
    ctx = ((InputSaveFn)ADDR_INPUT_SAVE)();
    ((InputSelectFn)ADDR_INPUT_SELECT)(0);
    dev = ((DetectDeviceFn)ADDR_DETECT_DEVICE)(0, DETECT_KINDS, &type, &code, 0);
    ((InputSelectFn)ADDR_INPUT_SELECT)(ctx);
    return dev >= 0 && dev < FIRST_KEYBOARD_DEVICE && code == g_start_button;
}

/*
 * Bring a playing cutscene animation close to the end of its timeline. The animation update (sub_508340) then ends it
 * the way it does at the end of a normal play: it runs the animation's completion callback (+180, one shot), stops it
 * and lets the cutscene code run its own ending. The earlier "mark it finished" shortcut skipped the callback, which is
 * what the game uses for things that follow a cutscene, so the skipped cutscene could be played again from its trigger
 * box.
 *
 * The actors (the characters, a character that joins the party) are placed by the update steps that do not end the
 * animation; the step that ends it does not place them. So the time stops a step and a half short of the end: the
 * next step places everything where the cutscene leaves it, the one after ends the animation.
 */
#define ANIM_TIME_OFS        144
#define ANIM_DATA_OFS        88    /* the animation's data; +8 = its length */
#define ANIM_STEP_OFS        152   /* animation time added per update (speed * frame time) */

static void JumpToEnd(uint8_t *anim)
{
    uint8_t *data = *(uint8_t **)(anim + ANIM_DATA_OFS);
    float last = data ? *(float *)(data + 8) - 1.0f : 0.0f;
    float target = last - 1.5f * *(float *)(anim + ANIM_STEP_OFS);

    if (last > 1.0f && target > 0.0f && *(float *)(anim + ANIM_TIME_OFS) < target)
        *(float *)(anim + ANIM_TIME_OFS) = target;
}

/*
 * Bring every playing cutscene animation of the level to its end. When a step did not end them (force: an animation
 * that never reaches the end by itself, a looping one) they are marked finished instead.
 */
static int FinishCutsceneAnims(int game, int force)
{
    int **list = game ? *(int ***)(game + GAME_CUTSCENE_LIST_OFS) : NULL;
    int n, i, marked = 0;
    uint8_t *inst, *anim;

    if (list && list[0] && (int)list[1] > 0 && (int)list[1] < 64) {
        n = (int)list[1];
        for (i = 0; i < n; i++) {
            inst = (uint8_t *)list[0][i];
            anim = inst ? *(uint8_t **)(inst + 4) : NULL;
            if (anim && (*(uint32_t *)(anim + ANIM_FLAGS_OFS) & 2)) {
                JumpToEnd(anim);
                if (force)
                    *(uint32_t *)(anim + ANIM_FLAGS_OFS) |= ANIM_FINISHED;
                marked++;
            }
        }
    }
    inst = *(uint8_t **)ADDR_CUTSCENE_INSTANCE;
    anim = inst ? *(uint8_t **)(inst + 4) : NULL;
    if (anim) {
        JumpToEnd(anim);
        if (force)
            *(uint32_t *)(anim + ANIM_FLAGS_OFS) |= ANIM_FINISHED;
        marked++;
    }
    return marked;
}

/*
 * A skip lasts while a cutscene plays and for SKIP_FOLLOW_MS after one ends, so the parts that follow it (each one is
 * a cutscene of its own) are skipped too: one press goes to gameplay.
 */
#define SKIP_FOLLOW_MS       3000
#define SKIP_QUIET_MS        200    /* nothing playing, pending or wiping for this long: no further part follows */
#define SKIP_FORCE_MS        250    /* a part that did not end by itself in this time is marked finished */

#define ADDR_WIPE_PROGRESS   0x00950780u  /* flt_950780: screen wipe, 1 = covered, falls to 0 while a scene is wiped in */
#define ADDR_LEVEL_REQUEST   0x0087658Cu
#define ADDR_NEXT_LEVEL      0x00951B80u

/*
 * The cutscene's music and dialogue are streams of the audio object at 0x924DF8 (two slots). The game stops them with
 * sub_693FD0 (thiscall, one argument) when a level changes; g_skipping inside a level never did, so the cutscene's sound
 * went on over the gameplay.
 */
#define ADDR_AUDIO_OBJECT    0x00924DF8u
#define ADDR_STOP_STREAMS    0x00693FD0u

typedef int (__attribute__((thiscall)) *StopStreamsFn)(void *, int);

static void CutsceneSkip(int game, int cur, int titles)
{
    static int prev_start;
    static DWORD busy_since, part_since, quiet_since;
    DWORD now = GetTickCount();
    int start, active;

    if (!g_skip_cutscenes || !cur || cur == titles) {
        prev_start = g_skipping = 0;
        g_fade_out_since = 0;
        return;
    }
    start = StartPressed();
    active = *(int *)ADDR_CUTSCENE_ACTIVE;
    if (start && !prev_start && active && !g_skipping && !g_fade_out_since) {
        g_fade_out_since = TickNonZero();
        Log("cutscene skip: fading out");
    }
    prev_start = start;
    if (g_fade_out_since && !active)
        g_fade_out_since = 0;                    /* the cutscene ended by itself meanwhile */
    if (g_fade_out_since && now - g_fade_out_since >= FADE_OUT_MS) {
        g_fade_out_since = 0;
        g_skipping = 1;
        busy_since = now;
        Log("cutscene skipped");
    }
    if (!g_skipping)
        return;
    if (active) {
        busy_since = now;
        quiet_since = 0;
        if (!part_since)
            part_since = now;
        FinishCutsceneAnims(game, now - part_since >= SKIP_FORCE_MS);
        return;
    }
    part_since = 0;
    if (*(int *)ADDR_LEVEL_REQUEST || *(int *)ADDR_NEXT_LEVEL || *(float *)ADDR_WIPE_PROGRESS > 0.0f)
        quiet_since = 0;
    else if (!quiet_since)
        quiet_since = now;
    if (now - busy_since >= SKIP_FOLLOW_MS || (quiet_since && now - quiet_since >= SKIP_QUIET_MS)) {
        g_skipping = 0;
        ((StopStreamsFn)ADDR_STOP_STREAMS)((void *)ADDR_AUDIO_OBJECT, 0);
        Log("cutscene skip done");
    }
}

static void UpdateFade(void)
{
    DWORD now = GetTickCount();

    if (g_fade_out_since) {
        g_skip_fade = (float)(now - g_fade_out_since) / FADE_OUT_MS;
        if (g_skip_fade > 1.0f)
            g_skip_fade = 1.0f;
    } else if (g_skipping) {
        g_skip_fade = 1.0f;
        g_was_black = 1;
        g_fade_in_since = 0;
    } else {
        if (g_was_black) {
            g_was_black = 0;
            g_fade_in_since = TickNonZero();
        }
        g_skip_fade = 0.0f;
        if (g_fade_in_since) {
            DWORD e = now - g_fade_in_since;

            if (e >= FADE_IN_MS)
                g_fade_in_since = 0;
            else
                g_skip_fade = 1.0f - (float)e / FADE_IN_MS;
        }
    }
}

/* Runs one game frame, after the skip has moved the cutscene towards its end. */
void CutsceneFrame(int game, int *out, int cur, int titles)
{
    CutsceneSkip(game, cur, titles);
    FramerateGameFrame(game, out);
    UpdateFade();
}
