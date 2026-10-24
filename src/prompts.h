#pragma once
#include "common.h"
#include "game.h"

extern IDirect3DTexture9 *g_buttons_tex;
extern SetTextureFn g_orig_set_texture;
extern int g_prompt_glyphs;
extern int g_prompt_style[2];
HRESULT STDMETHODCALLTYPE HookSetTexture(IDirect3DDevice9 *dev, DWORD stage, IDirect3DBaseTexture9 *tex);
int PromptsReady(void);
void PromptsInstall(void);
void PromptsFrame(void);
