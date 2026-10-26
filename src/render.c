/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Render scale: scaled effect render targets, depth twin, virtual backbuffer for the HUD pass,
 * shader screen constant, optional oversized video modes, and the D3D9 vtable hooks (plus pass tracing).
 */
#include "all.h"

static const uint8_t k_modelookup_orig[5]  = { 0x83, 0xEC, 0x18, 0x53, 0x55 };

static const uint8_t k_rt_h_orig[5]       = { 0xA1, 0x04, 0x67, 0x97, 0x02 };

static const uint8_t k_rt_w_orig[5]       = { 0xA1, 0x00, 0x67, 0x97, 0x02 };

double g_scales[MAX_RENDER_SCALES];

int g_scale_count;

void *g_lookup_tramp;

double g_render_scale = 1.0;

/* Scaled render target sizes (even, at least 16). */
int __cdecl ScaledWidth(void)
{
    int v;

    if (g_render_scale == 1.0)
        return *(int *)ADDR_BACKBUFFER_W;
    v = (int)(*(int *)ADDR_BACKBUFFER_W * g_render_scale + 0.5);

    return v < 16 ? 16 : (v & ~1);
}

int __cdecl ScaledHeight(void)
{
    int v;

    if (g_render_scale == 1.0)
        return *(int *)ADDR_BACKBUFFER_H;
    v = (int)(*(int *)ADDR_BACKBUFFER_H * g_render_scale + 0.5);

    return v < 16 ? 16 : (v & ~1);
}

/* register-preserving entry points so they can replace a "mov eax,[abs]" (5 bytes) */
extern void ScaledWidthStub(void);

extern void ScaledHeightStub(void);

__asm__(
    ".text\n"
    ".globl _ScaledWidthStub\n"
    "_ScaledWidthStub:\n"
    "    pushl %ecx\n    pushl %edx\n"
    "    call _ScaledWidth\n"
    "    popl %edx\n    popl %ecx\n    ret\n"
    ".globl _ScaledHeightStub\n"
    "_ScaledHeightStub:\n"
    "    pushl %ecx\n    pushl %edx\n"
    "    call _ScaledHeight\n"
    "    popl %edx\n    popl %ecx\n    ret\n"
);

typedef HRESULT (STDMETHODCALLTYPE *SetRenderTargetFn)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);

typedef HRESULT (STDMETHODCALLTYPE *SetViewportFn)(IDirect3DDevice9 *, const D3DVIEWPORT9 *);

typedef HRESULT (STDMETHODCALLTYPE *SetScissorRectFn)(IDirect3DDevice9 *, const RECT *);

typedef HRESULT (STDMETHODCALLTYPE *SetDepthStencilFn)(IDirect3DDevice9 *, IDirect3DSurface9 *);

static SetRenderTargetFn g_orig_set_rt;

static SetViewportFn g_orig_set_viewport;

static SetScissorRectFn g_orig_set_scissor;

static SetDepthStencilFn g_orig_set_depth;

typedef HRESULT (STDMETHODCALLTYPE *GetRenderTargetFn)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 **);

typedef HRESULT (STDMETHODCALLTYPE *GetDepthStencilFn)(IDirect3DDevice9 *, IDirect3DSurface9 **);

static GetRenderTargetFn g_orig_get_rt;

static GetDepthStencilFn g_orig_get_depth;

typedef HRESULT (STDMETHODCALLTYPE *SetConstFFn)(IDirect3DDevice9 *, UINT, const float *, UINT);

static SetConstFFn g_orig_set_vs_const, g_orig_set_ps_const;

static IDirect3DSurface9 *g_depth_main; /* the game's full-size depth buffer */
static IDirect3DSurface9 *g_depth_twin; /* same format, size of the scaled targets */
static IDirect3DSurface9 *g_engine_depth; /* what the game last asked for */
static UINT g_twin_w, g_twin_h;
static unsigned g_diag_scaled_binds, g_diag_twin_used, g_diag_twin_fail, g_diag_depth_sets;

