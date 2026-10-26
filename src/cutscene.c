/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Cutscene skip: Start finishes the running cutscene parts and hides the transition.
 */
#include "all.h"

int g_skip_cutscenes = 1;

int g_trace_skip;

DWORD g_skip_done_tick, g_skip_first_tick;

int g_cutscene_skipping;

/*
 * Fades around a skip. The picture fades to black while the cutscene still plays (FADE_OUT_MS), the skip then runs
 * hidden behind black, and gameplay fades in from black (FADE_IN_MS). g_skip_fade (0 = clear, 1 = black) is drawn over
 * the frame by the Present hook. A stock Start press does nothing in a cutscene, so letting the game run during the
 * fade-out is safe.
 */
#define FADE_OUT_MS 250
#define FADE_IN_MS  350

float g_skip_fade;
static DWORD g_fade_out_since;   /* tick count when the fade-out started, 0 = not fading out */
static DWORD g_fade_in_since;
static int g_was_black;

/*
 * Pressing Start during a cutscene skips it: every playing cutscene animation (dword_951C3C[],
 * count dword_951C80) is moved to its last moment, so the game's own end-of-cutscene handling
 * (fade, returning control, loading the next level) runs on the next update.
 */
#define ADDR_START_MASK      0x00802274u  /* dword_802274: the Start button bit in a slot's button mask */

#define ADDR_CUTSCENE_ACTIVE 0x00951BE0u  /* dword_951BE0: a cutscene is playing */

#define ADDR_CUTSCENE_ANIMS  0x00951C3Cu  /* dword_951C3C[4]: playing cutscene animations */

#define ADDR_CUTSCENE_COUNT  0x00951C80u

#define ADDR_CUTSCENE_INSTANCE 0x00951BE8u  /* dword_951BE8: the running cutscene instance, +4 = its animation */

#define ANIM_FLAGS_OFS       136

#define ANIM_FINISHED        0x1000u

static DWORD g_skip_tick;

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

/* Mark every playing cutscene animation of the level as finished. */
static int FinishCutsceneAnims(int game)
{
    int **list = game ? *(int ***)(game + GAME_CUTSCENE_LIST_OFS) : NULL;
    int n, i, marked = 0;
    uint8_t *anim;

    if (list && list[0] && (int)list[1] > 0 && (int)list[1] < 64) {
        n = (int)list[1];
        for (i = 0; i < n; i++) {
            uint8_t *inst = (uint8_t *)list[0][i];

            anim = inst ? *(uint8_t **)(inst + 4) : NULL;
            if (anim && (*(uint32_t *)(anim + ANIM_FLAGS_OFS) & 2)) {
                *(uint32_t *)(anim + ANIM_FLAGS_OFS) |= ANIM_FINISHED;
                marked++;
            }
        }
    }
    {
        uint8_t *inst = *(uint8_t **)ADDR_CUTSCENE_INSTANCE;

        anim = inst ? *(uint8_t **)(inst + 4) : NULL;
        if (anim) {
            *(uint32_t *)(anim + ANIM_FLAGS_OFS) |= ANIM_FINISHED;
            marked++;
        }
    }
    return marked;
}

static void LogCutsceneState(const char *tag)
{
    uint8_t *inst = *(uint8_t **)ADDR_CUTSCENE_INSTANCE;
    uint8_t *anim = inst ? *(uint8_t **)(inst + 4) : NULL;

    char buf[300];

    snprintf(buf, sizeof buf, "cutscene %s: inst %p flags %08x next %d str '%.20s' | anim flags %08x time %.2f step %.3f | hold %d snd %d count %d",
        tag, inst, inst ? *(uint32_t *)(inst + 80) : 0, inst ? *(short *)(inst + 236) : -9,
        inst ? (char *)(inst + 244) : "",
        anim ? *(uint32_t *)(anim + ANIM_FLAGS_OFS) : 0, anim ? *(float *)(anim + 144) : 0.f,
        anim ? *(float *)(anim + 152) : 0.f, *(int *)0x951C58, *(int *)0x803F78,
        *(int *)ADDR_CUTSCENE_COUNT);
    Log(buf);
}

static int g_skip_active; /* a skip is running: FrameHook steps the game until the cutscene is over */
#define SKIP_IDLE_STEPS 4    /* steps without a cutscene before the skip is over */

/*
 * Start skips the cutscene that is playing, and the parts that follow it straight away (the game
 * is stepped with a black screen until the next part starts), so one press goes to gameplay.
 */
#define SKIP_FOLLOW_MS       3000   /* after a skip, parts starting within this time are skipped too */

#define SKIP_QUIET_MS        200    /* nothing pending and no wipe for this long: no further part follows */

#define ADDR_WIPE_PROGRESS   0x00950780u  /* flt_950780: screen wipe, 1 = covered, falls to 0 while a scene is wiped in */

#define ADDR_WIPE_SPEED      0x00950784u

#define ADDR_LEVEL_REQUEST   0x0087658Cu

#define ADDR_NEXT_LEVEL      0x00951B80u

static DWORD g_skip_arm_until;

/*
 * The cutscene's music and dialogue are streams of the audio object at 0x924DF8 (two slots). The game stops them with
 * sub_693FD0 (thiscall, one argument) when a level changes; skipping inside a level never did, so the cutscene's sound
 * went on over the gameplay.
 */
#define ADDR_AUDIO_OBJECT    0x00924DF8u
#define ADDR_STOP_STREAMS    0x00693FD0u

