/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Frame rate: FPS cap, refresh rate, and frame-rate independence of clocks and jump physics.
 */
#include "all.h"

static const uint8_t k_limit_cmp_orig[6]   = { 0xDC, 0x1D, 0x30, 0xEC, 0x75, 0x00 };

static const uint8_t k_limit_load_orig[6]  = { 0xD9, 0x05, 0x2C, 0xEC, 0x75, 0x00 };

static const uint8_t k_clock_orig[6]       = { 0x8B, 0x4C, 0x24, 0x04, 0xD9, 0x01 };

static const uint8_t k_close_1_orig[6]    = { 0xD9, 0x05, 0xA4, 0x97, 0x75, 0x00 };

static const uint8_t k_close_2_orig[6]    = { 0xD9, 0x05, 0x18, 0xE8, 0x76, 0x00 };

static const uint8_t k_wind_orig[7]       = { 0x83, 0x05, 0x0C, 0x39, 0x97, 0x00, 0x01 };

static const uint8_t k_refresh_orig[9]     = { 0x8B, 0x40, 0x08, 0x89, 0x86, 0x68, 0x01, 0x00, 0x00 };

static double g_limit_d = UNLIMITED_FPS;

static float  g_limit_f = (float)UNLIMITED_FPS; /* button code reported by the input scan for Start */
int g_max_fps; /* 0 = unlimited (vsync decides) */
int g_refresh_override; /* 0 = desktop refresh rate */
int g_fix_counters = 1; /* make per-frame counters tick at 60 Hz */
int g_fix_physics = 1;

int g_log_fps;
int g_probe_sweep;
int g_probe_hero;
int g_probe_buttons;
int g_probe_lever;
int g_probe_anim;
int g_fix_gameplay = 1;
int g_logic_hz;   /* upper bound of game logic updates per second, 0 = every frame */
int g_live_ini;

void ApplyMaxFps(void)
{
    g_limit_d = g_max_fps > 0 ? (double)g_max_fps : UNLIMITED_FPS;
    g_limit_f = (float)g_limit_d;
}

/*
 * Replacement for "mov eax,[eax+8]; mov [esi+168h],eax": eax = refresh override or
 * the desktop refresh, store it, resume at the jz that follows. pushfd/popfd keep
 * the flags that jz depends on.
 */
extern void RefreshStub(void);

__asm__(
    ".text\n"
    ".globl _RefreshStub\n"
    "_RefreshStub:\n"
    "    pushfl\n"
    "    movl _g_refresh_override, %eax\n"
    "    testl %eax, %eax\n"
    "    jnz 1f\n"
    "    movl 0x610(%esi), %eax\n"
    "1:  popfl\n"
    "    movl %eax, 0x168(%esi)\n"
    "    movl $0x006E3AF1, %eax\n"
    "    jmp *%eax\n"
);

#define MAX_CLOCKS 64

static struct { void *clock; float acc; }
 g_clocks[MAX_CLOCKS];

static float *ClockAccumulator(void *clock)
{
    int i;

    for (i = 0; i < MAX_CLOCKS && g_clocks[i].clock; i++)
        if (g_clocks[i].clock == clock)
            return &g_clocks[i].acc;
    if (i == MAX_CLOCKS)
        return NULL;
    g_clocks[i].clock = clock;
    return &g_clocks[i].acc;
}

/* sub_548FE0 replacement; same behaviour except the integer counter at +12. */
static int __cdecl ClockUpdate(float *clock)
{
    float dt = *(float *)ADDR_FRAME_DT;
    int *counter = (int *)(clock + 3);
    float *acc;
    int n;

    clock[1] = clock[0];
    clock[0] += dt;
    clock[2] += dt;
    while (clock[2] >= 1.0f)
        clock[2] -= 1.0f;

    if (*counter == -1)
        return -1;
    if (!g_fix_counters) {
        (*counter)++;
        return *counter;
    }
    acc = ClockAccumulator(clock);
    if (!acc) {
        (*counter)++;
        return *counter;
    }
    *acc += dt * COUNTER_HZ;
    n = (int)(*acc + 0.05f);        /* tolerance so exactly 60 fps never skips or doubles a tick */
    if (n < 0)
        n = 0;
    *acc -= (float)n;
    *counter += n;
    return *counter;
}

extern LONGLONG g_present_ticks;

static int g_jump_fixes;   /* for LogFPS */

static void LogFps(void)
{
    static LARGE_INTEGER freq, last, start;
    static int frames, lines;
    static double worst;
    LARGE_INTEGER now;
    double dt, span;

    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        last = start = now;
        return;
    }
    dt = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
    last = now;
    frames++;
    if (dt > worst)
        worst = dt;
    span = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
    if (span >= 1.0 && lines < 120) {
        if (g_log) {
            fprintf(g_log, "fps %.1f, worst frame %.1f ms, in Present %.1f ms/frame, jump fixes %d\n",
                    frames / span, worst, (double)g_present_ticks * 1000.0 / (double)freq.QuadPart / frames,
                    g_jump_fixes);
            g_jump_fixes = 0;
            g_present_ticks = 0;
            fflush(g_log);
        }
        lines++;
        frames = 0;
        worst = 0;
        start = now;
    }
}

/*
 * Jump. The game integrates the jump with the frame time. At high frame rates that has two effects:
 *  - the first frame lifts the player by only speed * dt (0.002 at 1000 fps instead of 0.038 at 60 fps).
 *    Standing on a platform (the pallet in Episode VI "Speeder Showdown") the contact test still counts that
 *    as "on the platform" and cancels the jump on the next frame;
 *  - the apex is v^2 / 2g + v dt / 2, so it is lower than the 60 fps apex the levels were built for.
 * When a jump impulse is applied (vertical speed suddenly >= 1, from rest or falling) the player is lifted at
 * once by the 60 fps first-frame lift and the speed is chosen so that lift + v^2 / 2g + v dt / 2 equals the
 * 60 fps apex. The gravity is the character's own (physics block +36).
 */
#define JUMP_IMPULSE_GAP 0.3f    /* a mover that keeps pushing the speed up must not be corrected every frame */

/* Apex of the game's jump at 60 fps: heights of the steps v, v - g/60, ... while v > 0. */
static double JumpApex60(double v, double g)
{
    double h = 0.0;

    for (; v > 0.0; v -= g / 60.0)
        h += v / 60.0;
    return h;
}

static void JumpFix(float dt)
{
    static float last[4], quiet;
    int i;

    quiet -= dt;
    for (i = 0; i < 4; i++) {
        char *p = (char *)*(int *)(ADDR_PLAYER_OBJ + 4 * i);
        float vy, *vp;

        if (!p)
            continue;
        vp = (float *)(p + 108);
        vy = *vp;
        if (vy >= 1.0f && last[i] <= 0.5f && vy - last[i] >= 1.0f && quiet <= 0.0f && dt < 0.0158f) {
            int *phys = *(int **)(*(int *)(p + 84) + 36);
            double g = phys ? -(double)*(float *)((char *)phys + 36) : 0.0;

            if (g > 1.0 && g < 100.0) {
                double v0 = vy, d = dt;
                double rest = JumpApex60(v0, g) - v0 / 60.0;    /* what is left of the apex after the first frame */
                double v = g * (d / 2.0 + sqrt(d * d / 4.0 + 2.0 * rest / g));
                double lift = v0 / 60.0 - v * d;

                if (v > v0 * 0.9 && v < v0 * 1.1 && lift >= 0.0) {
                    *vp = (float)v;
                    *(float *)(p + 96) += (float)lift;
                    vy = (float)v;
                    g_jump_fixes++;
                    quiet = JUMP_IMPULSE_GAP;
                }
            }
        }
        last[i] = vy;
    }
}

static float g_close_dist = 0.005f, g_close_dist2 = 0.000005f;

#define PUSH_GROUPS 8

static const double k_push_base[PUSH_GROUPS] = { 0.0003000000142492354, 0.0006000000284984708, 0.0008999999845400453,
                                                 0.001500000013038516, 0.0035000001080334187, 0.004000000189989805,
                                                 0.009999999776482582, 0.0010000000474974513 };

static double g_push[PUSH_GROUPS] = { 0.0003000000142492354, 0.0006000000284984708, 0.0008999999845400453,
                                      0.001500000013038516, 0.0035000001080334187, 0.004000000189989805,
                                      0.009999999776482582, 0.0010000000474974513 };

/* Load sites (fadd / fmul / fld / fsubr qword [abs]) of the constants above: instruction, constant, group. */
static const struct { uintptr_t addr, cst; int group; } k_push_sites[] = {
    { 0x677FD6, 0x76E190, 0 }, { 0x678036, 0x76E190, 0 }, { 0x678096, 0x76E190, 0 }, { 0x678A5C, 0x76E190, 0 },
    { 0x678967, 0x76E1A8, 1 }, { 0x678998, 0x76E1A0, 2 }, { 0x6787C2, 0x76E1B8, 3 }, { 0x678BE8, 0x76E198, 4 },
    { 0x678CA0, 0x76E198, 4 }, { 0x679037, 0x76E1C8, 5 }, { 0x678CC1, 0x7531A8, 6 },
    { 0x68FE03, 0x753180, 7 },   /* sub_68F940: contact normal * 0.001 on every sweep loop pass */
    { 0x679675, 0x7531A8, 6 },   /* sub_679310 (hit on a moving object): movement -= normal * 0.01 */
    { 0x68CA90, 0x7531A8, 6 },   /* editor input handler: value += 0.01 per call while a button is held */
};

