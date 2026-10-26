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
int g_fix_gameplay = 1;
int g_logic_hz = 120;   /* upper bound of game logic updates per second, 0 = every frame */
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
__attribute__((used)) static float g_path_step = 0.0f;   /* flt_7F15DC * g_step_scale */
__attribute__((used)) static float g_step_scale = 1.0f;    /* min(1, dt*60) of the current logic frame */

static void HeroProbe(float dt);

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
    g_path_step = *(const float *)0x007F15DC * scale;
    g_damp75 = pow(0.75, scale);
    g_damp50 = pow(0.5, scale);
    g_wall_step = 4.0 * dt < 2.0 / 30.0 ? 4.0 * dt : 2.0 / 30.0;
    g_arc_kick = (float)(0.2000000029802322 * scale);
    g_rot_m16 = -16.0 * scale;
    g_rot_p16 = 16.0 * scale;
    g_overlay_ms = 33.333335876464844 * scale;
    g_rope_damp = (float)pow(0.8999999761581421, scale);
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
    { ADDR_DAMP_PUSH,   { 0xDD, 0x05 }, 0x007516D8u, &g_damp50 },
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

float __cdecl DecayPow(float x)      /* x^scale, used by the blend stub below */
{
    return x <= 0.0f ? 0.0f : powf(x, g_step_scale);
}

extern void BlendDecayStub(void);

/* Replaces "fsubrp st(1),st / fstp dword [esp+0Ch]" at 4AEB53: stores (1 - blend)^scale instead of 1 - blend. */
__asm__(
    ".text\n"
    ".globl _BlendDecayStub\n"
    "_BlendDecayStub:\n"
    "    .byte 0xde,0xe1\n"
    "    pushfl\n"
    "    pushl %eax\n"
    "    pushl %ecx\n"
    "    pushl %edx\n"
    "    subl $4,%esp\n"
    "    fstps (%esp)\n"
    "    call _DecayPow\n"
    "    addl $4,%esp\n"
    "    popl %edx\n"
    "    popl %ecx\n"
    "    popl %eax\n"
    "    popfl\n"
    "    fstps 0x10(%esp)\n"
    "    ret\n"
);

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
    return Matches(ADDR_EMIT_DEC, k_emit_dec_orig, sizeof k_emit_dec_orig) &&
           Matches(ADDR_EMIT_LIFE, k_emit_life_orig, sizeof k_emit_life_orig) &&
           Matches(ADDR_EMIT_PATH, k_emit_path_orig, sizeof k_emit_path_orig) &&
           Matches(ADDR_CNT4231, k_cnt4231_orig, sizeof k_cnt4231_orig) &&
           Matches(ADDR_CNT_M10, k_cnt_m10_orig, sizeof k_cnt_m10_orig) &&
           Matches(ADDR_CNT_M20, k_cnt_m20_orig, sizeof k_cnt_m20_orig) &&
           Matches(ADDR_CNT_P10, k_cnt_p10_orig, sizeof k_cnt_p10_orig) &&
           Matches(ADDR_KNOCKBACK_AMOUNT, k_knock_orig, sizeof k_knock_orig) &&
           Matches(ADDR_OBJPUSH_AMOUNT, k_push_orig, sizeof k_push_orig);
}

static const uint8_t k_approach_orig[7] = { 0x51, 0xD9, 0x05, 0x18, 0x50, 0x92, 0x00 };
static const uint8_t k_blend_orig[6] = { 0xDE, 0xE1, 0xD9, 0x5C, 0x24, 0x0C };

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
 * returns the result as an integer. The original truncates, so with small steps a positive delta never
 * moves (it stalls up to 1 / (dt * rate) units short) and a negative delta always moves a whole unit. Same
 * rounding as above: the mean step is exact.
 */
static short __cdecl Approach(unsigned from, unsigned to, float rate)
{
    float k = *(float *)ADDR_FRAME_DT * rate;
    int a = (int)(from & 0xFFFF), d = (int)(to & 0xFFFF) - a;

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

static float g_script_acc;
__attribute__((used)) static int g_steps = 1;

static int CallSiteOk(uint32_t site, uint32_t fn)
{
    const uint8_t *b = (const uint8_t *)site;

    return b[0] == 0xE8 && (uint32_t)(site + 5 + *(const int32_t *)(b + 1)) == fn;
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
           LimiterCallsOk() && CallSiteOk(ADDR_CHAIN_CALL, ADDR_CHAIN_FN) && CallSiteOk(ADDR_STRAND_CALL, ADDR_STRAND_FN) &&
           CallSiteOk(ADDR_SWAY_CALL, ADDR_SWAY_FN) && CallSiteOk(ADDR_WOBBLE_CALL, ADDR_WOBBLE_FN) &&
           DampSitesMatch() && Matches(ADDR_DAMP_BLEND, k_blend_orig, sizeof k_blend_orig) &&
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
        ok &= PatchCallPad(ADDR_DAMP_BLEND, BlendDecayStub, sizeof k_blend_orig);
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
        ok &= PatchJmp(ADDR_EMIT_DEC, EmitDecStub, sizeof k_emit_dec_orig);
        ok &= PatchJmp(ADDR_EMIT_LIFE, EmitLifeStub, sizeof k_emit_life_orig);
        g_path_step = *(const float *)0x007F15DC;   /* never 0 before the first logic frame */
        ok &= PatchPtr(ADDR_EMIT_PATH + 2, &g_path_step);
        ok &= PatchJmp(ADDR_CNT4231, Cnt4231Stub, sizeof k_cnt4231_orig);
        ok &= PatchCallPad(ADDR_CNT_M10, CntM10Stub, sizeof k_cnt_m10_orig);
        ok &= PatchCallPad(ADDR_CNT_M20, CntM20Stub, sizeof k_cnt_m20_orig);
        ok &= PatchCallPad(ADDR_CNT_P10, CntP10Stub, sizeof k_cnt_p10_orig);
        Log(ok ? "lightning, rotation, overlay, rope, knockback, push and counter rates scaled" : "rate patch FAILED");
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
    if (g_probe_hero)
        HeroProbe(*dt);
    *dt = saved;
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

    if (++frames < 60)
        return;
    frames = 0;
    g_fix_physics = GetPrivateProfileIntA("SagaTweaks", "FixJumpPhysics", 1, g_ini_path);
    g_fix_counters = GetPrivateProfileIntA("SagaTweaks", "FixFrameCounters", 1, g_ini_path);
    g_logic_hz = GetPrivateProfileIntA("SagaTweaks", "LogicHz", 120, g_ini_path);
    g_fix_contact = GetPrivateProfileIntA("SagaTweaks", "FixContactVelocity", 1, g_ini_path);
    g_max_fps = GetPrivateProfileIntA("SagaTweaks", "MaxFPS", 0, g_ini_path);
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