typedef int (__attribute__((thiscall)) *StopStreamsFn)(void *, int);

static void CutsceneSkip(int cur, int titles)
{
    static int prev_start;
    DWORD now = GetTickCount();
    int start, edge, active;

    g_cutscene_skipping = 0;
    if (!g_skip_cutscenes || !cur || cur == titles) {
        prev_start = g_skip_active = 0;
        g_skip_arm_until = 0;
        g_fade_out_since = 0;
        return;
    }
    start = StartPressed();
    edge = start && !prev_start;
    prev_start = start;
    active = *(int *)ADDR_CUTSCENE_ACTIVE;
    if (g_skip_arm_until && !g_skip_active && !active) {
        /*
         * Between two cutscene parts (or after the last one): the game wipes the next scene in
         * from black; that is skipped, and once nothing is pending or fading the wait is over.
         */
        static DWORD quiet_since;
        float *wipe = (float *)ADDR_WIPE_PROGRESS;
        int pending = *(int *)ADDR_LEVEL_REQUEST || *(int *)ADDR_NEXT_LEVEL;

        if (!pending && *wipe > 0.0f && *(float *)ADDR_WIPE_SPEED < 0.0f)
            *wipe = 0.0f;
        if (now >= g_skip_arm_until) {
            g_skip_arm_until = 0;
        } else if (!pending && *wipe <= 0.0f) {
            if (!quiet_since)
                quiet_since = now;
            if (now - quiet_since > SKIP_QUIET_MS) {
                g_skip_arm_until = 0;
                quiet_since = 0;
            }
        } else {
            quiet_since = 0;
        }
    }
    if (edge && !g_skip_active && !g_fade_out_since && active) {
        g_fade_out_since = TickNonZero();
        Log("cutscene skip: fading out");
    }
    if (g_fade_out_since && now - g_fade_out_since >= FADE_OUT_MS) {
        g_fade_out_since = 0;
        if (active) {
            g_skip_tick = g_skip_first_tick = now;
            g_skip_active = 1;
            LogCutsceneState("start");
            Log("cutscene skipped");
        }
    }
    if (active && !g_skip_active && now < g_skip_arm_until) {
        char buf[80];

        snprintf(buf, sizeof buf, "cutscene skip: part skipped %lu ms after the last one", now - g_skip_done_tick);
        Log(buf);
        g_skip_tick = now;
        g_skip_active = 1;
    }
    g_cutscene_skipping = g_skip_active;
}

/*
 * Runs the skip: the game is stepped back to back inside this one frame (no frames are shown)
 * with every playing cutscene animation marked finished, so each shot hands over to the next
 * until the cutscene's own ending has run. It stops the moment the game is out of cutscene mode
 * for a few steps, so gameplay never runs sped up.
 */
static void RunCutsceneSkip(int game, int *out)
{
    float *dt = (float *)ADDR_FRAME_DT, saved = *dt;
    DWORD t0 = GetTickCount();
    int k, idle = 0;

    for (k = 0; k < 96 && GetTickCount() - t0 < 300; k++) {
        if (*(int *)ADDR_CUTSCENE_ACTIVE) {
            uint8_t *inst = *(uint8_t **)ADDR_CUTSCENE_INSTANCE;

            idle = 0;
            (void)inst;
            FinishCutsceneAnims(game);
            *dt = 0.1f;
        } else {
            *dt = saved;
            idle++;
        }
        ((FrameFn)ADDR_FRAME_FUNC)(game, out);
        if (idle >= SKIP_IDLE_STEPS) {
            g_skip_active = 0;
            g_skip_done_tick = GetTickCount();
            g_skip_arm_until = g_skip_done_tick + SKIP_FOLLOW_MS;
            ((StopStreamsFn)ADDR_STOP_STREAMS)((void *)ADDR_AUDIO_OBJECT, 0);
            {
                char buf[64];

                snprintf(buf, sizeof buf, "cutscene skip done (after %d steps)", k);
                Log(buf);
            }
            break;
        }
    }
    *dt = saved;
    if (g_skip_active && GetTickCount() - g_skip_tick > 3000) {
        g_skip_active = 0;
        LogCutsceneState("gave up");
        Log("cutscene skip gave up");
    }
    g_cutscene_skipping = g_skip_active || GetTickCount() < g_skip_arm_until;
}
static void CutsceneFrameInner(int game, int *out, int cur, int titles);

static void UpdateFade(void)
{
    DWORD now = GetTickCount();

    if (g_fade_out_since) {
        g_skip_fade = (float)(now - g_fade_out_since) / FADE_OUT_MS;
        if (g_skip_fade > 1.0f)
            g_skip_fade = 1.0f;
    } else if (g_cutscene_skipping) {
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

/* Runs one game frame, or the cutscene skip in its place. */
void CutsceneFrame(int game, int *out, int cur, int titles)
{
    CutsceneFrameInner(game, out, cur, titles);
    UpdateFade();
}

static void CutsceneFrameInner(int game, int *out, int cur, int titles)
{
    CutsceneSkip(cur, titles);
    if (g_skip_active) {
        RunCutsceneSkip(game, out);
    } else if (g_skip_arm_until && GetTickCount() < g_skip_arm_until && !*(int *)ADDR_CUTSCENE_ACTIVE) {
        /* the next part of the cutscene is loading: it must not be shown */
        ((FrameFn)ADDR_FRAME_FUNC)(game, out);
        g_cutscene_skipping = 1;
    } else {
        FramerateGameFrame(game, out);
    }
}
