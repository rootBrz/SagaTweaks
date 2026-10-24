/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Frame rate: FPS cap, refresh rate, and frame-rate independence of clocks and jump physics.
 */
#include "all.h"

static const uint8_t k_limit_cmp_orig[6]   = { 0xDC, 0x1D, 0x30, 0xEC, 0x75, 0x00 };

static const uint8_t k_limit_load_orig[6]  = { 0xD9, 0x05, 0x2C, 0xEC, 0x75, 0x00 };

static const uint8_t k_clock_orig[6]       = { 0x8B, 0x4C, 0x24, 0x04, 0xD9, 0x01 };

static const uint8_t k_phys_orig[9]        = { 0x83, 0xEC, 0x18, 0xD9, 0x05, 0xDC, 0xB8, 0x87, 0x00 };

static const uint8_t k_posvy_orig[4]       = { 0x18, 0x50, 0x92, 0x00 };

static const uint8_t k_refresh_orig[9]     = { 0x8B, 0x40, 0x08, 0x89, 0x86, 0x68, 0x01, 0x00, 0x00 };

static double g_limit_d = UNLIMITED_FPS;

static float  g_limit_f = (float)UNLIMITED_FPS; /* button code reported by the input scan for Start */
int g_max_fps; /* 0 = unlimited (vsync decides) */
int g_refresh_override; /* 0 = desktop refresh rate */
int g_fix_counters = 1; /* make per-frame counters tick at 60 Hz */
int g_fix_physics = 1;

int g_log_fps;

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
            fprintf(g_log, "fps %.1f, worst frame %.1f ms\n", frames / span, worst);
            fflush(g_log);
        }
        lines++;
        frames = 0;
        worst = 0;
        start = now;
    }
}

static PhysFn g_phys_orig;

static float g_dt_phys;

static float g_phys_acc;

/* Called once per frame: how much 60 Hz physics time passed. */
static void UpdatePhysicsStep(void)
{
    float dt = *(float *)ADDR_FRAME_DT;
    int ticks;

    if (!g_fix_physics) {
        g_dt_phys = dt;                          /* the patched code then sees the real frame time */
        g_phys_acc = 0.0f;
        return;
    }
    g_phys_acc += dt;
    ticks = (int)(g_phys_acc * 60.0f + 0.05f);   /* tolerance: exactly 60 fps is one tick every frame */
    if (ticks > 0) {
        g_dt_phys = (float)ticks / 60.0f;
        g_phys_acc -= g_dt_phys;
        if (g_phys_acc < 0.0f)
            g_phys_acc = 0.0f;
    } else {
        g_dt_phys = 0.0f;
    }
}

static void __cdecl PhysWrapper(int player, int *a2, float a3, float a4, float *a5)
{
    float saved = *(float *)ADDR_FRAME_DT;

    *(float *)ADDR_FRAME_DT = g_dt_phys;
    g_phys_orig(player, a2, a3, a4, a5);
    *(float *)ADDR_FRAME_DT = saved;
}
int FramerateReady(void)
{
    return Matches(ADDR_LIMIT_CMP1, k_limit_cmp_orig, sizeof k_limit_cmp_orig) &&
           Matches(ADDR_LIMIT_LOAD1, k_limit_load_orig, sizeof k_limit_load_orig) &&
           Matches(ADDR_LIMIT_CMP2, k_limit_cmp_orig, sizeof k_limit_cmp_orig) &&
           Matches(ADDR_LIMIT_LOAD2, k_limit_load_orig, sizeof k_limit_load_orig) &&
           Matches(ADDR_REFRESH_LOAD, k_refresh_orig, sizeof k_refresh_orig) &&
           Matches(ADDR_CLOCK_UPDATE, k_clock_orig, sizeof k_clock_orig) &&
           Matches(ADDR_PHYS_VERTICAL, k_phys_orig, sizeof k_phys_orig) &&
           Matches(ADDR_POS_VY_1, k_posvy_orig, sizeof k_posvy_orig) &&
           Matches(ADDR_POS_VY_2, k_posvy_orig, sizeof k_posvy_orig) &&
           Matches(ADDR_POS_VY_3, k_posvy_orig, sizeof k_posvy_orig) &&
           Matches(ADDR_POS_VY_4, k_posvy_orig, sizeof k_posvy_orig);
}

void FramerateInstall(void)
{
    void *tramp;
    int ok;

    ApplyMaxFps();
    if (PatchPtr(ADDR_LIMIT_CMP1 + 2, &g_limit_d) && PatchPtr(ADDR_LIMIT_CMP2 + 2, &g_limit_d) &&
        PatchPtr(ADDR_LIMIT_LOAD1 + 2, &g_limit_f) && PatchPtr(ADDR_LIMIT_LOAD2 + 2, &g_limit_f))
        Log(g_max_fps > 0 ? "frame cap patched" : "frame cap removed");
    else
        Log("frame cap patch FAILED");

    Log(PatchJmp(ADDR_REFRESH_LOAD, RefreshStub, 9) ? "refresh rate patched" : "refresh rate patch FAILED");

    tramp = MakeTrampoline(k_phys_orig, sizeof k_phys_orig, ADDR_PHYS_RESUME);
    if (tramp) {
        g_phys_orig = (PhysFn)tramp;
        ok = PatchJmp(ADDR_PHYS_VERTICAL, PhysWrapper, 9) && PatchPtr(ADDR_POS_VY_1, &g_dt_phys) &&
             PatchPtr(ADDR_POS_VY_2, &g_dt_phys) && PatchPtr(ADDR_POS_VY_3, &g_dt_phys) &&
             PatchPtr(ADDR_POS_VY_4, &g_dt_phys);
        Log(ok ? "vertical physics fixed to 60 Hz steps" : "vertical physics patch FAILED");
    }

    Log(PatchJmp(ADDR_CLOCK_UPDATE, ClockUpdate, 6) ? "frame counters patched" : "frame counters patch FAILED");
}

void FramerateFrame(void)
{
    UpdatePhysicsStep();
    if (g_log_fps)
        LogFps();
}