#define PUSH_SITES ((int)(sizeof k_push_sites / sizeof k_push_sites[0]))

static int PushSitesMatch(void)
{
    int i;

    for (i = 0; i < PUSH_SITES; i++) {
        const uint8_t *b = (const uint8_t *)k_push_sites[i].addr;

        if ((b[0] != 0xDC && b[0] != 0xDD) || (b[1] & 0xC7) != 0x05 ||
            *(const uint32_t *)(b + 2) != (uint32_t)k_push_sites[i].cst)
            return 0;
    }
    return 1;
}

/*
 * Collision tolerances are meant for 1/60 s steps.
 *  - "Very close distance" is a fraction of the step (the hit time t = d0 / (d0 - d1) is a fraction of the
 *    movement vector): 0.5 % of a 60 fps step is 1e-4 units, 0.5 % of an 850 fps step is 7e-6, below the
 *    float resolution at the hero's coordinates. The fraction grows as the step shrinks so that the gap in
 *    world units stays that of 60 fps.
 *  - The fixed push-out offsets are world units added per contact, i.e. per frame: they shrink with the step.
 */
static double g_damp75 = 0.75;
static double g_damp50 = 0.5;
static double g_wall_step = 2.0 / 30.0;
static float g_arc_kick = 0.2f;          /* flt_8002D0 * scale */
static double g_rot_m16 = -16.0, g_rot_p16 = 16.0, g_overlay_ms = 33.333335876464844;
static float g_rope_damp = 0.9f;         /* flt_7FFCA0 ^ scale */
__attribute__((used)) static float g_release_neg = 0.0f;  /* release threshold of the floor buttons, see ReleaseSlackStub */
__attribute__((used)) static float g_rope_imp = 1.0f;     /* (1 - g_rope_damp) / (1 - 0.9): see RopeImpulseStub */
__attribute__((used)) static float g_path_step = 0.0f;   /* flt_7F15DC * g_step_scale */
__attribute__((used)) static float g_boost_step = 4.0f;    /* 240 * dt, see BoostResetStub */
__attribute__((used)) static float g_step_scale = 1.0f;    /* min(1, dt*60) of the current logic frame */


/* Replacement for sub_54FB40, see above. */
static double __cdecl Lerp60(float cur, float target, float rate)
{
    float dt = *(float *)ADDR_FRAME_DT;
    float f = dt * rate;

    if (dt < 1.0f / 60.0f && rate > 0.0f) {
        float base = 1.0f - rate / COUNTER_HZ;

        f = base <= 0.0f ? 1.0f : 1.0f - powf(base, dt * COUNTER_HZ);
    }
    if (f > 1.0f)
        f = 1.0f;
    return (float)((target - cur) * f + cur);
}

static void HeroProbe(float dt);
static void ButtonProbe(int game, float dt);
static void LeverProbe(float dt);

static void UpdateCollisionTolerance(float dt)
{
    float scale = dt * COUNTER_HZ;
    int i;

    if (scale > 1.0f)
        scale = 1.0f;
    g_close_dist = scale < 0.01f ? 0.5f : 0.005f / scale;
    if (g_close_dist > 0.5f)
        g_close_dist = 0.5f;
    for (i = 0; i < PUSH_GROUPS; i++)
        g_push[i] = k_push_base[i] * scale;
    g_step_scale = scale;
    g_boost_step = 240.0f * dt;
    g_path_step = *(const float *)0x007F15DC * scale;
    g_damp75 = pow(0.75, scale);
    g_damp50 = pow(0.5, scale);
    g_wall_step = 4.0 * dt < 2.0 / 30.0 ? 4.0 * dt : 2.0 / 30.0;
    g_arc_kick = (float)(0.2000000029802322 * scale);
    g_rot_m16 = -16.0 * scale;
    g_rot_p16 = 16.0 * scale;
    g_overlay_ms = 33.333335876464844 * scale;
    g_rope_damp = (float)pow(0.8999999761581421, scale);
    g_rope_imp = (float)((1.0 - (double)g_rope_damp) / 0.10000000238418579);
    g_release_neg = (float)(-(2.0 / 60.0) * (1.0 - scale));
}

/*
 * Per-call damping of the hero physics. These multiply a stored value by a constant on every call, so the
 * decay per second depends on the call rate. Raising the constant to the power dt*60 gives the same decay per
 * second at any rate. The steps below 1/60 s only; longer steps behave as in the original.
 */
static const struct { uint32_t addr; uint8_t op[2]; uint32_t orig; double *var; } k_damp_sites[] = {
    { ADDR_DAMP_STEPUP, { 0xDC, 0x0D }, 0x00751AC0u, &g_damp75 },
    { ADDR_DAMP_BOOST,  { 0xDC, 0x0D }, 0x007516D8u, &g_damp50 },
    { ADDR_DAMP_BOOST2, { 0xDC, 0x0D }, 0x007516D8u, &g_damp50 },
};

static int DampSitesMatch(void)
{
    size_t i;

    for (i = 0; i < sizeof k_damp_sites / sizeof k_damp_sites[0]; i++) {
        const uint8_t *b = (const uint8_t *)k_damp_sites[i].addr;

        if (b[0] != k_damp_sites[i].op[0] || b[1] != k_damp_sites[i].op[1] ||
            *(const uint32_t *)(b + 2) != k_damp_sites[i].orig)
            return 0;
    }
    return 1;
}

/*
 * sub_59BD40: the timer +3524 (hero pushes against a wall candidate near the ground; at 0.25 the wall-push
 * state starts) gains dt * (2 / dt / 30) = 2/30 per CALL when dt <= 1/30, so it fires after 4 calls: 67 ms at
 * 60 fps but 5 ms at 850 fps, e.g. when a jump only brushes the log. 4*dt per call is the same 2/30 at 60 fps and
 * the same 67 ms at any rate.
 */
static const uint8_t k_wall_orig[16] = { 0xD9, 0xC0, 0xD9, 0xE8, 0xDE, 0xF1, 0xDC, 0x35, 0xC8, 0x31, 0x75, 0x00,
                                         0xDC, 0xC0, 0xDE, 0xC9 };

static int PatchWallStep(void)
{
    uint8_t code[16];
    uint32_t p = (uint32_t)(uintptr_t)&g_wall_step;

    memset(code, 0x90, sizeof code);
    code[0] = 0xDD; code[1] = 0xD8;                    /* fstp st(0): drop dt */
    code[2] = 0xDD; code[3] = 0x05;                    /* fld qword [g_wall_step] */
    memcpy(code + 4, &p, 4);
    return WritePatch(ADDR_WALLPUSH_STEP, code, sizeof code);
}

/* Calls of sub_6E6B90 whose argument is a product with the frame time (found by scanning the code). */
static const uintptr_t k_trunc_sites[] = {
    0x40CDA9, 0x41044A, 0x410CE7, 0x410D08, 0x416105, 0x425925, 0x425936, 0x432B7E, 0x432BFA, 0x432C5A, 0x4334B3, 0x437607, 0x43761C, 0x469614, 0x469658, 0x469771, 0x478276, 0x478299, 0x4782BC, 0x4782DF, 0x49A883, 0x4BB32C, 0x4D9671, 0x4E6CD9, 0x4E73FE, 0x4E7419, 0x554E40, 0x5A3AEF
};
#define TRUNC_SITES ((int)(sizeof k_trunc_sites / sizeof k_trunc_sites[0]))

static int CallSiteOk(uint32_t site, uint32_t fn);

static int TruncSitesMatch(void)
{
    int i;

    for (i = 0; i < TRUNC_SITES; i++)
        if (!CallSiteOk(k_trunc_sites[i], ADDR_TRUNC_FN))
            return 0;
    return 1;
}

/*
 * Per-call additions and decays of gameplay code, found by the whole-loop audit (docs/AUDIT.md). Each site loads a
 * constant from memory; the operand is redirected to a variable holding the constant times the step scale (or
 * raised to it for a decay). Sites are verified by opcode and constant address before they are patched.
 *  - 4EB408: lightning arc, +-flt_8002D0 added to the hero's x and z velocity on every call (sub_4EAF30)
 *  - 620911 / 620D11: -16 / +16 angle units added every call (sub_620870 / sub_620BF0), followed by an int
 *    conversion that has to keep the fraction (calls 620929 / 620D1E use the scaled dither)
 *  - 631774: overlay timer reduced by 33.33 ms on every call (sub_631440)
 *  - 4D6DBA: rope vector multiplied by flt_7FFCA0 = 0.9 on every call (sub_4D6060)
 */
static const struct { uint32_t addr; uint8_t op[2]; uint32_t cst; const void *var; } k_rate_sites[] = {
    { 0x4EB408, { 0xD9, 0x05 }, 0x008002D0u, &g_arc_kick },
    { 0x620911, { 0xDD, 0x05 }, 0x0076AEC0u, &g_rot_m16 },
    { 0x620D11, { 0xDD, 0x05 }, 0x00758FB8u, &g_rot_p16 },
    { 0x631774, { 0xDC, 0x25 }, 0x0076B1F0u, &g_overlay_ms },
    { 0x4D6DBA, { 0xD9, 0x05 }, 0x007FFCA0u, &g_rope_damp },
};
#define RATE_SITES ((int)(sizeof k_rate_sites / sizeof k_rate_sites[0]))
static const uintptr_t k_rate_calls[] = { 0x620929, 0x620D1E };

