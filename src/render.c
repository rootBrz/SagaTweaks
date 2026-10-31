/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * D3D9 device hooks: Present (frame statistics), SetTexture / CreateTexture (button prompts, texture
 * dump), shader constant probe (debug), and the video mode table's refresh rates. The render scale itself is a
 * backbuffer size and lives in window.c.
 */
#include "all.h"

static void **g_hooked_vtable;

int g_probe_consts;   /* debug: log shader constants that change every frame */

void ProbeFrame(void);

#define VT_PRESENT 17

#define VT_CREATE_TEXTURE 23

typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);

typedef HRESULT (STDMETHODCALLTYPE *SetConstFFn)(IDirect3DDevice9 *, UINT, const float *, UINT);

typedef HRESULT (STDMETHODCALLTYPE *CreateTextureFn)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);

static PresentFn g_orig_present;

static SetConstFFn g_orig_set_vs_const;

static CreateTextureFn g_orig_create_texture;

LONGLONG g_present_ticks;   /* time spent inside Present, for LogFPS */

/*
 * Debug (ProbeConsts=1): which vertex shader constants change from frame to frame, and how fast.
 * The first value set for a register in a frame is compared with the one of the previous frame; a
 * constant that moves the same amount per frame at any fps is a per-frame clock.
 */
static struct { float first[4], prev[4]; int set, valid; double sum; int frames; } g_probe[256];

static void ProbeSet(UINT start, const float *data, UINT count)
{
    UINT i;

    for (i = 0; i < count && start + i < 256; i++)
        if (!g_probe[start + i].set) {
            memcpy(g_probe[start + i].first, data + 4 * i, 16);
            g_probe[start + i].set = 1;
        }
}