/* Depth buffer that matches the scaled targets (a smaller one would clip or disable depth). */
static int EnsureDepthTwin(IDirect3DDevice9 *dev)
{
    D3DSURFACE_DESC desc;
    UINT w = (UINT)ScaledWidth(), h = (UINT)ScaledHeight();

    if (!g_depth_main)
        return 0;
    if (g_depth_twin && g_twin_w == w && g_twin_h == h)
        return 1;
    if (FAILED(IDirect3DSurface9_GetDesc(g_depth_main, &desc)))
        return 0;
    if (g_depth_twin) {
        IDirect3DSurface9_Release(g_depth_twin);
        g_depth_twin = NULL;
    }
    if (FAILED(IDirect3DDevice9_CreateDepthStencilSurface(dev, w, h, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_depth_twin, NULL)))
        return 0;
    g_twin_w = w;
    g_twin_h = h;
    Log("render scale: depth buffer created");
    return 1;
}

static void **g_hooked_vtable;

static int g_rt0_scaled;

int g_trace_rt; /* debug: log device calls for a few frames */

int g_probe_consts;   /* debug: log shader constants that change every frame */

void ProbeFrame(void);
static int g_trace_budget;

static void Trace(const char *fmt, ...)
{
    va_list ap;

    if (!g_log || g_trace_budget <= 0)
        return;
    g_trace_budget--;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* ---- debug: per-pass draw statistics / pass skipping (TraceRT=1, SkipPass=N) ---------- */
#define VT_END_SCENE 42

#define VT_DRAW_PRIMITIVE 81

#define VT_DRAW_INDEXED 82

#define VT_DRAW_PRIMITIVE_UP 83

#define VT_DRAW_INDEXED_UP 84

typedef HRESULT (STDMETHODCALLTYPE *EndSceneFn)(IDirect3DDevice9 *);

typedef HRESULT (STDMETHODCALLTYPE *DrawPrimFn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);

typedef HRESULT (STDMETHODCALLTYPE *DrawIdxFn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);

typedef HRESULT (STDMETHODCALLTYPE *DrawPrimUPFn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);

typedef HRESULT (STDMETHODCALLTYPE *DrawIdxUPFn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT);

static EndSceneFn g_orig_end_scene;

static DrawPrimFn g_orig_draw_prim;

static DrawIdxFn g_orig_draw_idx;

static DrawPrimUPFn g_orig_draw_prim_up;

static DrawIdxUPFn g_orig_draw_idx_up;

int g_pass_index, g_pass_draws, g_skip_pass = -1;

static unsigned g_pass_w, g_pass_h;

int g_skip_scaled = -1, g_scaled_done;

int g_trace_passes, g_pass_lines;

static char g_frame_sig[1024], g_last_sig[1024];

static void EndPass(void)
{
    if (g_pass_draws && g_pass_w == (unsigned)ScaledWidth() && g_pass_h == (unsigned)ScaledHeight())
        g_scaled_done++;
    if (g_trace_passes && g_pass_draws) {
        size_t n = strlen(g_frame_sig);

        if (n < sizeof g_frame_sig - 64)
            snprintf(g_frame_sig + n, sizeof g_frame_sig - n, "[%d: %ux%u] ", g_pass_index, g_pass_w, g_pass_h);
    }
    g_pass_draws = 0;
}

static void EndFrameSig(void)
{
    if (g_trace_passes && g_log && g_pass_lines < 400 && g_frame_sig[0] && strcmp(g_frame_sig, g_last_sig)) {
        fprintf(g_log, "frame passes: %s\n", g_frame_sig);
        fflush(g_log);
        g_pass_lines++;
        memcpy(g_last_sig, g_frame_sig, sizeof g_last_sig);
    }
    g_frame_sig[0] = 0;
}

static int SkipDraw(void)
{
    g_pass_draws++;
    if (g_skip_scaled >= 0 && g_pass_w == (unsigned)ScaledWidth() && g_pass_h == (unsigned)ScaledHeight() &&
        g_scaled_done == g_skip_scaled)
        return 1;
    return g_skip_pass >= 0 && g_pass_index == g_skip_pass;
}

static HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9 *dev)
{
    EndPass();
    EndFrameSig();
    g_pass_index = 0;
    g_scaled_done = 0;
    return g_orig_end_scene(dev);
}

static HRESULT STDMETHODCALLTYPE HookDrawPrim(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT b)
{
    return SkipDraw() ? D3D_OK : g_orig_draw_prim(d, t, a, b);
}

static HRESULT STDMETHODCALLTYPE HookDrawIdx(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT a, UINT b, UINT c, UINT e, UINT f)
{
    return SkipDraw() ? D3D_OK : g_orig_draw_idx(d, t, a, b, c, e, f);
}