/* Pushes added to a velocity on every call while two objects overlap: sub_58E4E0 (hero knockback) and sub_42C740
 * (object pushes player). The amount is loaded from the stack; the stubs scale it by the step. */
static const uint8_t k_knock_orig[6] = { 0xD9, 0x44, 0x24, 0x1C, 0xD9, 0xC0 };
static const uint8_t k_push_orig[6] = { 0xD9, 0x44, 0x24, 0x0C, 0xD9, 0xC0 };
#define ADDR_KNOCKBACK_AMOUNT 0x0058E7D0u
#define ADDR_OBJPUSH_AMOUNT   0x0042CC1Eu
extern void KnockbackStub(void);
extern void ObjPushStub(void);

/*
 * Byte counters changed once per call. g_steps (60 Hz ticks in this frame, 0 on most frames at high fps) says how
 * many times the call would have happened at 60 fps. sub_4802E0 counts +4231 down from 50; sub_54EA40 moves +1972
 * by +10, -10 or -20.
 */
static const uint8_t k_cnt4231_orig[8] = { 0x84, 0xC0, 0x74, 0x22, 0x2C, 0x01, 0xDD, 0xD8 };
static const uint8_t k_cnt_m10_orig[5] = { 0xB9, 0xF6, 0xFF, 0xFF, 0xFF };
static const uint8_t k_cnt_m20_orig[5] = { 0xB9, 0xEC, 0xFF, 0xFF, 0xFF };
static const uint8_t k_cnt_p10_orig[7] = { 0x80, 0x87, 0xB4, 0x07, 0x00, 0x00, 0x0A };
#define ADDR_CNT4231  0x004803B5u
#define ADDR_CNT_M10  0x0054F355u
#define ADDR_CNT_M20  0x0054F3DDu
#define ADDR_CNT_P10  0x0054F40Au
extern void Cnt4231Stub(void);
extern void CntM10Stub(void);
extern void CntM20Stub(void);
extern void CntP10Stub(void);

/*
 * Floor buttons (sub_557B90, 0x557FEC): "if (hold timer <= 0) release, else timer -= dt" runs once per call, and the release
 * only sets the animation speed; the animation step runs before the obstacle class in the frame, so the button leaves the
 * pressed state two calls after the timer reached zero: 33 ms at 60 fps, 3 ms at 600 fps. The game is balanced on that
 * lag: the five buttons of the Death Star Escape row, 0.5 units apart, are crossed by the cart (1.8 units/s) in 1.111 s,
 * the hold is 1.1 s, and only the 33 ms lag makes all five lit at the same time. The release now waits until the timer is
 * -(2/60)(1 - min(1, 60 dt)) (0 at 60 fps, 33 ms minus the two calls of lag at high rates), mode 2 only, so the lag is
 * 2/60 s at any rate.
 */
static const uint8_t k_release_orig[5] = { 0xD9, 0xEE, 0xD8, 0x5E, 0x40 };
#define ADDR_RELEASE_CMP 0x00557FECu
extern void ReleaseSlackStub(void);

/*
 * Rope / zipline swing (sub_4D6060, 0x4D6DBA..0x4D6DDD): every call the rope offset vector (+68..+76 of the rope) is
 * multiplied by 0.9 and 0.5 * hero+180 is added to its height: p = 0.9 p + 0.5 h, equilibrium 5 h. Scaling only the 0.9
 * (above) gave 0.5 h / (1 - 0.9^s), 13 times too much at 850 fps, so the rope hung far too low. The added term must
 * follow: over s steps of the 60 fps rule the sum is 0.5 h (1 - 0.9^s) / 0.1, so the load of hero+180 is multiplied
 * by (1 - 0.9^s) / 0.1 (1 at 60 fps).
 */
static const uint8_t k_rope_imp_orig[6] = { 0xD9, 0x86, 0xB4, 0x00, 0x00, 0x00 };
#define ADDR_ROPE_IMP 0x004D6DDDu
extern void RopeImpulseStub(void);

/*
 * Loose building-block pieces (GizBuildit, update sub_5912E0 via the class dispatch sub_559CD0): each piece that
 * is not twitching rolls "random < p" on every call, p = 0.001 to 0.1, and twitches for 0.2 s on success, so the
 * pieces on the ground twitched about 14 times as often at 850 fps. The roll's random number (call at 0x591F2B) is
 * replaced by one that always fails on frames without a 60 Hz tick (g_steps), so the roll happens 60 times a second.
 */
#define ADDR_BUILDIT_ROLL 0x00591F2Bu
#define ADDR_RANDOM_FN    0x0054FA60u

/*
 * sub_5A1480(anim, offset) adds a vector to the current position of an animated object (read the matrix, add, write
 * it back). sub_468B50 (gizmo class with the "LegShake" animation objects) calls it on every call: a random +-0.025
 * shake per axis and the stack height, three sites. Per call, so the shake grew with the frame rate. The three calls
 * now go through ShakeOffset, which makes it once per 60 Hz tick (g_steps times). The other callers (sub_467200,
 * sub_4688C0, sub_467580) place objects once and are left alone.
 */
#define ADDR_SHAKE_FN 0x005A1480u
static const uintptr_t k_shake_calls[] = { 0x00468CC7, 0x00468FFE, 0x004690CD };
#define SHAKE_CALLS ((int)(sizeof k_shake_calls / sizeof k_shake_calls[0]))

/*
 * Speed boost +3448 of sub_4AC620 (a rate multiplier 1..5 on the velocity lerps). It ramps up by 5 per second, but
 * on every call where the ground gate fails it was set straight back to 1.0, so a ground flag that blips for one
 * call (1/850 s) threw away what 0.8 s of contact had built. It now drops by 240 per second (a 1/60 s blip still
 * ends at 1.0, as at 60 fps).
 * Saber clash push, sub_49FA50: table[+24] * direction is added to the other character's velocity on every call
 * while the sabers overlap; scaled by the step like the other per-call pushes.
 */
static const uint8_t k_boost_orig[8] = { 0xD9, 0xE8, 0xD9, 0x9E, 0x78, 0x0D, 0x00, 0x00 };
static const uint8_t k_clash1_orig[6] = { 0xD9, 0x41, 0x18, 0x83, 0xC4, 0x08 };
static const uint8_t k_clash2_orig[7] = { 0xD9, 0x42, 0x18, 0xD8, 0x4C, 0x24, 0x20 };
#define ADDR_BOOST_RESET 0x004AEE14u
#define ADDR_CLASH1      0x004A039Au
#define ADDR_CLASH2      0x004A03B0u
extern void BoostResetStub(void);

/*
 * sub_54FB40(cur, target, rate) is the game's smoothing step used by 80 callers (hero velocity, cart/pushblock
 * momentum, camera ...): cur + (target - cur) * min(1, dt * rate). That explicit Euler step converges faster than
 * the exponential e^(-rate*t) it approximates, and the more so the larger dt is: 4 % faster at rate 5 and 21 %
 * faster at rate 20 with dt = 1/60. Everything was tuned at 60 fps, so at 850 fps the hero accelerated visibly
 * slower (the Death Star Escape cart takes 20 ms longer to get going and misses the 1.1 s window of the five
 * floor switches by 10 ms). The step now keeps the per-second decay of the 60 fps version: 1 - (1 - rate/60)^(60 dt).
 */
static const uint8_t k_lerp_orig[6] = { 0xD9, 0x05, 0x18, 0x50, 0x92, 0x00 };
#define ADDR_LERP 0x0054FB40u
extern void Clash1Stub(void);
extern void Clash2Stub(void);

/*
 * Particle emitter sub_410A10: lifetime counters +268 and +280 drop by one per call and the path position +216 moves
 * by flt_7F15DC per step. The counters drop only on frames with a 60 Hz tick (as many ticks as the original frame
 * would have counted, one at most), the path step shrinks with the frame time.
 */
static const uint8_t k_emit_dec_orig[7] = { 0x83, 0x83, 0x0C, 0x01, 0x00, 0x00, 0xFF };
static const uint8_t k_emit_life_orig[9] = { 0x83, 0xC0, 0xFF, 0x39, 0xB3, 0x0C, 0x01, 0x00, 0x00 };
static const uint8_t k_emit_path_orig[6] = { 0xD9, 0x05, 0xDC, 0x15, 0x7F, 0x00 };
#define ADDR_EMIT_DEC  0x00410A7Bu
#define ADDR_EMIT_LIFE 0x00410A82u
#define ADDR_EMIT_PATH 0x00410ED5u
extern void EmitDecStub(void);
extern void EmitLifeStub(void);
__attribute__((used)) static const uintptr_t emit_life_addr = ADDR_EMIT_LIFE, emit_life_back = 0x00410A8B;

