/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Cutscene skip: Start brings the running cutscene to its end. The game ends it itself, at normal speed, so its own
 * ending (what follows the cutscene, the transition) runs as after a cutscene played to the end.
 */
#include "all.h"

int g_skip_cutscenes = 1;

static int g_skipping;    /* the cutscene is being ended; the captured picture covers the screen until it is over */
static int g_skip_phase;  /* 1 = the picture was asked for, 2 = it is held */

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
#define SKIP_MAX_MS          10000  /* a whole skip, however many parts, is over after this */

/*
 * The transition manager (byte_95077C) is the game's own screen wipe, the "Star Wars wipe": +0 = style bits (1/2 = sideways,
 * 4/8 = up and down), +4 = progress (1 = covered,
 * falls to 0), +8 = rate per second, +36 = wipe style in use (-1 = none). A wipe-in (sub_5B11B0(1)) shows the picture
 * that was captured with byte_950740 (taken at the end of the frame) covering the screen and sweeping it away over the
 * live scene. The skip captures the cutscene's last picture, holds it (rate 0) while the cutscene ends behind it,
 * and lets the sweep run once gameplay is back, like the stock end of a cutscene.
 */
#define ADDR_WIPE_MANAGER    0x0095077Cu
#define ADDR_WIPE_PROGRESS   0x00950780u
#define ADDR_WIPE_RATE       0x00950784u
#define ADDR_WIPE_CAPTURE    0x00950740u  /* byte_950740: capture the screen at the end of this frame */
#define WIPE_STYLE_PICTURE   2            /* the wipe that sweeps a captured picture away */
#define WIPE_IN_RATE         -1.3333334f  /* what the game's own wipe-in uses */

#define ADDR_LEVEL_REQUEST   0x0087658Cu
#define ADDR_NEXT_LEVEL      0x00951B80u

typedef int (__attribute__((thiscall)) *WipeSelectFn)(void *, int *, int);
typedef int (__attribute__((thiscall)) *WipeStartFn)(void *, int);
typedef int (__cdecl *PlaySoundFn)(const char *, int);

#define ADDR_WIPE_SELECT     0x005B1220u
#define ADDR_WIPE_START      0x005B11B0u
#define ADDR_PLAY_SOUND      0x00681F40u

/*
 * The cutscene's music and dialogue are streams of the audio object at 0x924DF8 (two slots). The game stops them with
 * sub_693FD0 (thiscall, one argument) when a level changes; skipping inside a level never did, so the cutscene's sound
 * went on over the gameplay.
 */
#define ADDR_AUDIO_OBJECT    0x00924DF8u
#define ADDR_STOP_STREAMS    0x00693FD0u

typedef int (__attribute__((thiscall)) *StopStreamsFn)(void *, int);

/* Ends the skip: lets the held picture sweep away (when one is held) and stops the cutscene's sound. */
static void EndSkip(int *holding, int sound)
{
    g_skipping = g_skip_phase = 0;
    if (*holding) {
        *holding = 0;
        *(float *)ADDR_WIPE_RATE = WIPE_IN_RATE;
        if (sound)
            ((PlaySoundFn)ADDR_PLAY_SOUND)("WipeScreen", 0);
    }
    if (sound)
        ((StopStreamsFn)ADDR_STOP_STREAMS)((void *)ADDR_AUDIO_OBJECT, 0);
}

static void CutsceneSkip(int game, int cur, int titles)
{
    static int prev_start, holding, style_bits;
    static DWORD skip_since, busy_since, part_since, quiet_since;
    DWORD now = GetTickCount();
    int start, active, pending;

    if (!g_skip_cutscenes || !cur || cur == titles) {
        prev_start = 0;
        EndSkip(&holding, 0);                    /* the option was turned off, or the level is gone: never leave the screen covered */
        return;
    }
    start = StartPressed();
    active = *(int *)ADDR_CUTSCENE_ACTIVE;
    pending = *(int *)ADDR_LEVEL_REQUEST || *(int *)ADDR_NEXT_LEVEL;
    if (start && !prev_start && active && !g_skipping) {
        *(uint8_t *)ADDR_WIPE_CAPTURE = 1;       /* the picture of this frame is the one that is held */
        g_skipping = 1;
        g_skip_phase = 1;
        busy_since = skip_since = now;
        Log("cutscene skipped");
        prev_start = start;
        return;
    }
    prev_start = start;
    if (!g_skipping)
        return;
    if (g_skip_phase == 1) {
        int style = WIPE_STYLE_PICTURE;

        ((WipeSelectFn)ADDR_WIPE_SELECT)((void *)ADDR_WIPE_MANAGER, &style, 0);
        ((WipeStartFn)ADDR_WIPE_START)((void *)ADDR_WIPE_MANAGER, 1);
        g_skip_phase = 2;
        holding = 1;
        style_bits = *(int *)ADDR_WIPE_MANAGER;   /* the style the game picked at the start */
        if (!(style_bits & 3))
            style_bits = 1;                       /* a sideways sweep */
    }
    if (now - skip_since >= SKIP_MAX_MS) {
        Log("cutscene skip gave up");
        EndSkip(&holding, 1);                    /* a cutscene that never ends must not keep the picture frozen */
        return;
    }
    if (holding) {
        *(float *)ADDR_WIPE_RATE = 0.0f;
        *(int *)ADDR_WIPE_MANAGER = style_bits;   /* the end of a cutscene clears it */
    }
    if (active) {
        busy_since = now;
        quiet_since = 0;
        if (!part_since)
            part_since = now;
        FinishCutsceneAnims(game, now - part_since >= SKIP_FORCE_MS);
        return;
    }
    part_since = 0;
    if (pending)
        quiet_since = 0;
    else if (!quiet_since)
        quiet_since = now;
    if (now - busy_since >= SKIP_FOLLOW_MS || (quiet_since && now - quiet_since >= SKIP_QUIET_MS)) {
        EndSkip(&holding, 1);
        Log("cutscene skip done");
    }
}

/* Runs one game frame, after the skip has moved the cutscene towards its end. */
void CutsceneFrame(int game, int *out, int cur, int titles)
{
    CutsceneSkip(game, cur, titles);
    FramerateGameFrame(game, out);
}