void ProbeFrame(void)
{
    static LARGE_INTEGER freq, last;
    LARGE_INTEGER now;
    int r, k;

    for (r = 0; r < 256; r++) {
        if (!g_probe[r].set)
            continue;
        if (g_probe[r].valid) {
            for (k = 0; k < 4; k++)
                g_probe[r].sum += fabs((double)g_probe[r].first[k] - (double)g_probe[r].prev[k]);
            g_probe[r].frames++;
        }
        memcpy(g_probe[r].prev, g_probe[r].first, 16);
        g_probe[r].valid = 1;
        g_probe[r].set = 0;
    }
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        last = now;
    }
    if ((double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart >= 3.0) {
        static int lines;
        double span = (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart;

        if (g_log && lines++ < 12) {
            for (r = 0; r < 256; r++)
                if (g_probe[r].frames > 10 && g_probe[r].sum > 0.0)
                    fprintf(g_log, "probe c%d: %.3f/s, %.2f/frame, now %.3f %.3f %.3f %.3f\n", r, g_probe[r].sum / span,
                            g_probe[r].sum / g_probe[r].frames, g_probe[r].prev[0], g_probe[r].prev[1],
                            g_probe[r].prev[2], g_probe[r].prev[3]);
            fprintf(g_log, "probe ----\n");
            fflush(g_log);
        }
        for (r = 0; r < 256; r++)
            g_probe[r].sum = 0, g_probe[r].frames = 0;
        last = now;
    }
}


/*
 * Debug (ProbeDraw=1): per rendered frame, every vertex / pixel shader constant write is recorded in call order together
 * with the number of draws done so far and the shaders in use; three seconds later (starting 8 s after the first frame,
 * three reports 20 s apart) the log lists, per write, which components change from one frame to the next and by how much,
 * with a stable hash of the vertex and pixel shader bytecode and the primitive count of the draw the write belongs to.
 * Run it standing next to the object in question at two frame rates and compare the two reports.
 */
int g_probe_draw;
int g_probe_draw_x, g_probe_draw_z, g_probe_draw_r;   /* centimetres; r > 0 keeps only draws whose world matrix is within r of (x, z) */

#define PD_CALLS  4096
#define PD_DRAWS  2048
#define VT_SET_VERTEX_SHADER 92
#define VT_SET_PIXEL_SHADER  107
#define VT_SET_PS_CONST_F    109
#define VT_DRAW_PRIMITIVE    81
#define VT_DRAW_INDEXED      82

typedef struct { unsigned short type, start, count, draw; float v[16]; } PdCall;
typedef struct { unsigned vs, ps, prims; } PdDraw;

typedef HRESULT (STDMETHODCALLTYPE *SetShaderFn)(IDirect3DDevice9 *, void *);
typedef HRESULT (STDMETHODCALLTYPE *DrawPrimFn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *DrawIdxFn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);

static SetShaderFn g_orig_set_vs, g_orig_set_ps;
static SetConstFFn g_orig_set_ps_const, g_orig_set_vs_const2;
static DrawPrimFn g_orig_draw_prim;
static DrawIdxFn g_orig_draw_idx;

static PdCall g_pd_cur[PD_CALLS], g_pd_prev[PD_CALLS];
static PdDraw g_pd_dcur[PD_DRAWS], g_pd_dprev[PD_DRAWS];
static int g_pd_n, g_pd_pn, g_pd_nd, g_pd_pnd;
static unsigned g_pd_vs_hash, g_pd_ps_hash;
static struct { void *ptr; unsigned hash; } g_pd_hash[512];
static int g_pd_hash_n;

/* FNV-1a of the shader bytecode, cached per shader pointer; shader objects: IUnknown(3), GetDevice, GetFunction = slot 4. */
static unsigned PdShaderHash(void *sh)
{
    unsigned h = 2166136261u;
    UINT size = 0, i;
    unsigned char *buf;
    void **vt;
    int k;

    if (!sh)
        return 0;
    for (k = 0; k < g_pd_hash_n; k++)
        if (g_pd_hash[k].ptr == sh)
            return g_pd_hash[k].hash;
    vt = *(void ***)sh;
    ((HRESULT (STDMETHODCALLTYPE *)(void *, void *, UINT *))vt[4])(sh, NULL, &size);
    if (size && size < (1u << 20) && (buf = (unsigned char *)malloc(size))) {
        ((HRESULT (STDMETHODCALLTYPE *)(void *, void *, UINT *))vt[4])(sh, buf, &size);
        for (i = 0; i < size; i++)
            h = (h ^ buf[i]) * 16777619u;
        free(buf);
    }
    if (g_pd_hash_n < 512) {
        g_pd_hash[g_pd_hash_n].ptr = sh;
        g_pd_hash[g_pd_hash_n++].hash = h;
    }
    return h;
}

static void PdConst(int type, UINT start, const float *data, UINT count)
{
    PdCall *c;

    if (!data || g_pd_n >= PD_CALLS)
        return;
    c = &g_pd_cur[g_pd_n++];
    c->type = (unsigned short)type;
    c->start = (unsigned short)start;
    c->count = (unsigned short)count;
    c->draw = (unsigned short)g_pd_nd;
    memset(c->v, 0, sizeof c->v);
    memcpy(c->v, data, 16 * (count > 4 ? 4 : count));
}

static HRESULT STDMETHODCALLTYPE HookPdSetVSConst(IDirect3DDevice9 *d, UINT s, const float *p, UINT n) { PdConst(0, s, p, n); return g_orig_set_vs_const2(d, s, p, n); }
static HRESULT STDMETHODCALLTYPE HookPdSetPSConst(IDirect3DDevice9 *d, UINT s, const float *p, UINT n) { PdConst(1, s, p, n); return g_orig_set_ps_const(d, s, p, n); }
static HRESULT STDMETHODCALLTYPE HookPdSetVS(IDirect3DDevice9 *d, void *sh) { g_pd_vs_hash = PdShaderHash(sh); return g_orig_set_vs(d, sh); }
static HRESULT STDMETHODCALLTYPE HookPdSetPS(IDirect3DDevice9 *d, void *sh) { g_pd_ps_hash = PdShaderHash(sh); return g_orig_set_ps(d, sh); }

static void PdDrawNote(UINT prims)
{
    if (g_pd_nd < PD_DRAWS) {
        g_pd_dcur[g_pd_nd].vs = g_pd_vs_hash;
        g_pd_dcur[g_pd_nd].ps = g_pd_ps_hash;
        g_pd_dcur[g_pd_nd].prims = prims;
    }
    g_pd_nd++;
}

static HRESULT STDMETHODCALLTYPE HookPdDrawPrim(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT s, UINT n) { PdDrawNote(n); return g_orig_draw_prim(d, t, s, n); }
static HRESULT STDMETHODCALLTYPE HookPdDrawIdx(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT si, UINT n) { PdDrawNote(n); return g_orig_draw_idx(d, t, b, mi, nv, si, n); }

static struct { double sum; float lo, hi; } g_pd_stat[PD_CALLS][16];
static int g_pd_pairs, g_pd_frames_seen;

typedef struct { double mean; int call, comp; } PdRow;

static int PdCmp(const void *a, const void *b)
{
    double d = ((const PdRow *)b)->mean - ((const PdRow *)a)->mean;

    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

static void PdReport(double span)
{
    static PdRow rows[PD_CALLS * 16];
    static unsigned char near_draw[PD_DRAWS];
    int n = 0, i, k, shown = 0;

    memset(near_draw, 0, sizeof near_draw);
    {
        int shown_m = 0;

        for (i = 0; i < g_pd_pn && shown_m < 4; i++)
            if (g_pd_prev[i].type == 0 && g_pd_prev[i].count >= 3 && g_pd_prev[i].start == 16) {
                fprintf(g_log, "pd sample w%d c%d x%d:", i, g_pd_prev[i].start, g_pd_prev[i].count);
                for (k = 0; k < 16; k++)
                    fprintf(g_log, " %.3f", g_pd_prev[i].v[k]);
                fprintf(g_log, "\n");
                shown_m++;
            }
    }
    for (i = 0; i < g_pd_pn && i < PD_CALLS; i++) {
        PdCall *c = &g_pd_prev[i];

        /* the translation is in v[3], v[7], v[11] (column form) or v[12], v[13], v[14] (the transposed form D3D receives) */
        if (g_probe_draw_r > 0 && c->type == 0 && c->count >= 3 && c->draw < PD_DRAWS &&
            ((fabs(c->v[3] * 100.0 - g_probe_draw_x) < g_probe_draw_r && fabs(c->v[11] * 100.0 - g_probe_draw_z) < g_probe_draw_r) ||
             (fabs(c->v[12] * 100.0 - g_probe_draw_x) < g_probe_draw_r && fabs(c->v[14] * 100.0 - g_probe_draw_z) < g_probe_draw_r)))
            near_draw[c->draw] = 1;
    }
    for (i = 0; i < g_pd_pn && i < PD_CALLS; i++)
        for (k = 0; k < 16; k++)
            if (g_pd_stat[i][k].sum > 0.0 && (g_probe_draw_r <= 0 || (g_pd_prev[i].draw < PD_DRAWS && near_draw[g_pd_prev[i].draw]))) {
                rows[n].mean = g_pd_stat[i][k].sum / g_pd_pairs;
                rows[n].call = i;
                rows[n++].comp = k;
            }
    qsort(rows, n, sizeof rows[0], PdCmp);
    {
        int nd = 0;

        for (i = 0; i < PD_DRAWS; i++)
            nd += near_draw[i];
        fprintf(g_log, "pd report: %d frames in %.2f s (%.1f fps), %d frame pairs compared, %d writes and %d draws per frame, %d changing components, %d draws near the point\n",
                g_pd_frames_seen, span, g_pd_frames_seen / span, g_pd_pairs, g_pd_pn, g_pd_pnd, n, nd);
    }
    for (i = 0; i < n && shown < 600; i++) {
        PdCall *c = &g_pd_prev[rows[i].call];
        PdDraw *d = c->draw < PD_DRAWS ? &g_pd_dprev[c->draw] : NULL;

        fprintf(g_log, "pd w%d %s c%d x%d comp %d mean %.6f/frame %.6f/s range %.4f..%.4f | draw %d vs %08x ps %08x prims %u\n",
                rows[i].call, c->type ? "PS" : "VS", c->start, c->count, rows[i].comp, rows[i].mean,
                rows[i].mean * g_pd_frames_seen / span, g_pd_stat[rows[i].call][rows[i].comp].lo,
                g_pd_stat[rows[i].call][rows[i].comp].hi, c->draw, d ? d->vs : 0, d ? d->ps : 0, d ? d->prims : 0);
        shown++;
    }
    fprintf(g_log, "pd ----\n");
    fflush(g_log);
}

static void PdFrame(void)
{
    static LARGE_INTEGER freq, start, t0;
    static int phase, reports;
    LARGE_INTEGER now;
    int i, k;

    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        start = now;
    }
    if (g_pd_n == g_pd_pn && g_pd_n > 0 && phase == 1) {
        for (i = 0; i < g_pd_n; i++) {
            if (g_pd_cur[i].type != g_pd_prev[i].type || g_pd_cur[i].start != g_pd_prev[i].start ||
                g_pd_cur[i].count != g_pd_prev[i].count)
                continue;
            for (k = 0; k < 16; k++) {
                double dv = fabs((double)g_pd_cur[i].v[k] - (double)g_pd_prev[i].v[k]);

                if (g_pd_pairs == 0 || g_pd_cur[i].v[k] < g_pd_stat[i][k].lo) g_pd_stat[i][k].lo = g_pd_cur[i].v[k];
                if (g_pd_pairs == 0 || g_pd_cur[i].v[k] > g_pd_stat[i][k].hi) g_pd_stat[i][k].hi = g_pd_cur[i].v[k];
                g_pd_stat[i][k].sum += dv;
            }
        }
        g_pd_pairs++;
    }
    if (phase == 1)
        g_pd_frames_seen++;
    memcpy(g_pd_prev, g_pd_cur, sizeof(PdCall) * (g_pd_n < PD_CALLS ? g_pd_n : PD_CALLS));
    memcpy(g_pd_dprev, g_pd_dcur, sizeof(PdDraw) * (g_pd_nd < PD_DRAWS ? g_pd_nd : PD_DRAWS));
    g_pd_pn = g_pd_n;
    g_pd_pnd = g_pd_nd;
    g_pd_n = g_pd_nd = 0;

    {
        double since = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;

        if (phase == 0 && since >= 8.0 + reports * 20.0 && reports < 3 && g_log) {
            memset(g_pd_stat, 0, sizeof g_pd_stat);
            g_pd_pairs = g_pd_frames_seen = 0;
            t0 = now;
            phase = 1;
        } else if (phase == 1 && (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart >= 3.0) {
            PdReport((double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart);
            phase = 0;
            reports++;
        }
    }
}

/*
 * Title screen. While the game shows "press any button" (and the first moments after it) the scene is drawn with the title
 * camera of the level data, a projection with a 4:3 aspect (y scale / x scale = 1.333; the menus and levels have 1.778), so on a
 * wide viewport the logo and characters are stretched sideways. The camera is set up by sub_5FA330(view, projection) from
 * three call sites; while the titles level is current, a projection narrower than the game's menu aspect gets its x scale set to
 * that aspect (a copy is passed), and the title camera's field of view is replaced by the menu camera's, which is the same view
 * zoomed 1.156 times: without it the logo pops bigger when the menu opens. (The cut between the two cameras has no blend, the
 * original did not blend either.) The aspect fix applies to every projection of the titles level that is narrower than the
 * menu aspect, so it also holds if the cameras ever blend.
 */
int g_fix_title_aspect = 1;

/* y scale of the title camera (43 degrees) and of the menu camera of the same level (37.4 degrees): same position, 1.156 times the zoom. */
#define TITLE_FOV_Y 2.5415f
#define MENU_FOV_Y  2.9379f
#define ADDR_CAMERA_SET_FN 0x005FA330u
static const uintptr_t k_camera_calls[] = { 0x005F6B77u, 0x005F6ED4u, 0x00603B44u };
#define CAMERA_CALLS ((int)(sizeof k_camera_calls / sizeof k_camera_calls[0]))
#define ADDR_TITLES_LEVEL 0x00951B88u
#define ADDR_GAME_PTR     0x00802C54u

/* The aspect the game uses for its menus and levels (sub_6DF290): the "aspect" setting 0 4:3, 1 16:9, 2 16:10, 3 the display's. */
static double MenuAspect(void)
{
    switch (*(const int *)0x00826494u) {
    case 1:  return 16.0 / 9.0;
    case 2:  return 1.6;
    case 3:  return *(const float *)0x02976BF8u;
    default: return 4.0 / 3.0;
    }
}

/*
 * The menu camera's y scale, read from the game: the main camera's projection (sub_5ED540 builds it from the camera
 * struct that CameraAspect fills, and the menu camera is that one) is at flt_80B748 (x scale at [0], y scale at [5]). Until the
 * game has built it once (or if it looks wrong) the scale is computed from the camera's field of view.
 */
#define ADDR_MAIN_PROJECTION 0x0080B748u
#define ADDR_MAIN_CAMERA_PTR 0x00925164u

static float MenuZoom(double aspect)
{
    const float *p = (const float *)ADDR_MAIN_PROJECTION;
    const float *cam = (const float *)*(const uintptr_t *)ADDR_MAIN_CAMERA_PTR;

    (void)aspect;
    if (p[5] > 1.5f && p[5] < 8.0f)
        return p[5];
    if (cam && cam[16] > 0.1f && cam[16] < 2.0f)
        return (float)(1.0 / tan(cam[16] * 0.5));
    return MENU_FOV_Y;
}

static int __cdecl CameraSet(const void *view, const float *proj)
{
    float m[16];
    double aspect;
    int game = *(const int *)ADDR_GAME_PTR;

    if (g_fix_title_aspect && proj && game && *(const int *)(game + 296) == *(const int *)ADDR_TITLES_LEVEL &&
        proj[0] > 0.1f && proj[5] > 0.1f && (aspect = MenuAspect()) > 1.45 && proj[5] / proj[0] < aspect - 0.01) {
        memcpy(m, proj, sizeof m);
        if (m[5] > TITLE_FOV_Y - 0.05f && m[5] < TITLE_FOV_Y + 0.05f)
            m[5] = MenuZoom(aspect);
        m[0] = (float)(m[5] / aspect);
        proj = m;
    }
    return ((int (__cdecl *)(const void *, const float *))ADDR_CAMERA_SET_FN)(view, proj);
}

static int CameraCallsOk(void)
{
    int i;

    for (i = 0; i < CAMERA_CALLS; i++) {
        const uint8_t *b = (const uint8_t *)k_camera_calls[i];

        if (b[0] != 0xE8 || (uint32_t)(k_camera_calls[i] + 5 + *(const int32_t *)(b + 1)) != ADDR_CAMERA_SET_FN)
            return 0;
    }
    return 1;
}

static HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty)
{
    if (g_probe_consts)
        ProbeFrame();
    if (g_probe_draw)
        PdFrame();
    if (g_log_fps) {
        LARGE_INTEGER a, b;
        HRESULT hr;

        QueryPerformanceCounter(&a);
        hr = g_orig_present(dev, src, dst, wnd, dirty);
        QueryPerformanceCounter(&b);
        g_present_ticks += b.QuadPart - a.QuadPart;
        return hr;
    }
    return g_orig_present(dev, src, dst, wnd, dirty);
}

static HRESULT STDMETHODCALLTYPE HookSetVSConst(IDirect3DDevice9 *dev, UINT start, const float *data, UINT count)
{
    if (g_probe_consts && data)
        ProbeSet(start, data, count);
    return g_orig_set_vs_const(dev, start, data, count);
}

static HRESULT STDMETHODCALLTYPE HookCreateTexture(IDirect3DDevice9 *dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *shared)
{
    HRESULT hr = g_orig_create_texture(dev, w, h, levels, usage, fmt, pool, out, shared);

    if (SUCCEEDED(hr) && out && *out && w == 256 && h == 128 && fmt == D3DFMT_DXT5 && !g_buttons_tex)
        g_buttons_tex = *out;
    if (g_dump_textures && SUCCEEDED(hr) && out && *out && g_tex_count < MAX_DUMP_TEX && !(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))
        g_tex_list[g_tex_count++] = *out;
    return hr;
}

/* Called every frame; hooks the device once it exists. */
static void InstallDeviceHooks(void)
{
    IDirect3DDevice9 *dev;
    void **vtable;

    dev = *(IDirect3DDevice9 **)(ADDR_DEVICE_OBJECT + 4);
    if (!dev)
        return;
    vtable = *(void ***)dev;
    if (vtable == g_hooked_vtable)
        return;
    PatchVtableEntry(vtable, VT_CREATE_TEXTURE, (void *)HookCreateTexture, (void **)&g_orig_create_texture);
    PatchVtableEntry(vtable, VT_SET_TEXTURE, (void *)HookSetTexture, (void **)&g_orig_set_texture);
    PatchVtableEntry(vtable, VT_PRESENT, (void *)HookPresent, (void **)&g_orig_present);
    if (g_probe_consts)
        PatchVtableEntry(vtable, VT_SET_VS_CONST_F, (void *)HookSetVSConst, (void **)&g_orig_set_vs_const);
    if (g_probe_draw) {
        PatchVtableEntry(vtable, VT_SET_VS_CONST_F, (void *)HookPdSetVSConst, (void **)&g_orig_set_vs_const2);
        PatchVtableEntry(vtable, VT_SET_PS_CONST_F, (void *)HookPdSetPSConst, (void **)&g_orig_set_ps_const);
        PatchVtableEntry(vtable, VT_SET_VERTEX_SHADER, (void *)HookPdSetVS, (void **)&g_orig_set_vs);
        PatchVtableEntry(vtable, VT_SET_PIXEL_SHADER, (void *)HookPdSetPS, (void **)&g_orig_set_ps);
        PatchVtableEntry(vtable, VT_DRAW_PRIMITIVE, (void *)HookPdDrawPrim, (void **)&g_orig_draw_prim);
        PatchVtableEntry(vtable, VT_DRAW_INDEXED, (void *)HookPdDrawIdx, (void **)&g_orig_draw_idx);
        Log("draw probe installed");
    }
    g_hooked_vtable = vtable;
    Log("device hooks installed");
}

static void SyncModeTable(void)
{
    static int last_hz;
    int count = *(int *)ADDR_MODE_COUNT;
    int *table = *(int **)ADDR_MODE_TABLE;
    int hz = g_refresh_override ? g_refresh_override : *(int *)ADDR_DESKTOP_HZ;
    int i;

    if (!table || count <= 0 || count > 256 || hz < 24 || hz > 1000 || hz == last_hz)
        return;
    for (i = 0; i < count; i++)
        table[i * 4 + 2] = hz;
    last_hz = hz;
    Log("video mode refresh rates updated");
}

int RenderReady(void)
{
    return CameraCallsOk();
}

static DWORD WINAPI EarlyHookThread(LPVOID param)
{
    (void)param;
    while (!g_hooked_vtable) {
        InstallDeviceHooks();
        Sleep(1);
    }
    return 0;
}

void RenderInstall(void)
{
    int i;

    for (i = 0; i < CAMERA_CALLS; i++)
        PatchCall(k_camera_calls[i], CameraSet);
    /* the D3D9 device appears early; hook it from its own thread as soon as it exists */
    CreateThread(NULL, 0, EarlyHookThread, NULL, 0, NULL);
}

void RenderFrame(int cur, int titles)
{
    SyncModeTable();
    InstallDeviceHooks();
    if (g_dump_textures) {
        static int frames;

        if (cur && cur != titles && ++frames == 600)
            DumpTextures();
    }
}