static int RateSitesMatch(void)
{
    int i;

    for (i = 0; i < RATE_SITES; i++) {
        const uint8_t *b = (const uint8_t *)k_rate_sites[i].addr;

        if (b[0] != k_rate_sites[i].op[0] || b[1] != k_rate_sites[i].op[1] ||
            *(const uint32_t *)(b + 2) != k_rate_sites[i].cst)
            return 0;
    }
    for (i = 0; i < 2; i++)
        if (!CallSiteOk(k_rate_calls[i], ADDR_TRUNC_FN))
            return 0;
    return CallSiteOk(ADDR_BUILDIT_ROLL, ADDR_RANDOM_FN) &&
           CallSiteOk(k_shake_calls[0], ADDR_SHAKE_FN) && CallSiteOk(k_shake_calls[1], ADDR_SHAKE_FN) &&
           CallSiteOk(k_shake_calls[2], ADDR_SHAKE_FN) &&
           Matches(ADDR_EMIT_DEC, k_emit_dec_orig, sizeof k_emit_dec_orig) &&
           Matches(ADDR_EMIT_LIFE, k_emit_life_orig, sizeof k_emit_life_orig) &&
           Matches(ADDR_EMIT_PATH, k_emit_path_orig, sizeof k_emit_path_orig) &&
           Matches(ADDR_CNT4231, k_cnt4231_orig, sizeof k_cnt4231_orig) &&
           Matches(ADDR_CNT_M10, k_cnt_m10_orig, sizeof k_cnt_m10_orig) &&
           Matches(ADDR_CNT_M20, k_cnt_m20_orig, sizeof k_cnt_m20_orig) &&
           Matches(ADDR_CNT_P10, k_cnt_p10_orig, sizeof k_cnt_p10_orig) &&
           Matches(ADDR_ROPE_IMP, k_rope_imp_orig, sizeof k_rope_imp_orig) &&
           Matches(ADDR_RELEASE_CMP, k_release_orig, sizeof k_release_orig) &&
           Matches(ADDR_KNOCKBACK_AMOUNT, k_knock_orig, sizeof k_knock_orig) &&
           Matches(ADDR_OBJPUSH_AMOUNT, k_push_orig, sizeof k_push_orig) &&
           Matches(ADDR_BOOST_RESET, k_boost_orig, sizeof k_boost_orig) &&
           Matches(ADDR_CLASH1, k_clash1_orig, sizeof k_clash1_orig) &&
           Matches(ADDR_CLASH2, k_clash2_orig, sizeof k_clash2_orig) &&
           Matches(ADDR_LERP, k_lerp_orig, sizeof k_lerp_orig);
}

static const uint8_t k_approach_orig[7] = { 0x51, 0xD9, 0x05, 0x18, 0x50, 0x92, 0x00 };

static int PatchCallPad(uintptr_t addr, const void *fn, size_t len)
{
    DWORD old;
    uint8_t *p = (uint8_t *)addr;

    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    p[0] = 0xE8;
    *(int32_t *)(p + 1) = (int32_t)((uintptr_t)fn - (addr + 5));
    memset(p + 5, 0x90, len - 5);
    VirtualProtect(p, len, old, &old);
    return 1;
}

/*
 * sub_550100 (heading approach) truncates rate*dt to a whole number of angle units each call. With small steps
 * that loses up to a unit per call, or all of it: at 850 fps a turn slower than 850 units/s (4.6 deg/s) never
 * happens. The fraction is kept in expectation by rounding up with a probability equal to it.
 */
static int DitherRound(double x)
{
    static uint32_t rng = 0x9E3779B9u;
    int n = (int)x;                     /* toward zero */
    double frac = x - (double)n;

    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    if (frac != 0.0 && (double)(rng & 0xFFFFFF) < (frac < 0.0 ? -frac : frac) * 16777216.0)
        n += frac < 0.0 ? -1 : 1;
    return n;
}

int __cdecl DitherTruncate(double x)
{
    return DitherRound(x);
}

/*
 * sub_550070(from, to, rate): moves a 16-bit angle toward the target by (to - from) * min(1, dt * rate) and
 * returns the result as an integer (the step fraction matches the 60 fps response per second, see below). The original truncates, so with small steps a positive delta never
 * moves (it stalls up to 1 / (dt * rate) units short) and a negative delta always moves a whole unit. Same
 * rounding as above: the mean step is exact.
 */
static short __cdecl Approach(unsigned from, unsigned to, float rate)
{
    float dt = *(float *)ADDR_FRAME_DT, k = dt * rate;
    int a = (int)(from & 0xFFFF), d = (int)(to & 0xFFFF) - a;

    /* At 60 fps the step is the fraction rate/60 of the gap. Below 1/60 s the same response per second is
     * 1 - (1 - rate/60)^(60 dt); dt * rate alone lags for big rates (a rate of 60 or more snapped at 60 fps). */
    if (g_fix_gameplay && rate > 0.0f && dt < 1.0f / COUNTER_HZ) {
        float r = rate / COUNTER_HZ;

        k = r >= 1.0f ? 1.0f : 1.0f - powf(1.0f - r, dt * COUNTER_HZ);
    }
    if (k > 1.0f)
        k = 1.0f;
    if (d > 0x8000)
        d -= 0x10000;
    else if (d < -0x8000)
        d += 0x10000;
    return (short)(a + DitherRound((double)d * (double)k));
}

/*
 * Same, for the many "(int)(rate * dt)" truncations spread over the game (28 call sites of sub_6E6B90 that take a
 * dt product). The value is proportional to dt, so what it would have been at a 1/60 s step is known. Where that is
 * below 1 the original always truncated to 0 (also at 60 fps): that behaviour is kept. Where it is 1 or more, the
 * fraction is carried on average instead of being lost.
 */
int __cdecl ScaledDither(double x)
{
    double dt = *(float *)ADDR_FRAME_DT, nominal;

    if (dt <= 0.0)
        return (int)x;
    nominal = x * (1.0 / 60.0) / dt;
    if (nominal < 1.0 && nominal > -1.0)
        return (int)x;
    return DitherRound(x);
}

extern void DitherStub(void);
extern void ScaledDitherStub(void);

__asm__(
    ".text\n"
    ".globl _ScaledDitherStub\n"
    "_ScaledDitherStub:\n"
    "    pushl %ecx\n"
    "    pushl %edx\n"
    "    subl $8,%esp\n"
    "    fstpl (%esp)\n"
    "    call _ScaledDither\n"
    "    addl $8,%esp\n"
    "    popl %edx\n"
    "    popl %ecx\n"
    "    ret\n"
);

__asm__(
    ".text\n"
    ".globl _BoostResetStub\n"
    "_BoostResetStub:\n"
    "    flds 0xd78(%esi)\n"
    "    fsubs _g_boost_step\n"
    "    fstps 0xd78(%esi)\n"
    "    cmpl $0x3f800000,0xd78(%esi)\n"       /* float compare as int: negative results also sort below 1.0 */
    "    jge 1f\n"
    "    movl $0x3f800000,0xd78(%esi)\n"
    "1:  jmp *_boost_back\n"
    ".globl _Clash1Stub\n"
    "_Clash1Stub:\n"
    "    flds 0x18(%ecx)\n"
    "    fmuls _g_step_scale\n"
    "    addl $8,%esp\n"
    "    jmp *_clash1_back\n"
    ".globl _Clash2Stub\n"
    "_Clash2Stub:\n"
    "    flds 0x18(%edx)\n"
    "    fmuls _g_step_scale\n"
    "    fmuls 0x20(%esp)\n"
    "    jmp *_clash2_back\n"
);
__attribute__((used)) static const uintptr_t boost_back = 0x4AEE1C, clash1_back = 0x4A03A0, clash2_back = 0x4A03B7;

__asm__(
    ".text\n"
    ".globl _KnockbackStub\n"
    "_KnockbackStub:\n"
    "    flds 0x20(%esp)\n"
    "    fmuls _g_step_scale\n"
    "    fld %st(0)\n"
    "    ret\n"
    ".globl _ObjPushStub\n"
    "_ObjPushStub:\n"
    "    flds 0x10(%esp)\n"
    "    fmuls _g_step_scale\n"
    "    fld %st(0)\n"
    "    ret\n"
);

__asm__(
    ".text\n"
    ".globl _EmitDecStub\n"
    "_EmitDecStub:\n"
    "    cmpl $0,_g_steps\n"
    "    jle 1f\n"
    "    decl 0x10c(%ebx)\n"
    "1:  jmp *_emit_life_addr\n"
    ".globl _EmitLifeStub\n"
    "_EmitLifeStub:\n"
    "    cmpl $0,_g_steps\n"
    "    jle 1f\n"
    "    decl %eax\n"
    "1:  cmpl %esi,0x10c(%ebx)\n"
    "    jmp *_emit_life_back\n"
);