static HRESULT STDMETHODCALLTYPE HookDrawPrimUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, const void *b, UINT c)
{
    return SkipDraw() ? D3D_OK : g_orig_draw_prim_up(d, t, a, b, c);
}

static HRESULT STDMETHODCALLTYPE HookDrawIdxUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT c, const void *e, D3DFORMAT f, const void *g, UINT h)
{
    return SkipDraw() ? D3D_OK : g_orig_draw_idx_up(d, t, a, b, c, e, f, g, h);
}

/*
 * Everything the game draws straight onto the backbuffer after the scene (HUD, glow and
 * sparkle particles that are depth tested against the scene) has to use the same resolution
 * and the same scaled depth buffer as the scene, otherwise the particles are tested against an
 * empty depth buffer and show through walls. While the scale is not 100% the backbuffer is
 * replaced by a render target of the scaled size and copied (filtered) onto the real
 * backbuffer just before Present.
 */
#define VT_PRESENT 17

#define VT_RESET   16

typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);

typedef HRESULT (STDMETHODCALLTYPE *ResetFn)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *); /* a cutscene skip is running: the screen is kept black */
static PresentFn g_orig_present;

LONGLONG g_present_ticks;   /* time spent inside Present, for LogFPS */

static ResetFn g_orig_reset;

static IDirect3DSurface9 *g_virtual_bb;

static void *g_real_bb; /* identity of the swap chain's backbuffer, not a reference */
static UINT g_vbb_w, g_vbb_h;

static int g_vbb_used;

static IDirect3DSurface9 *VirtualBackbuffer(IDirect3DDevice9 *dev, IDirect3DSurface9 *surface)
{
    D3DSURFACE_DESC desc;
    UINT w, h;

    if (g_render_scale == 1.0)
        return NULL;
    if (!g_real_bb) {
        IDirect3DSurface9 *bb = NULL;

        if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
            return NULL;
        g_real_bb = bb;
        IDirect3DSurface9_Release(bb);
    }
    if ((void *)surface != g_real_bb || FAILED(IDirect3DSurface9_GetDesc(surface, &desc)) ||
        desc.MultiSampleType != D3DMULTISAMPLE_NONE)
        return NULL;
    w = (UINT)ScaledWidth();
    h = (UINT)ScaledHeight();
    if (g_virtual_bb && (g_vbb_w != w || g_vbb_h != h)) {
        IDirect3DSurface9_Release(g_virtual_bb);
        g_virtual_bb = NULL;
    }
    if (!g_virtual_bb) {
        if (FAILED(IDirect3DDevice9_CreateRenderTarget(dev, w, h, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_virtual_bb, NULL)))
            return NULL;
        g_vbb_w = w;
        g_vbb_h = h;
        Log("render scale: scaled backbuffer created");
    }
    g_vbb_used = 1;
    return g_virtual_bb;
}

static HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty)
{
    if (g_vbb_used && g_virtual_bb && g_real_bb && g_render_scale != 1.0)
        IDirect3DDevice9_StretchRect(dev, g_virtual_bb, NULL, (IDirect3DSurface9 *)g_real_bb, NULL, D3DTEXF_LINEAR);
    g_vbb_used = 0;
    if (g_trace_skip && g_skip_first_tick && GetTickCount() - g_skip_first_tick < 4000) {
        char buf[160];

        snprintf(buf, sizeof buf, "present +%lu ms black %d wipe %.3f dir %.3f style %02x pend %d/%d be0 %d",
                 GetTickCount() - g_skip_first_tick, g_cutscene_skipping, *(float *)0x950780, *(float *)0x950784,
                 *(uint8_t *)0x95077C, *(int *)0x87658C, *(int *)0x951B80 != 0, *(int *)0x951BE0);
        Log(buf);
    }
    if (g_cutscene_skipping) {
        IDirect3DSurface9 *bb = NULL;

        if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            HRESULT hr = IDirect3DDevice9_ColorFill(dev, bb, NULL, 0xFF000000);
            static int logged;

            if (FAILED(hr) && !logged++) {
                char buf[64];

                snprintf(buf, sizeof buf, "skip black fill failed: %08lx", (unsigned long)hr);
                Log(buf);
            }
            IDirect3DSurface9_Release(bb);
        }
    }
    if (g_probe_consts)
        ProbeFrame();
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

static HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    if (g_virtual_bb) {
        IDirect3DSurface9_Release(g_virtual_bb);
        g_virtual_bb = NULL;
    }
    if (g_depth_twin) {
        IDirect3DSurface9_Release(g_depth_twin);
        g_depth_twin = NULL;
    }
    g_real_bb = NULL;
    g_depth_main = NULL;      /* the game recreates its surfaces; learn them again from the next calls */
    g_engine_depth = NULL;
    g_vbb_used = 0;
    return g_orig_reset(dev, pp);
}

static void ResendScreenConst(IDirect3DDevice9 *dev);

static HRESULT STDMETHODCALLTYPE HookSetRenderTarget(IDirect3DDevice9 *dev, DWORD index, IDirect3DSurface9 *surface)
{
    HRESULT hr;

    if (index == 0 && surface) {
        IDirect3DSurface9 *vbb = VirtualBackbuffer(dev, surface);

        if (vbb)
            surface = vbb;
    }
    hr = g_orig_set_rt(dev, index, surface);

    if (g_trace_rt && surface) {
        D3DSURFACE_DESC d;

        if (SUCCEEDED(IDirect3DSurface9_GetDesc(surface, &d)))
            Trace("SetRenderTarget idx %lu surf %p %ux%u fmt %d ms %d", (unsigned long)index, (void *)surface, d.Width, d.Height, (int)d.Format, (int)d.MultiSampleType);
    }
    if (index == 0 && SUCCEEDED(hr)) {
        int was_scaled = g_rt0_scaled;

        if (surface) {
            D3DSURFACE_DESC pd;

            EndPass();
            g_pass_index++;
            if (SUCCEEDED(IDirect3DSurface9_GetDesc(surface, &pd))) {
                g_pass_w = pd.Width;
                g_pass_h = pd.Height;
            }
        }

        g_rt0_scaled = 0;
        if (surface) {
            D3DSURFACE_DESC desc;

            if (SUCCEEDED(IDirect3DSurface9_GetDesc(surface, &desc)))
                g_rt0_scaled = desc.Width == (UINT)ScaledWidth() && desc.Height == (UINT)ScaledHeight() &&
                               (desc.Width != (UINT)*(int *)ADDR_BACKBUFFER_W ||
                                desc.Height != (UINT)*(int *)ADDR_BACKBUFFER_H);
        }
        if (g_rt0_scaled)
            g_diag_scaled_binds++;
        if (g_engine_depth && g_engine_depth == g_depth_main) {
            int twin = g_rt0_scaled && EnsureDepthTwin(dev);

            if (g_rt0_scaled) {
                if (twin)
                    g_diag_twin_used++;
                else
                    g_diag_twin_fail++;
            }
            g_orig_set_depth(dev, twin ? g_depth_twin : g_depth_main);
        }
        if (was_scaled != g_rt0_scaled)
            ResendScreenConst(dev);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookSetDepthStencil(IDirect3DDevice9 *dev, IDirect3DSurface9 *surface)
{
    D3DSURFACE_DESC desc;

    if (g_trace_rt && surface && SUCCEEDED(IDirect3DSurface9_GetDesc(surface, &desc)))
        Trace("SetDepthStencil surf %p %ux%u fmt %d scaledRT=%d main=%p", (void *)surface, desc.Width, desc.Height, (int)desc.Format, g_rt0_scaled, (void *)g_depth_main);
    if (surface && surface != g_depth_twin && SUCCEEDED(IDirect3DSurface9_GetDesc(surface, &desc)) &&
        desc.Width == (UINT)*(int *)ADDR_BACKBUFFER_W && desc.Height == (UINT)*(int *)ADDR_BACKBUFFER_H)
        g_depth_main = surface;                    /* the game's own full-size depth buffer */
    g_engine_depth = surface;
    g_diag_depth_sets++;
    if (surface && surface == g_depth_main && g_rt0_scaled && EnsureDepthTwin(dev))
        surface = g_depth_twin;
    return g_orig_set_depth(dev, surface);
}

/*
 * The game saves the bound surfaces with Get* and restores them later with Set*. It must never be
 * handed the surfaces we substituted (scaled depth twin, virtual backbuffer): they are replaced
 * when the scale changes, and the game would keep restoring a stale, smaller one - the depth test
 * then clips the scene and every character disappears.
 */
static HRESULT STDMETHODCALLTYPE HookGetDepthStencil(IDirect3DDevice9 *dev, IDirect3DSurface9 **out)
{
    HRESULT hr = g_orig_get_depth(dev, out);

    if (SUCCEEDED(hr) && out && *out && *out == g_depth_twin && g_depth_main) {
        IDirect3DSurface9_Release(*out);
        *out = g_depth_main;
        IDirect3DSurface9_AddRef(g_depth_main);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookGetRenderTarget(IDirect3DDevice9 *dev, DWORD index, IDirect3DSurface9 **out)
{
    HRESULT hr = g_orig_get_rt(dev, index, out);

    if (SUCCEEDED(hr) && index == 0 && out && *out && *out == g_virtual_bb && g_real_bb) {
        IDirect3DSurface9_Release(*out);
        *out = (IDirect3DSurface9 *)g_real_bb;
        IDirect3DSurface9_AddRef(*out);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookSetViewport(IDirect3DDevice9 *dev, const D3DVIEWPORT9 *vp)
{
    D3DVIEWPORT9 scaled;

    if (g_trace_rt && vp)
        Trace("SetViewport %lu,%lu %lux%lu scaledRT=%d", (unsigned long)vp->X, (unsigned long)vp->Y, (unsigned long)vp->Width, (unsigned long)vp->Height, g_rt0_scaled);
    if (vp && g_rt0_scaled) {
        scaled = *vp;
        scaled.X = (DWORD)(vp->X * g_render_scale + 0.5);
        scaled.Y = (DWORD)(vp->Y * g_render_scale + 0.5);
        scaled.Width = (DWORD)(vp->Width * g_render_scale + 0.5);
        scaled.Height = (DWORD)(vp->Height * g_render_scale + 0.5);
        return g_orig_set_viewport(dev, &scaled);
    }
    return g_orig_set_viewport(dev, vp);
}

static HRESULT STDMETHODCALLTYPE HookSetScissorRect(IDirect3DDevice9 *dev, const RECT *rect)
{
    RECT scaled;

    if (g_trace_rt && rect)
        Trace("SetScissorRect %ld,%ld %ld,%ld scaledRT=%d", rect->left, rect->top, rect->right, rect->bottom, g_rt0_scaled);
    if (rect && g_rt0_scaled) {
        scaled.left = (LONG)(rect->left * g_render_scale + 0.5);
        scaled.top = (LONG)(rect->top * g_render_scale + 0.5);
        scaled.right = (LONG)(rect->right * g_render_scale + 0.5);
        scaled.bottom = (LONG)(rect->bottom * g_render_scale + 0.5);
        return g_orig_set_scissor(dev, &scaled);
    }
    return g_orig_set_scissor(dev, rect);
}

static void TraceConstants(const char *kind, UINT start, const float *data, UINT count)
{
    UINT i, k;
    double w = *(int *)ADDR_BACKBUFFER_W, h = *(int *)ADDR_BACKBUFFER_H;

    for (i = 0; i < count; i++)
        for (k = 0; k < 4; k++) {
            double v = data[i * 4 + k];

            if (fabs(v - w) < 0.5 || fabs(v - h) < 0.5 || fabs(v - 1.0 / w) < 2e-6 || fabs(v - 1.0 / h) < 2e-6 ||
                fabs(v - 0.5 / w) < 1e-6 || fabs(v - 0.5 / h) < 1e-6 || fabs(v - 2.0 / w) < 4e-6 || fabs(v - 2.0 / h) < 4e-6)
                Trace("%s const reg %u [%u] = %.7f (%g %g %g %g)", kind, start + i, k, v, data[i * 4], data[i * 4 + 1], data[i * 4 + 2], data[i * 4 + 3]);
        }
}

/*
 * c45 holds (width, height, 1/width, 1/height) of the screen. Shaders turn pixel positions into
 * texture coordinates with it (effects compare against the scene depth that way), so while a
 * scaled target is bound it has to describe that target instead of the backbuffer.
 */
#define SCREEN_CONST_REG 45

static float g_screen_const[4];

static int g_screen_const_valid;

static void ScaledScreenConst(float *out)
{
    out[0] = (float)(g_screen_const[0] * g_render_scale);
    out[1] = (float)(g_screen_const[1] * g_render_scale);
    out[2] = (float)(g_screen_const[2] / g_render_scale);
    out[3] = (float)(g_screen_const[3] / g_render_scale);
}

static int IsScreenConst(const float *v)
{
    return fabsf(v[0] - (float)*(int *)ADDR_BACKBUFFER_W) < 0.5f && fabsf(v[1] - (float)*(int *)ADDR_BACKBUFFER_H) < 0.5f;
}

static HRESULT SetConstFiltered(IDirect3DDevice9 *dev, SetConstFFn orig, UINT start, const float *data, UINT count)
{
    float copy[256 * 4];

    if (data && start <= SCREEN_CONST_REG && start + count > SCREEN_CONST_REG && count <= 256 &&
        IsScreenConst(data + (SCREEN_CONST_REG - start) * 4)) {
        memcpy(g_screen_const, data + (SCREEN_CONST_REG - start) * 4, sizeof g_screen_const);
        g_screen_const_valid = 1;
        if (g_rt0_scaled && g_render_scale != 1.0) {
            memcpy(copy, data, (size_t)count * 16);
            ScaledScreenConst(copy + (SCREEN_CONST_REG - start) * 4);
            return orig(dev, start, copy, count);
        }
    }
    return orig(dev, start, data, count);
}

static void ResendScreenConst(IDirect3DDevice9 *dev)
{
    float v[4];

    if (!g_screen_const_valid)
        return;
    if (g_rt0_scaled && g_render_scale != 1.0)
        ScaledScreenConst(v);
    else
        memcpy(v, g_screen_const, sizeof v);
    g_orig_set_vs_const(dev, SCREEN_CONST_REG, v, 1);
    g_orig_set_ps_const(dev, SCREEN_CONST_REG, v, 1);
}

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

static HRESULT STDMETHODCALLTYPE HookSetVSConst(IDirect3DDevice9 *dev, UINT start, const float *data, UINT count)
{
    if (g_probe_consts && data)
        ProbeSet(start, data, count);
    if (g_trace_rt && data)
        TraceConstants("VS", start, data, count);
    return SetConstFiltered(dev, g_orig_set_vs_const, start, data, count);
}

static HRESULT STDMETHODCALLTYPE HookSetPSConst(IDirect3DDevice9 *dev, UINT start, const float *data, UINT count)
{
    if (g_trace_rt && data)
        TraceConstants("PS", start, data, count);
    return SetConstFiltered(dev, g_orig_set_ps_const, start, data, count);
}

/* ---- debug: texture dump (DumpTextures=1) -------------------------------------------- */
#define VT_CREATE_TEXTURE 23

typedef HRESULT (STDMETHODCALLTYPE *CreateTextureFn)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);

static CreateTextureFn g_orig_create_texture;

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
static void InstallRenderScaleHooks(void)
{
    IDirect3DDevice9 *dev;
    void **vtable;

    dev = *(IDirect3DDevice9 **)(ADDR_DEVICE_OBJECT + 4);
    if (!dev)
        return;
    vtable = *(void ***)dev;
    if (vtable == g_hooked_vtable)
        return;
    PatchVtableEntry(vtable, VT_SET_RENDER_TARGET, (void *)HookSetRenderTarget, (void **)&g_orig_set_rt);
    PatchVtableEntry(vtable, VT_SET_VIEWPORT, (void *)HookSetViewport, (void **)&g_orig_set_viewport);
    PatchVtableEntry(vtable, VT_SET_DEPTH_STENCIL, (void *)HookSetDepthStencil, (void **)&g_orig_set_depth);
    PatchVtableEntry(vtable, VT_GET_RENDER_TARGET, (void *)HookGetRenderTarget, (void **)&g_orig_get_rt);
    PatchVtableEntry(vtable, VT_GET_DEPTH_STENCIL, (void *)HookGetDepthStencil, (void **)&g_orig_get_depth);
    PatchVtableEntry(vtable, VT_SET_SCISSOR_RECT, (void *)HookSetScissorRect, (void **)&g_orig_set_scissor);
    PatchVtableEntry(vtable, VT_CREATE_TEXTURE, (void *)HookCreateTexture, (void **)&g_orig_create_texture);
    PatchVtableEntry(vtable, VT_SET_TEXTURE, (void *)HookSetTexture, (void **)&g_orig_set_texture);
    if (g_trace_passes || g_skip_pass >= 0 || g_skip_scaled >= 0) {
        PatchVtableEntry(vtable, VT_END_SCENE, (void *)HookEndScene, (void **)&g_orig_end_scene);
        PatchVtableEntry(vtable, VT_DRAW_PRIMITIVE, (void *)HookDrawPrim, (void **)&g_orig_draw_prim);
        PatchVtableEntry(vtable, VT_DRAW_INDEXED, (void *)HookDrawIdx, (void **)&g_orig_draw_idx);
        PatchVtableEntry(vtable, VT_DRAW_PRIMITIVE_UP, (void *)HookDrawPrimUP, (void **)&g_orig_draw_prim_up);
        PatchVtableEntry(vtable, VT_DRAW_INDEXED_UP, (void *)HookDrawIdxUP, (void **)&g_orig_draw_idx_up);
    }
    PatchVtableEntry(vtable, VT_PRESENT, (void *)HookPresent, (void **)&g_orig_present);
    PatchVtableEntry(vtable, VT_RESET, (void *)HookReset, (void **)&g_orig_reset);
    PatchVtableEntry(vtable, VT_SET_VS_CONST_F, (void *)HookSetVSConst, (void **)&g_orig_set_vs_const);
    PatchVtableEntry(vtable, VT_SET_PS_CONST_F, (void *)HookSetPSConst, (void **)&g_orig_set_ps_const);
    g_hooked_vtable = vtable;
    Log("render scale: device hooks installed");
}

/* Append the render-scale resolutions to the game's mode table (once). */
void __cdecl AppendRenderScaleModes(char *device)
{
    static int done;
    int count, i, k, added = 0;
    int *table, *fresh, w, h;

    if (done || !g_scale_count)
        return;
    count = *(int *)(device + DEVICE_MODE_COUNT_OFS);
    table = *(int **)(device + DEVICE_MODE_TABLE_OFS);
    if (!table || count <= 0 || count > 256)
        return;                                   /* mode table not built yet */
    done = 1;

    fresh = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)(count + MAX_RENDER_SCALES) * 16);
    if (!fresh)
        return;
    memcpy(fresh, table, (size_t)count * 16);
    for (k = 0; k < g_scale_count; k++) {
        w = ((int)(*(int *)(device + DEVICE_DESKTOP_W_OFS) * g_scales[k] + 0.5)) & ~1;
        h = ((int)(*(int *)(device + DEVICE_DESKTOP_H_OFS) * g_scales[k] + 0.5)) & ~1;
        if (w > MAX_RENDER_DIM || h > MAX_RENDER_DIM)
            continue;
        for (i = 0; i < count + added; i++)
            if (fresh[i * 4] == w && fresh[i * 4 + 1] == h)
                break;
        if (i < count + added)
            continue;
        fresh[(count + added) * 4 + 0] = w;
        fresh[(count + added) * 4 + 1] = h;
        fresh[(count + added) * 4 + 2] = fresh[(count - 1) * 4 + 2];
        fresh[(count + added) * 4 + 3] = fresh[(count - 1) * 4 + 3];
        added++;
        if (g_log)
            fprintf(g_log, "render scale mode added: %dx%d (x%.2f)\n", w, h, g_scales[k]);
    }
    *(int **)(device + DEVICE_MODE_TABLE_OFS) = fresh;
    *(int *)(device + DEVICE_MODE_COUNT_OFS) = count + added;
    fflush(g_log);
}

/* Entry stub for sub_6E1B70: add the modes, then run the original function. */
extern void ModeLookupStub(void);

__asm__(
    ".text\n"
    ".globl _ModeLookupStub\n"
    "_ModeLookupStub:\n"
    "    pushl %ecx\n"
    "    pushl %ecx\n"
    "    call _AppendRenderScaleModes\n"
    "    addl $4, %esp\n"
    "    popl %ecx\n"
    "    jmp *_g_lookup_tramp\n"
);

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
    return Matches(ADDR_MODE_LOOKUP, k_modelookup_orig, sizeof k_modelookup_orig) &&
           Matches(ADDR_RT_H_1, k_rt_h_orig, sizeof k_rt_h_orig) &&
           Matches(ADDR_RT_W_1, k_rt_w_orig, sizeof k_rt_w_orig) &&
           Matches(ADDR_RT_H_2, k_rt_h_orig, sizeof k_rt_h_orig) &&
           Matches(ADDR_RT_W_2, k_rt_w_orig, sizeof k_rt_w_orig) &&
           Matches(ADDR_RT_H_3, k_rt_h_orig, sizeof k_rt_h_orig) &&
           Matches(ADDR_RT_W_3, k_rt_w_orig, sizeof k_rt_w_orig);
}

static DWORD WINAPI EarlyHookThread(LPVOID param)
{
    (void)param;
    while (!g_hooked_vtable) {
        InstallRenderScaleHooks();
        Sleep(1);
    }
    return 0;
}

void RenderInstall(void)
{
    int ok;

    /* the effect render targets take their size from the backbuffer size: scale it */
    ok = PatchCall(ADDR_RT_H_1, ScaledHeightStub) && PatchCall(ADDR_RT_H_2, ScaledHeightStub) &&
         PatchCall(ADDR_RT_H_3, ScaledHeightStub) && PatchCall(ADDR_RT_W_1, ScaledWidthStub) &&
         PatchCall(ADDR_RT_W_2, ScaledWidthStub) && PatchCall(ADDR_RT_W_3, ScaledWidthStub);
    Log(ok ? "render scale patched" : "render scale patch FAILED");

    /* the D3D9 device appears early; hook it from its own thread as soon as it exists */
    CreateThread(NULL, 0, EarlyHookThread, NULL, 0, NULL);

    if (g_scale_count) {
        g_lookup_tramp = MakeTrampoline(k_modelookup_orig, sizeof k_modelookup_orig, ADDR_MODE_LOOKUP_2);
        if (g_lookup_tramp)
            Log(PatchJmp(ADDR_MODE_LOOKUP, ModeLookupStub, 5) ? "render scale hook installed" : "render scale hook FAILED");
    }
}

/* Debug (TraceRT=1): a file "<ini>.scale" holding a number sets the render scale at run time (for scripted tests). */
static void DebugScaleRequest(void)
{
    static int frames;
    char path[MAX_PATH + 8];
    FILE *f;
    double v;

    if (++frames % 30)
        return;
    snprintf(path, sizeof path, "%s.scale", g_ini_path);
    f = fopen(path, "r");
    if (!f)
        return;
    if (fscanf(f, "%lf", &v) == 1 && v >= 0.25 && v <= 4.0) {
        g_render_scale = v;
        ((BuildTargetsFn)ADDR_BUILD_TARGETS)((void *)ADDR_DEVICE_OBJECT);
        Log("render scale changed by debug file");
    }
    fclose(f);
    remove(path);
}

void RenderFrame(int cur, int titles)
{
    static DWORD last_diag;
    static int diag_lines;

    if (g_trace_passes) {
        static int frames;

        if (frames++ % 20 == 0) {
            char path[MAX_PATH + 8];
            FILE *f;

            snprintf(path, sizeof path, "%s.skip", g_ini_path);
            f = fopen(path, "r");
            g_skip_scaled = -1;
            if (f) {
                if (fscanf(f, "%d", &g_skip_scaled) != 1)
                    g_skip_scaled = -1;
                fclose(f);
            }
        }
    }
    if (g_trace_rt)
        DebugScaleRequest();
    if (g_trace_rt && g_render_scale != 1.0 && GetTickCount() - last_diag > 3000 && diag_lines < 40) {
        char buf[200];

        last_diag = GetTickCount();
        diag_lines++;
        snprintf(buf, sizeof buf, "diag: main=%p twin=%p %ux%u engine=%p sets=%u scaledBinds=%u twinUsed=%u twinFail=%u scale=%.2f",
                 (void *)g_depth_main, (void *)g_depth_twin, g_twin_w, g_twin_h, (void *)g_engine_depth,
                 g_diag_depth_sets, g_diag_scaled_binds, g_diag_twin_used, g_diag_twin_fail, g_render_scale);
        Log(buf);
    }
    SyncModeTable();
    InstallRenderScaleHooks();
    if (g_trace_rt) {
        static int frames;

        if (cur && cur != titles && ++frames == 400)
            g_trace_budget = 6000;
    }
    if (g_dump_textures) {
        static int frames;

        if (cur && cur != titles && ++frames == 600)
            DumpTextures();
    }
}
