/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * D3D9 device hooks: Present (cutscene skip fade, frame statistics), SetTexture / CreateTexture (button prompts, texture
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
 * Black overlay of the given opacity over the real backbuffer (cutscene skip fades). The device state is saved in a state
 * block and put back.
 */
static void DrawFadeOverlay(IDirect3DDevice9 *dev, float alpha)
{
    IDirect3DStateBlock9 *sb = NULL;
    IDirect3DSurface9 *bb = NULL, *old_rt = NULL;
    D3DSURFACE_DESC desc;
    D3DVIEWPORT9 vp;
    struct { float x, y, z, rhw; DWORD color; } v[4];
    DWORD color = (DWORD)(alpha * 255.0f + 0.5f) << 24;
    int i;

    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
        return;
    IDirect3DSurface9_GetDesc(bb, &desc);
    IDirect3DDevice9_GetRenderTarget(dev, 0, &old_rt);
    if (SUCCEEDED(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &sb)) && sb) {
        vp = (D3DVIEWPORT9){ 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
        for (i = 0; i < 4; i++) {
            v[i].x = (i & 1) ? (float)desc.Width : 0.0f;
            v[i].y = (i & 2) ? (float)desc.Height : 0.0f;
            v[i].z = 0.0f;
            v[i].rhw = 1.0f;
            v[i].color = color;
        }
        IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
        IDirect3DDevice9_SetViewport(dev, &vp);
        IDirect3DDevice9_SetVertexShader(dev, NULL);
        IDirect3DDevice9_SetPixelShader(dev, NULL);
        IDirect3DDevice9_SetTexture(dev, 0, NULL);
        IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_SRGBWRITEENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        if (SUCCEEDED(IDirect3DDevice9_BeginScene(dev))) {
            IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof v[0]);
            IDirect3DDevice9_EndScene(dev);
        }
        IDirect3DStateBlock9_Apply(sb);
        IDirect3DStateBlock9_Release(sb);
    }
    if (old_rt) {
        IDirect3DDevice9_SetRenderTarget(dev, 0, old_rt);
        IDirect3DSurface9_Release(old_rt);
    }
    IDirect3DSurface9_Release(bb);
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

static HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty)
{
    if (g_trace_skip && g_skip_first_tick && GetTickCount() - g_skip_first_tick < 4000) {
        char buf[160];

        snprintf(buf, sizeof buf, "present +%lu ms black %d wipe %.3f dir %.3f style %02x pend %d/%d be0 %d",
                 GetTickCount() - g_skip_first_tick, g_cutscene_skipping, *(float *)0x950780, *(float *)0x950784,
                 *(uint8_t *)0x95077C, *(int *)0x87658C, *(int *)0x951B80 != 0, *(int *)0x951BE0);
        Log(buf);
    }
    if (g_skip_fade > 0.001f)
        DrawFadeOverlay(dev, g_skip_fade);
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
    return 1;
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