/* g_steps is a static int defined further down; the stubs only read it. */
__asm__(
    ".text\n"
    ".globl _CntM10Stub\n"
    "_CntM10Stub:\n"
    "    imull $-10,_g_steps,%ecx\n"
    "    ret\n"
    ".globl _CntM20Stub\n"
    "_CntM20Stub:\n"
    "    imull $-20,_g_steps,%ecx\n"
    "    ret\n"
    ".globl _ReleaseSlackStub\n"
    "_ReleaseSlackStub:\n"
    "    flds _g_release_neg\n"
    "    cmpl $2,%ecx\n"
    "    je 1f\n"
    "    fstp %st(0)\n"
    "    fldz\n"
    "1:  fcomps 0x40(%esi)\n"
    "    ret\n"
    ".globl _RopeImpulseStub\n"
    "_RopeImpulseStub:\n"
    "    flds 0xb4(%esi)\n"
    "    fmuls _g_rope_imp\n"
    "    ret\n"
    ".globl _CntP10Stub\n"
    "_CntP10Stub:\n"
    "    pushl %eax\n"
    "    imull $10,_g_steps,%eax\n"
    "    addb %al,0x7b4(%edi)\n"
    "    popl %eax\n"
    "    ret\n"
    /* replaces "test al,al / je 4803DB / sub al,1 / fstp st0": subtract g_steps (not below 0) from the counter */
    ".globl _Cnt4231Stub\n"
    "_Cnt4231Stub:\n"
    "    testb %al,%al\n"
    "    jz 1f\n"
    "    pushl %ecx\n"
    "    movb _g_steps,%cl\n"
    "    cmpb %al,%cl\n"
    "    jb 2f\n"
    "    xorb %al,%al\n"
    "    jmp 3f\n"
    "2:  subb %cl,%al\n"
    "3:  popl %ecx\n"
    "    fstp %st(0)\n"
    "    jmp *_cnt4231_back\n"
    "1:  jmp *_cnt4231_zero\n"
);
__attribute__((used)) static const uintptr_t cnt4231_back = 0x4803BD, cnt4231_zero = 0x4803DB;

__asm__(
    ".text\n"
    ".globl _DitherStub\n"
    "_DitherStub:\n"
    "    pushl %ecx\n"
    "    pushl %edx\n"
    "    subl $8,%esp\n"
    "    fstpl (%esp)\n"
    "    call _DitherTruncate\n"
    "    addl $8,%esp\n"
    "    popl %edx\n"
    "    popl %ecx\n"
    "    ret\n"
);

/*
 * Debug (ProbeSweep=1): trace of the swept collision for player 1, to compare frame rates. Logged: the step
 * asked for, the contact that was found (hit flag, type, normal) and where the sphere ended up.
 */
static const uint8_t k_sweep_orig[9] = { 0x8B, 0x15, 0x68, 0xFC, 0x92, 0x02, 0x83, 0xEC, 0x14 };

typedef void (__cdecl *SweepFn)(float *, float *, uint8_t *, short, float, float, float, int, int, int);

static SweepFn g_sweep_orig;

static float g_jump_clock = 99.0f;   /* seconds since the last jump impulse of player 1 (trace only) */

static void __cdecl SweepProbe(float *pos, float *mv, uint8_t *flags, short idx, float a5, float a6, float a7,
                               int a8, int a9, int a10)
{
    static int lines;
    char *hero = (char *)*(int *)ADDR_PLAYER_OBJ;
    float in_pos[3], in_mv[3];
    int mine = g_probe_sweep && hero && (uint8_t *)(hero + 4176) == flags;

    if (mine) {
        memcpy(in_pos, pos, sizeof in_pos);
        memcpy(in_mv, mv, sizeof in_mv);
    }
    g_sweep_orig(pos, mv, flags, idx, a5, a6, a7, a8, a9, a10);
    if (mine && lines < 30000 && g_log) {
        short hit = *(short *)0x292125C;
        float *n = (float *)0x292FDB8;

        if (hit && fabsf(n[1]) < 0.9f) {
            fprintf(g_log, "S tj %.4f dt %.5f in(%.4f %.4f %.4f) mv(%.5f %.5f %.5f) v(%.3f %.3f %.3f) -> hit %d type %.0f n(%.3f %.3f %.3f) out(%.5f %.5f %.5f) omv(%.5f %.5f %.5f)\n",
                    g_jump_clock, *(float *)ADDR_FRAME_DT, in_pos[0], in_pos[1], in_pos[2], in_mv[0], in_mv[1], in_mv[2],
                    *(float *)(hero + 104), *(float *)(hero + 108), *(float *)(hero + 112), hit,
                    *(float *)0x2947B68, n[0], n[1], n[2], pos[0] - in_pos[0], pos[1] - in_pos[1], pos[2] - in_pos[2],
                    mv[0], mv[1], mv[2]);
            lines++;
        }
    }
}

#define SMOOTH60 0.19f   /* fitted: the walk easing is 1/6 per 1/60 s, 0.19 matches 60 fps best (ratio 0.99-1.01 over a jump) */

int g_fix_contact = 1;

/* The call at ADDR_VEL_REBUILD: dst = src * inv_dt. At short steps part of the old velocity survives a contact. */
static float *__cdecl VelRebuild(float *dst, float *src, float inv_dt)
{
    float pre[3], s;
    int i;

    memcpy(pre, dst, sizeof pre);
    for (i = 0; i < 3; i++)
        dst[i] = src[i] * inv_dt;
    s = inv_dt > 0.0f ? COUNTER_HZ / inv_dt : 1.0f;
    /*
     * The walk/air speed eases to its target by SMOOTH60 of the gap per 1/60 s (0.2 of 1.2), a fraction that scales
     * with the step. Against a wall the speed that enters the sweep settles at f*v/(s + f*(1 - s)) with the blend s;
     * the 60 fps value (s = 1) is f60*v. s is lowered so that both agree (measured: 0.172 instead of 0.200 with s = 60 dt; 0.19 instead of 1/6 fits the measured jumps).
     */
    if (g_fix_contact && s < 1.0f) {
        s = s * (1.0f - SMOOTH60) / (1.0f - SMOOTH60 * s);   /* only for steps below 1/60 s: the divisor stays positive */
        for (i = 0; i < 3; i++)
            dst[i] = s * dst[i] + (1.0f - s) * pre[i];
    }
    return dst;
}

static int VelRebuildSiteOk(void)
{
    const uint8_t *b = (const uint8_t *)ADDR_VEL_REBUILD;

    return b[0] == 0xE8 && (uint32_t)(ADDR_VEL_REBUILD + 5 + *(const int32_t *)(b + 1)) == ADDR_VEL_REBUILD_FN;
}

/*
 * Step-up probe. sub_68EE40 drops a copy of the hero's ellipsoid by a fixed 2 * step height (0.2) to find the
 * ledge under it; the hero is snapped onto it if it is within half a step (0.05) of the hero. The sweep stops
 * short of the surface by "veryClose fraction * vector length" plus the push-out offsets. Both are scaled for
 * vectors of move * dt, but this vector is a fixed length: at 850 fps the probe stopped 0.014 above the ledge
 * instead of 0.001, so the hero had to rise 0.013 higher before the step-up fired (the apex of the jump on the
 * fallen tree of Episode VI is 0.014 short of it). The probe runs with the 60 fps tolerances.
 */
static int __cdecl StepUpProbe(int a1, float *a2)
{
    float *tol = (float *)(*(int *)ADDR_WORK_BLOCK + 160);
    float tol_saved = *tol;
    double push_saved[PUSH_GROUPS];
    int r;

    memcpy(push_saved, g_push, sizeof push_saved);
    memcpy(g_push, k_push_base, sizeof g_push);
    *tol = 0.005f;
    r = ((int (__cdecl *)(int, float *))ADDR_STEPUP_FN)(a1, a2);
    *tol = tol_saved;
    memcpy(g_push, push_saved, sizeof g_push);
    return r;
}

/*
 * Frame limiter sub_4FD050 aims at max(refresh, limit) (the 85.0 constants, redirected to the MaxFPS value), so a
 * limit below the refresh rate was ignored. Its four calls of the refresh getter return min(refresh, limit) instead:
 * a limit under the refresh rate then wins, the other cases come out as before.
 */
static const uintptr_t k_limiter_calls[] = ADDR_LIMITER_CALLS;
#define LIMITER_CALLS ((int)(sizeof k_limiter_calls / sizeof k_limiter_calls[0]))

static unsigned __cdecl LimiterRefresh(void)
{
    unsigned hz = ((unsigned (__cdecl *)(void))ADDR_REFRESH_FN)();

    return g_max_fps > 0 && hz > (unsigned)g_max_fps ? (unsigned)g_max_fps : hz;
}

static int LimiterCallsOk(void)
{
    int i;

    for (i = 0; i < LIMITER_CALLS; i++)
        if (!CallSiteOk(k_limiter_calls[i], ADDR_REFRESH_FN))
            return 0;
    return 1;
}

/*
 * Trails (the lightsaber streak). sub_5A3730 adds one segment per call, each living 0.25 s, from a pool of 128
 * segments; the saber code calls it once per frame. At 60 fps that is 15 segments per trail, at 850 fps 212: the pool
 * ran dry during the first swing and the trails never came back (the segments were left unreachable: free list
 * empty, no active trail). A frame without a 60 Hz tick only marks the trail as still in use (field +16, and the
 * colour field +24 which the full call sets as well) instead of adding a segment.
 */
static int g_steps;     /* 60 Hz ticks of this frame, defined below */
static const uintptr_t k_streak_calls[] = ADDR_STREAK_CALLS;
#define STREAK_CALLS ((int)(sizeof k_streak_calls / sizeof k_streak_calls[0]))

typedef void (__cdecl *StreakFn)(int, float, int, int *, char, int);

static void __cdecl StreakAdd(int pts, float life, int color, int *owner, char flag, int entity)
{
    if (g_fix_gameplay && g_steps < 1 && *owner) {
        *(int *)(*owner + 16) = 1;
        *(int *)(*owner + 24) = color;
        return;
    }
    ((StreakFn)ADDR_STREAK_FN)(pts, life, color, owner, flag, entity);
}

static int StreakCallsOk(void)
{
    int i;

    for (i = 0; i < STREAK_CALLS; i++)
        if (!CallSiteOk(k_streak_calls[i], ADDR_STREAK_FN))
            return 0;
    return 1;
}

/*
 * Saber sparks. sub_62C350(rate, dt) rolls how many events a rate per second gives in one frame (0 or 1 at the
 * saber's 10 and 100 per second), and each event emits particles over a window of one frame (sub_656DC0 passes the
 * frame time as the window). The window shrinks with the frame time as well, so the number of sparks per second fell
 * with the square of it: 14x fewer at 850 fps. The rolls use the time of one 60 Hz frame at most, as at 60 fps.
 */
static const uintptr_t k_spark_calls[] = ADDR_SPARK_CALLS;
#define SPARK_CALLS ((int)(sizeof k_spark_calls / sizeof k_spark_calls[0]))

typedef unsigned (__cdecl *SparkRateFn)(float, float);

static unsigned __cdecl SparkRate(float rate, float dt)
{
    return ((SparkRateFn)ADDR_SPARK_RATE_FN)(rate, dt < 1.0f / 60.0f ? 1.0f / 60.0f : dt);
}

static int SparkCallsOk(void)
{
    int i;

    for (i = 0; i < SPARK_CALLS; i++)
        if (!CallSiteOk(k_spark_calls[i], ADDR_SPARK_RATE_FN))
            return 0;
    return 1;
}

static float g_script_acc;
__attribute__((used)) static int g_steps = 1;

static int CallSiteOk(uint32_t site, uint32_t fn)
{
    const uint8_t *b = (const uint8_t *)site;

    return b[0] == 0xE8 && (uint32_t)(site + 5 + *(const int32_t *)(b + 1)) == fn;
}

static void *__cdecl ShakeOffset(int anim, float *offset)
{
    void *r = NULL;
    int i;

    for (i = 0; i < g_steps; i++)
        r = ((void *(__cdecl *)(int, float *))ADDR_SHAKE_FN)(anim, offset);
    return r;
}

/* sub_54FA60 (random 0..65535) for the twitch roll: a number that is never below the chance when there was no tick. */
static int __cdecl BuilditRoll(void)
{
    return g_steps > 0 ? ((int (__cdecl *)(void))ADDR_RANDOM_FN)() : 0x7FFFFFFF;
}

static int ScriptTickSiteOk(void)
{
    return CallSiteOk(ADDR_SCRIPT_TICK_CALL, ADDR_SCRIPT_TICK_FN);
}

/*
 * 60 Hz step count of the logic frame. sub_686950(1.0) (script/effect timeline) is called once per frame in
 * the original, here once per 1/60 s of real time, however many frames that takes. The foliage systems that
 * follow it in sub_491D50 are written the same way (per-call phase counters, fixed lerp factors, unit-step
 * springs and call-counted timers), so they run g_steps times per frame as well, which is exactly what they
 * did at 60 fps.
 */
static void __cdecl ScriptTick(float ticks)
{
    int n = 1;

    if (g_fix_counters) {
        g_script_acc += *(float *)ADDR_FRAME_DT * COUNTER_HZ;
        n = (int)(g_script_acc + 0.05f);
        if (n < 0)
            n = 0;
        g_script_acc -= (float)n;
        if (g_script_acc < 0.0f)
            g_script_acc = 0.0f;
    }
    g_steps = n;
    if (n < 1)
        return;
    ((void (__cdecl *)(float))ADDR_SCRIPT_TICK_FN)((float)n * ticks);
}

static int __cdecl StepChains(int a)
{
    int i, r = 0;

    for (i = 0; i < g_steps; i++)
        r = ((int (__cdecl *)(int))ADDR_CHAIN_FN)(a);
    return r;
}

static int __cdecl StepWind(int a)
{
    int i, r = 0;

    for (i = 0; i < g_steps; i++)
        r = ((int (__cdecl *)(int))ADDR_STRAND_FN)(a);
    return r;
}

static void __cdecl StepSway(int a)
{
    int i;

    for (i = 0; i < g_steps; i++)
        ((void (__cdecl *)(int))ADDR_SWAY_FN)(a);
}

static void __cdecl StepWobble(void)
{
    int i;

    for (i = 0; i < g_steps; i++)
        ((void (__cdecl *)(void))ADDR_WOBBLE_FN)();
}

static float g_wind_acc;

/* Replacement for "add dword_97390C, 1": the wind phase advances 60 times per second of real time. */
void __cdecl WindTick(void)
{
    int n = 1;

    if (g_fix_counters) {
        g_wind_acc += *(float *)ADDR_FRAME_DT;
        n = (int)(g_wind_acc * COUNTER_HZ + 0.05f);
        g_wind_acc -= (float)n / COUNTER_HZ;
        if (n < 0)
            n = 0;
    }
    *(int *)ADDR_WIND_PHASE += n;
}

extern void WindTickStub(void);

__asm__(
    ".text\n"
    ".globl _WindTickStub\n"
    "_WindTickStub:\n"
    "    pushfl\n"
    "    pushal\n"
    "    call _WindTick\n"
    "    popal\n"
    "    popfl\n"
    "    pushl $0x0060B558\n"
    "    ret\n"
);

/*
 * Debug (ProbeAnim=1): once per second, per animation list handed to sub_5F4B90, how often it was stepped and the sum of
 * the step argument (dt * 60). A list stepped once per frame with dt*60 shows ~60 per second at any frame rate; more
 * means it is advanced from more than one place.
 */
static const uint8_t k_anim_orig[7] = { 0x83, 0xEC, 0x24, 0x8B, 0x4C, 0x24, 0x28 };
#define ADDR_ANIM_STEP 0x005F4B90u
typedef int (__cdecl *AnimFn)(int, float);
static AnimFn g_anim_orig;
static struct { int list, calls; double sum; } g_anim_stat[8];

static int __cdecl AnimProbe(int list, float step)
{
    static DWORD last;
    DWORD now = GetTickCount();
    int i;

    for (i = 0; i < 8 && g_anim_stat[i].list && g_anim_stat[i].list != list; i++)
        ;
    if (i < 8) {
        g_anim_stat[i].list = list;
        g_anim_stat[i].calls++;
        g_anim_stat[i].sum += step;
    }
    if (!last)
        last = now;
    if (now - last >= 1000) {
        char buf[100];

        for (i = 0; i < 8 && g_anim_stat[i].list; i++) {
            snprintf(buf, sizeof buf, "A list %x calls %d sum %.1f", g_anim_stat[i].list, g_anim_stat[i].calls,
                     g_anim_stat[i].sum);
            Log(buf);
            g_anim_stat[i].calls = 0;
            g_anim_stat[i].sum = 0.0;
        }
        last = now;
    }
    return g_anim_orig(list, step);
}

int FramerateReady(void)
{
    return Matches(ADDR_LIMIT_CMP1, k_limit_cmp_orig, sizeof k_limit_cmp_orig) &&
           Matches(ADDR_LIMIT_LOAD1, k_limit_load_orig, sizeof k_limit_load_orig) &&
           Matches(ADDR_LIMIT_CMP2, k_limit_cmp_orig, sizeof k_limit_cmp_orig) &&
           Matches(ADDR_LIMIT_LOAD2, k_limit_load_orig, sizeof k_limit_load_orig) &&
           Matches(ADDR_REFRESH_LOAD, k_refresh_orig, sizeof k_refresh_orig) &&
           Matches(ADDR_CLOCK_UPDATE, k_clock_orig, sizeof k_clock_orig) &&
           Matches(ADDR_CLOSE_DIST_1, k_close_1_orig, sizeof k_close_1_orig) &&
           Matches(ADDR_CLOSE_DIST_2, k_close_2_orig, sizeof k_close_2_orig) &&
           PushSitesMatch() && VelRebuildSiteOk() && ScriptTickSiteOk() &&
           CallSiteOk(ADDR_STEPUP_CALL, ADDR_STEPUP_FN) &&
           LimiterCallsOk() && StreakCallsOk() && SparkCallsOk() && CallSiteOk(ADDR_CHAIN_CALL, ADDR_CHAIN_FN) && CallSiteOk(ADDR_STRAND_CALL, ADDR_STRAND_FN) &&
           CallSiteOk(ADDR_SWAY_CALL, ADDR_SWAY_FN) && CallSiteOk(ADDR_WOBBLE_CALL, ADDR_WOBBLE_FN) &&
           DampSitesMatch() &&
           CallSiteOk(ADDR_HEADING_TRUNC, ADDR_TRUNC_FN) && TruncSitesMatch() && RateSitesMatch() && Matches(ADDR_ANGLE_APPROACH, k_approach_orig, sizeof k_approach_orig) &&
           Matches(ADDR_WALLPUSH_STEP, k_wall_orig, sizeof k_wall_orig) &&
           Matches(ADDR_WIND_TICK, k_wind_orig, sizeof k_wind_orig);
}

void FramerateInstall(void)
{
    ApplyMaxFps();
    if (PatchPtr(ADDR_LIMIT_CMP1 + 2, &g_limit_d) && PatchPtr(ADDR_LIMIT_CMP2 + 2, &g_limit_d) &&
        PatchPtr(ADDR_LIMIT_LOAD1 + 2, &g_limit_f) && PatchPtr(ADDR_LIMIT_LOAD2 + 2, &g_limit_f))
        Log(g_max_fps > 0 ? "frame cap patched" : "frame cap removed");
    else
        Log("frame cap patch FAILED");

    {
        int i, ok = 1;

        for (i = 0; i < LIMITER_CALLS; i++)
            ok &= PatchCall(k_limiter_calls[i], LimiterRefresh);
        Log(ok ? "frame limiter honours limits below the refresh rate" : "frame limiter patch FAILED");
    }
    if (g_fix_gameplay) {
        int i, ok = 1;

        for (i = 0; i < STREAK_CALLS; i++)
            ok &= PatchCall(k_streak_calls[i], StreakAdd);
        Log(ok ? "trail segments added at 60 Hz" : "trail patch FAILED");

        ok = 1;
        for (i = 0; i < SPARK_CALLS; i++)
            ok &= PatchCall(k_spark_calls[i], SparkRate);
        Log(ok ? "saber sparks rolled at 60 Hz" : "saber spark patch FAILED");
    }
    Log(PatchJmp(ADDR_REFRESH_LOAD, RefreshStub, 9) ? "refresh rate patched" : "refresh rate patch FAILED");

    Log(PatchJmp(ADDR_CLOCK_UPDATE, ClockUpdate, 6) ? "frame counters patched" : "frame counters patch FAILED");
    Log(PatchPtr(ADDR_CLOSE_DIST_1 + 2, &g_close_dist) && PatchPtr(ADDR_CLOSE_DIST_2 + 2, &g_close_dist2) ?
        "collision tolerance scaled with the step" : "collision tolerance patch FAILED");
    {
        int i, ok = 1;

        for (i = 0; i < PUSH_SITES; i++)
            ok &= PatchPtr(k_push_sites[i].addr + 2, &g_push[k_push_sites[i].group]);
        Log(ok ? "contact response offsets scaled with the step" : "contact response patch FAILED");
    }
    if (g_probe_anim) {
        void *tramp = MakeTrampoline(k_anim_orig, sizeof k_anim_orig, ADDR_ANIM_STEP + sizeof k_anim_orig);

        if (tramp && Matches(ADDR_ANIM_STEP, k_anim_orig, sizeof k_anim_orig)) {
            g_anim_orig = (AnimFn)tramp;
            Log(PatchJmp(ADDR_ANIM_STEP, AnimProbe, sizeof k_anim_orig) ? "animation probe installed" : "animation probe FAILED");
        }
    }
    if (g_probe_sweep) {
        void *tramp = MakeTrampoline(k_sweep_orig, sizeof k_sweep_orig, ADDR_SWEEP_RESUME);

        if (tramp && Matches(ADDR_SWEEP, k_sweep_orig, sizeof k_sweep_orig)) {
            g_sweep_orig = (SweepFn)tramp;
            Log(PatchJmp(ADDR_SWEEP, SweepProbe, 9) ? "sweep probe installed" : "sweep probe FAILED");
        }
    }
    Log(PatchCall(ADDR_SCRIPT_TICK_CALL, ScriptTick) ? "effect script tick fixed to 60 Hz" : "script tick patch FAILED");
    Log(PatchCall(ADDR_CHAIN_CALL, StepChains) && PatchCall(ADDR_STRAND_CALL, StepWind) &&
        PatchCall(ADDR_SWAY_CALL, StepSway) && PatchCall(ADDR_WOBBLE_CALL, StepWobble) ?
        "foliage and chain systems stepped at 60 Hz" : "foliage step patch FAILED");
    {
        size_t i;
        int ok = 1;

        for (i = 0; i < sizeof k_damp_sites / sizeof k_damp_sites[0]; i++)
            ok &= PatchPtr(k_damp_sites[i].addr + 2, k_damp_sites[i].var);
        Log(ok ? "per-call hero damping scaled with the step" : "hero damping patch FAILED");
    }
    if (g_fix_gameplay)
        Log(PatchCall(ADDR_STEPUP_CALL, StepUpProbe) ? "step-up probe uses the 60 fps tolerances" : "step-up probe patch FAILED");
    Log(PatchWallStep() ? "wall-push timer scaled with the step" : "wall-push timer patch FAILED");
    if (g_fix_gameplay) {
        int i, ok = 1;

        for (i = 0; i < TRUNC_SITES; i++)
            ok &= PatchCall(k_trunc_sites[i], ScaledDitherStub);
        Log(ok ? "rate*dt truncations keep their fraction" : "truncation patch FAILED");
    }
    if (g_fix_gameplay) {
        int i, ok = 1;

        for (i = 0; i < RATE_SITES; i++)
            ok &= PatchPtr(k_rate_sites[i].addr + 2, k_rate_sites[i].var);
        for (i = 0; i < 2; i++)
            ok &= PatchCall(k_rate_calls[i], ScaledDitherStub);
        ok &= PatchCallPad(ADDR_KNOCKBACK_AMOUNT, KnockbackStub, sizeof k_knock_orig);
        ok &= PatchCallPad(ADDR_OBJPUSH_AMOUNT, ObjPushStub, sizeof k_push_orig);
        ok &= PatchJmp(ADDR_BOOST_RESET, BoostResetStub, sizeof k_boost_orig);
        ok &= PatchJmp(ADDR_CLASH1, Clash1Stub, sizeof k_clash1_orig);
        ok &= PatchJmp(ADDR_CLASH2, Clash2Stub, sizeof k_clash2_orig);
        ok &= PatchJmp(ADDR_LERP, Lerp60, sizeof k_lerp_orig);
        ok &= PatchJmp(ADDR_EMIT_DEC, EmitDecStub, sizeof k_emit_dec_orig);
        ok &= PatchJmp(ADDR_EMIT_LIFE, EmitLifeStub, sizeof k_emit_life_orig);
        g_path_step = *(const float *)0x007F15DC;   /* never 0 before the first logic frame */
        ok &= PatchPtr(ADDR_EMIT_PATH + 2, &g_path_step);
        ok &= PatchJmp(ADDR_CNT4231, Cnt4231Stub, sizeof k_cnt4231_orig);
        ok &= PatchCall(ADDR_BUILDIT_ROLL, BuilditRoll);
        for (i = 0; i < SHAKE_CALLS; i++)
            ok &= PatchCall(k_shake_calls[i], ShakeOffset);
        ok &= PatchCallPad(ADDR_CNT_M10, CntM10Stub, sizeof k_cnt_m10_orig);
        ok &= PatchCallPad(ADDR_CNT_M20, CntM20Stub, sizeof k_cnt_m20_orig);
        ok &= PatchCallPad(ADDR_CNT_P10, CntP10Stub, sizeof k_cnt_p10_orig);
        ok &= PatchCallPad(ADDR_ROPE_IMP, RopeImpulseStub, sizeof k_rope_imp_orig);
        ok &= PatchCallPad(ADDR_RELEASE_CMP, ReleaseSlackStub, sizeof k_release_orig);
        Log(ok ? "lightning, rotation, overlay, rope, knockback, push, saber clash, speed boost, smoothing step, counter, building-block twitch, object shake, rope and button release rates scaled" : "rate patch FAILED");
    }
    Log(PatchJmp(ADDR_ANGLE_APPROACH, Approach, 7) ? "angle smoothing rounding fixed" : "angle smoothing patch FAILED");
    Log(PatchCall(ADDR_HEADING_TRUNC, DitherStub) ? "heading step rounding fixed" : "heading step patch FAILED");
    Log(PatchCall(ADDR_VEL_REBUILD, VelRebuild) ? "contact velocity rebuild patched" : "contact velocity patch FAILED");
    Log(PatchJmp(ADDR_WIND_TICK, WindTickStub, 7) ? "foliage wind fixed to 60 Hz steps" : "wind patch FAILED");
}

/*
 * Game logic rate. The game's update (sub_491D50: input, movement, collision, effects) is written for
 * frame times around 1/60 s. With tiny steps its contact handling breaks: at 850 fps the hero reaches the
 * fallen tree of Episode VI "Speeder Showdown" but does not slide up its rounded top as it does at 60 fps,
 * and per-frame constants (idle drift, grass wobble) run too fast. The update therefore runs at most
 * g_logic_hz times per second with the accumulated frame time; drawing and presenting still happen on every
 * frame, so below that rate nothing changes.
 */
static float g_logic_acc;

/*
 * Presses and releases are one-frame pulses. Game pads: the per-frame device poll sets word 5 (pressed) and
 * word 6 (released) of the raw device record (0x917000 + 132 * dev) and clears them on the next frame; the
 * translation into the player's buttons happens inside the game update. Keyboards: key messages. A frame
 * that carries any such pulse always runs the logic, otherwise most presses would fall between two updates.
 */
#define PAD_RECORD_BASE   0x00917000u
#define PAD_RECORD_SIZE   132u
#define PAD_PRESSED_WORD  5
#define PAD_RELEASED_WORD 6

static volatile int g_key_event;
static WNDPROC g_orig_wndproc;

static LRESULT CALLBACK KeyWatchProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
        g_key_event = 1;
    return CallWindowProcA(g_orig_wndproc, wnd, msg, wp, lp);
}

static int InputPulse(void)
{
    HWND wnd = *(HWND *)ADDR_GAME_WINDOW;
    int dev, pulse = g_key_event;

    if (wnd && !g_orig_wndproc)
        g_orig_wndproc = (WNDPROC)SetWindowLongA(wnd, GWL_WNDPROC, (LONG)KeyWatchProc);
    g_key_event = 0;
    for (dev = 0; dev < DEVICE_COUNT; dev++) {
        const uint32_t *rec = (const uint32_t *)(PAD_RECORD_BASE + PAD_RECORD_SIZE * dev);

        if (rec[PAD_PRESSED_WORD] || rec[PAD_RELEASED_WORD])
            pulse = 1;
    }
    return pulse;
}

void FramerateGameFrame(int game, int *out)
{
    float *dt = (float *)ADDR_FRAME_DT, saved = *dt;

    if (g_logic_hz > 0) {
        int input = InputPulse();

        g_logic_acc += saved;
        if (g_logic_acc < 0.95f / (float)g_logic_hz && !input) {
            *out = 0;
            return;
        }
        *dt = g_logic_acc;
        g_logic_acc = 0.0f;
    }
    UpdateCollisionTolerance(*dt);
    {
        static float last_vy;
        char *hero = (char *)*(int *)ADDR_PLAYER_OBJ;
        float vy = hero ? *(float *)(hero + 108) : 0.0f;

        g_jump_clock += *dt;
        if (vy >= 1.0f && last_vy <= 0.5f)
            g_jump_clock = 0.0f;
        last_vy = vy;
    }
    if (g_fix_physics)
        JumpFix(*dt);
    ((FrameFn)ADDR_FRAME_FUNC)(game, out);
    if (g_probe_buttons)
        ButtonProbe(game, *dt);
    if (g_probe_lever)
        LeverProbe(*dt);
    if (g_probe_hero)
        HeroProbe(*dt);
    *dt = saved;
}


/*
 * Debug (ProbeButtons=1): obstacle records (160 bytes, list of the current group at game+10952) of type 2 and 6 (buttons).
 * A line whenever one changes: triggered flag +152, animation state (*(+52)+12: 0 up, 1 pressing, 2 down, 3 releasing),
 * hold timer +64 of +60, trigger object +56, flags +156.
 */
#define BTN_MAX 64
static struct { int trig, state, zero; uint32_t fl; } g_btn_last[BTN_MAX];

static void ButtonProbe(int game, float dt)
{
    static int lines;
    static double t;
    int **head = *(int ***)(game + 10952);
    char *grp;
    int n, i;
    int **list;

    t += dt;
    if (!head || !*head || lines >= 20000)
        return;
    grp = (char *)*head + 20 * *(int *)0x802644;
    n = *(int *)(grp + 4);
    list = *(int ***)(grp + 12);
    if (!list)
        return;
    for (i = 0; i < n && i < BTN_MAX; i++) {
        char *r = (char *)list[2 * i];
        char type, trig, state, zero;
        uint32_t fl;
        char buf[300];

        if (!r)
            continue;
        type = r[145];
        if (type != 2 && type != 6)
            continue;
        trig = *(int *)(r + 152) != 0;
        state = *(int *)(r + 52) ? (char)*(int *)(*(int *)(r + 52) + 12) : -1;
        zero = *(float *)(r + 64) <= 0.0f;
        fl = *(uint32_t *)(r + 156) & 0xFF;
        if (g_btn_last[i].trig == trig && g_btn_last[i].state == state && g_btn_last[i].zero == zero && g_btn_last[i].fl == fl)
            continue;
        g_btn_last[i].trig = trig;
        g_btn_last[i].state = state;
        g_btn_last[i].zero = zero;
        g_btn_last[i].fl = fl;
        char *hero = (char *)*(int *)ADDR_PLAYER_OBJ;

        snprintf(buf, sizeof buf, "B %.4f dt %.5f #%d at %.2f %.2f trig %d state %d timer %.3f/%.3f fl %x obj %x | hero f641 %d f638 %d f508 %x st %d",
                 t, dt, i, *(float *)(r + 28), *(float *)(r + 36), trig, state, *(float *)(r + 64), *(float *)(r + 60),
                 fl, *(unsigned *)(r + 56), hero ? hero[641] : -1, hero ? *(short *)(hero + 638) : -1,
                 hero ? *(unsigned *)(hero + 508) : 0, hero ? hero[1973] : -1);
        Log(buf);
        lines++;
    }
}

/*
 * Debug (ProbeLever=1): while player 1 is in the lever state (id in dword 0x802850) a line per frame with the crank
 * angle of the object (+124, 65536 per turn), its previous angle +128, flags +132 and the hero's grip state.
 */
static void LeverProbe(float dt)
{
    static int lines;
    static double t;
    char *hero = (char *)*(int *)ADDR_PLAYER_OBJ;
    int state = *(int *)0x00802850;
    char *obj;
    char buf[160];

    t += dt;
    if (!hero || state == -1 || (signed char)hero[1973] != state || lines >= 6000)
        return;
    obj = (char *)*(int *)(hero + 1944);
    if (!obj)
        return;
    snprintf(buf, sizeof buf, "L %.4f dt %.5f n %d angle %u prev %u fl %x kind %d", t, dt, g_steps,
             *(unsigned short *)(obj + 124), *(unsigned short *)(obj + 128), *(unsigned *)(obj + 132), hero[1974]);
    Log(buf);
    lines++;
}

/* Debug (ProbeHero=1): one line per logic frame for player 1: position, velocity, ground flags, input. */
static void HeroProbe(float dt)
{
    static int lines;
    static double t;
    char *hero = (char *)*(int *)ADDR_PLAYER_OBJ;
    char buf[256];
    int pad;

    t += dt;
    if (!hero || lines >= 200000 || *(float *)(hero + 92) < 35.0f)
        return;
    pad = *(int *)(hero + 3236) ? *(int *)(*(int *)(hero + 3236) + 4) : 0;
    snprintf(buf, sizeof buf, "H %.4f dt %.5f n %d p %.5f %.5f %.5f v %.4f %.4f %.4f f641 %d f508 %x in %x st %d %d t %.3f",
             t, dt, g_steps, *(float *)(hero + 92), *(float *)(hero + 96), *(float *)(hero + 100),
             *(float *)(hero + 104), *(float *)(hero + 108), *(float *)(hero + 112),
             *(uint8_t *)(hero + 641), *(uint32_t *)(hero + 508), pad,
             *(int8_t *)(hero + 1973), *(uint8_t *)(hero + 1976), *(float *)(hero + 3524));
    Log(buf);
    lines++;
}

/* Debug (LiveIni=1): the fix toggles and the FPS cap are re-read from the ini about once a second. */
static void LiveIniPoll(void)
{
    static int frames;
    static struct { const char *key; int *var; int def; int last; } k_live[] = {
        { "FixJumpPhysics", &g_fix_physics, 1, 1 },
        { "FixFrameCounters", &g_fix_counters, 1, 1 },
        { "LogicHz", &g_logic_hz, 0, 0 },
        { "FixContactVelocity", &g_fix_contact, 1, 1 },
        { "MaxFPS", &g_max_fps, 0, 0 },
    };
    static int primed;
    size_t i;

    if (!primed) {            /* the values loaded at start are the baseline */
        for (i = 0; i < sizeof k_live / sizeof k_live[0]; i++)
            k_live[i].last = *k_live[i].var;
        primed = 1;
    }
    if (++frames < 60)
        return;
    frames = 0;
    for (i = 0; i < sizeof k_live / sizeof k_live[0]; i++) {
        *k_live[i].var = GetPrivateProfileIntA("SagaTweaks", k_live[i].key, k_live[i].def, g_ini_path);
        if (*k_live[i].var != k_live[i].last) {
            char line[96];

            snprintf(line, sizeof line, "live ini: %s %d -> %d", k_live[i].key, k_live[i].last, *k_live[i].var);
            Log(line);
            k_live[i].last = *k_live[i].var;
        }
    }
    ApplyMaxFps();
}

/*
 * The game's limiter sleeps whole milliseconds (truncated), so it overshoots: a limit of 90 gave 97 fps. This waits for
 * the rest of the frame period once per frame: Sleep(1) while more than 2 ms are left, then a spin on the performance
 * counter. It only ever adds time, so a limit above what the game itself allows changes nothing.
 */
static void PaceFrame(void)
{
    static LARGE_INTEGER freq, next;
    LARGE_INTEGER now;
    LONGLONG period;

    if (g_max_fps <= 0) {
        next.QuadPart = 0;
        return;
    }
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    period = freq.QuadPart / g_max_fps;
    QueryPerformanceCounter(&now);
    if (!next.QuadPart || now.QuadPart - next.QuadPart > period * 4) {   /* first frame, or after a pause or level load */
        next.QuadPart = now.QuadPart + period;
        return;
    }
    while (now.QuadPart < next.QuadPart) {
        if (next.QuadPart - now.QuadPart > freq.QuadPart / 500)
            Sleep(1);
        QueryPerformanceCounter(&now);
    }
    next.QuadPart += period;
    if (next.QuadPart < now.QuadPart)       /* late: do not try to catch up */
        next.QuadPart = now.QuadPart + period;
}

void FramerateFrame(void)
{
    PaceFrame();
    if (g_live_ini)
        LiveIniPoll();
    if (g_log_fps)
        LogFps();
}
