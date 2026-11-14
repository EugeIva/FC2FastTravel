#include "overlay.h"
#include <d3d9.h>
#include "archive.h"
#include "game.h"
#include "log.h"
#include "mapscreen.h"

namespace {

// Vtable slots.
const int kD3D_CreateDevice = 16;
const int kDev_Present = 17;

typedef IDirect3D9*(WINAPI* Direct3DCreate9_t)(UINT);
typedef HRESULT(__stdcall* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                           D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(__stdcall* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND,
                                      const RGNDATA*);

Direct3DCreate9_t g_realCreate9 = nullptr;
CreateDevice_t g_realCreateDevice = nullptr;
Present_t g_realPresent = nullptr;

volatile LONG g_screenW = 0, g_screenH = 0;

// Font atlas: ASCII 32..127 in a 16 x 6 grid.
const int kCellW = 16, kCellH = 28, kAtlasW = 256, kAtlasH = 256;
int g_glyphAdvance = 12;
IDirect3DDevice9* g_fontDevice = nullptr;
IDirect3DTexture9* g_fontTexture = nullptr;

// Textures loaded from the game's archives, by name hash.
struct GameTexture {
    unsigned long hash;
    IDirect3DDevice9* device;
    IDirect3DTexture9* texture;   // null if loading failed
};
const int kMaxGameTextures = 4;
GameTexture g_gameTextures[kMaxGameTextures];
int g_gameTextureCount = 0;

// The device being drawn to, valid during MapScreenRender().
IDirect3DDevice9* g_dev = nullptr;

struct Vertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};
const DWORD kFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

void* PatchVtable(void* object, int slot, void* hook) {
    void** vtable = *(void***)object;
    if (vtable[slot] == hook) return nullptr;
    DWORD old;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    void* orig = vtable[slot];
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    return orig;
}

bool BuildFont(IDirect3DDevice9* dev) {
    if (g_fontTexture && g_fontDevice == dev) return true;
    if (g_fontTexture) {
        g_fontTexture->Release();
        g_fontTexture = nullptr;
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kAtlasW;
    bi.bmiHeader.biHeight = -kAtlasH;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        return false;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HFONT font = CreateFontA(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, ANSI_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             FIXED_PITCH | FF_MODERN, "Consolas");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkColor(dc, RGB(0, 0, 0));
    SetTextColor(dc, RGB(255, 255, 255));
    ZeroMemory(bits, kAtlasW * kAtlasH * 4);
    SIZE sz = {};
    if (GetTextExtentPoint32A(dc, "M", 1, &sz) && sz.cx > 0 && sz.cx <= kCellW)
        g_glyphAdvance = sz.cx;
    for (int c = 32; c < 128; ++c) {
        char ch = (char)c;
        int col = (c - 32) % 16, row = (c - 32) / 16;
        RECT cell = {col * kCellW, row * kCellH, (col + 1) * kCellW, (row + 1) * kCellH};
        ExtTextOutA(dc, cell.left, cell.top + 2, ETO_CLIPPED, &cell, &ch, 1, nullptr);
    }
    GdiFlush();

    bool ok = false;
    if (SUCCEEDED(dev->CreateTexture(kAtlasW, kAtlasH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                     &g_fontTexture, nullptr))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(g_fontTexture->LockRect(0, &lr, nullptr, 0))) {
            const DWORD* src = (const DWORD*)bits;
            for (int y = 0; y < kAtlasH; ++y) {
                DWORD* dst = (DWORD*)((BYTE*)lr.pBits + y * lr.Pitch);
                for (int x = 0; x < kAtlasW; ++x) {
                    DWORD a = src[y * kAtlasW + x] & 0xFF;  // white text: any channel
                    dst[x] = (a << 24) | 0x00FFFFFF;
                }
            }
            g_fontTexture->UnlockRect(0);
            g_fontDevice = dev;
            ok = true;
        }
    }
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(font);
    DeleteObject(bmp);
    DeleteDC(dc);
    if (!ok && g_fontTexture) {
        g_fontTexture->Release();
        g_fontTexture = nullptr;
    }
    return ok;
}

void SetupStates(IDirect3DDevice9* dev, int w, int h) {
    D3DVIEWPORT9 vp = {0, 0, (DWORD)w, (DWORD)h, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(kFVF);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_CLIPPING, TRUE);
    dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetTexture(0, nullptr);
}

void RenderOverlay(IDirect3DDevice9* dev) {
    IDirect3DSurface9* back = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return;
    D3DSURFACE_DESC desc;
    back->GetDesc(&desc);
    InterlockedExchange(&g_screenW, (LONG)desc.Width);
    InterlockedExchange(&g_screenH, (LONG)desc.Height);

    if (!MapScreenIsOpen() || !BuildFont(dev)) {
        back->Release();
        return;
    }
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) {
        back->Release();
        return;
    }
    IDirect3DSurface9* oldTarget = nullptr;
    dev->GetRenderTarget(0, &oldTarget);
    dev->SetRenderTarget(0, back);

    bool began = SUCCEEDED(dev->BeginScene());
    SetupStates(dev, (int)desc.Width, (int)desc.Height);
    g_dev = dev;
    MapScreenRender((int)desc.Width, (int)desc.Height);
    g_dev = nullptr;
    if (began) dev->EndScene();

    if (oldTarget) {
        dev->SetRenderTarget(0, oldTarget);
        oldTarget->Release();
    }
    saved->Apply();
    saved->Release();
    back->Release();
}

HRESULT __stdcall Hook_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd,
                               const RGNDATA* dirty) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("Direct3D: first Present on thread %lu", GetCurrentThreadId());
    }
    __try {
        RenderOverlay(dev);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool faultLogged = false;
        if (!faultLogged) {
            faultLogged = true;
            Log("ERROR: drawing the map screen faulted");
        }
    }
    return g_realPresent(dev, src, dst, wnd, dirty);
}

HRESULT __stdcall Hook_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus,
                                    DWORD flags, D3DPRESENT_PARAMETERS* pp,
                                    IDirect3DDevice9** out) {
    HRESULT hr = g_realCreateDevice(d3d, adapter, type, focus, flags, pp, out);
    if (FAILED(hr) || !out || !*out) return hr;
    void* orig = PatchVtable(*out, kDev_Present, (void*)Hook_Present);
    if (orig) g_realPresent = (Present_t)orig;
    Log("Direct3D: device created (%ux%u, %s, flags 0x%lX), Present %s",
        pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
        (pp && pp->Windowed) ? "windowed" : "fullscreen", flags,
        g_realPresent ? "intercepted" : "NOT intercepted");
    return hr;
}

IDirect3D9* WINAPI Hook_Direct3DCreate9(UINT sdk) {
    IDirect3D9* d3d = g_realCreate9(sdk);
    if (d3d) {
        void* orig = PatchVtable(d3d, kD3D_CreateDevice, (void*)Hook_CreateDevice);
        if (orig) g_realCreateDevice = (CreateDevice_t)orig;
        Log("Direct3D: Direct3DCreate9 called, device creation %s",
            g_realCreateDevice ? "intercepted" : "NOT intercepted");
    }
    return d3d;
}

void Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
          DWORD color) {
    if (!g_dev) return;
    Vertex v[4] = {
        {x0 - 0.5f, y0 - 0.5f, 0.0f, 1.0f, color, u0, v0},
        {x1 - 0.5f, y0 - 0.5f, 0.0f, 1.0f, color, u1, v0},
        {x0 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, color, u0, v1},
        {x1 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, color, u1, v1},
    };
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
}

IDirect3DTexture9* CreateGameTexture(IDirect3DDevice9* dev, unsigned long hash) {
    ArchiveTexture src;
    if (!ArchiveLoadTexture(hash, &src)) return nullptr;
    // Skip top levels above 2048 pixels; they are not needed on screen.
    int skip = 0;
    const unsigned char* data = src.data;
    unsigned long left = src.dataSize;
    int w = src.width, h = src.height;
    while ((w > 2048 || h > 2048) && skip + 1 < src.mips) {
        unsigned long sz = (unsigned long)((w + 3) / 4) * ((h + 3) / 4) * 8;
        if (sz > left) break;
        data += sz;
        left -= sz;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        ++skip;
    }
    int levels = src.mips - skip;
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(dev->CreateTexture(w, h, levels, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &tex, nullptr)) ||
        !tex) {
        Log("Map image: Direct3D could not create a %dx%d DXT1 texture", w, h);
        ArchiveFree(&src);
        return nullptr;
    }
    bool ok = true;
    for (int i = 0; i < levels && ok; ++i) {
        int bw = (w + 3) / 4, bh = (h + 3) / 4;
        unsigned long sz = (unsigned long)bw * bh * 8;
        D3DLOCKED_RECT lr;
        if (sz > left || FAILED(tex->LockRect(i, &lr, nullptr, 0))) {
            ok = false;
            break;
        }
        for (int row = 0; row < bh; ++row)
            memcpy((BYTE*)lr.pBits + row * lr.Pitch, data + (unsigned long)row * bw * 8, bw * 8);
        tex->UnlockRect(i);
        data += sz;
        left -= sz;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    ArchiveFree(&src);
    if (!ok) {
        Log("Map image: texture data is shorter than expected");
        tex->Release();
        return nullptr;
    }
    return tex;
}

}  // namespace

bool DrawGameTexture(unsigned long nameHash, float x0, float y0, float x1, float y1, DWORD color) {
    if (!g_dev) return false;
    GameTexture* t = nullptr;
    for (int i = 0; i < g_gameTextureCount; ++i)
        if (g_gameTextures[i].hash == nameHash) t = &g_gameTextures[i];
    if (!t) {
        if (g_gameTextureCount >= kMaxGameTextures) return false;
        t = &g_gameTextures[g_gameTextureCount++];
        t->hash = nameHash;
        t->device = nullptr;
        t->texture = nullptr;
    }
    if (t->device != g_dev) {
        // First use, or the game made a new device: (re)load. A failure is
        // remembered so the archives are not searched every frame.
        if (t->texture && t->device) t->texture->Release();
        t->device = g_dev;
        t->texture = CreateGameTexture(g_dev, nameHash);
    }
    if (!t->texture) return false;
    g_dev->SetTexture(0, t->texture);
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    g_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    Quad(x0, y0, x1, y1, 0.0f, 0.0f, 1.0f, 1.0f, color);
    g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    g_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    g_dev->SetTexture(0, nullptr);
    return true;
}

void OverlayInstall() {
    void* orig = GameHookImport("d3d9.dll", "Direct3DCreate9", (void*)Hook_Direct3DCreate9);
    if (orig) g_realCreate9 = (Direct3DCreate9_t)orig;
    Log(orig ? "Direct3D: Direct3DCreate9 import hooked"
             : "ERROR: could not hook Direct3DCreate9, the map screen cannot be drawn");
}

void OverlayGetScreenSize(int* w, int* h) {
    *w = (int)g_screenW;
    *h = (int)g_screenH;
}

void DrawRect(float x0, float y0, float x1, float y1, DWORD color) {
    if (!g_dev) return;
    g_dev->SetTexture(0, nullptr);
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    Quad(x0, y0, x1, y1, 0, 0, 0, 0, color);
}

void DrawFrame(float x0, float y0, float x1, float y1, float t, DWORD color) {
    DrawRect(x0, y0, x1, y0 + t, color);
    DrawRect(x0, y1 - t, x1, y1, color);
    DrawRect(x0, y0 + t, x0 + t, y1 - t, color);
    DrawRect(x1 - t, y0 + t, x1, y1 - t, color);
}

void DrawDiamond(float cx, float cy, float r, DWORD color) {
    if (!g_dev) return;
    g_dev->SetTexture(0, nullptr);
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    Vertex v[4] = {
        {cx, cy - r, 0.0f, 1.0f, color, 0, 0},
        {cx + r, cy, 0.0f, 1.0f, color, 0, 0},
        {cx - r, cy, 0.0f, 1.0f, color, 0, 0},
        {cx, cy + r, 0.0f, 1.0f, color, 0, 0},
    };
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
}

void DrawTextAt(float x, float y, const char* text, DWORD color) {
    if (!g_dev || !g_fontTexture) return;
    g_dev->SetTexture(0, g_fontTexture);
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    x = (float)(int)x;
    y = (float)(int)y;
    for (const char* p = text; *p; ++p) {
        int c = (unsigned char)*p;
        if (c < 32 || c > 127) c = '?';
        if (c != ' ') {
            int col = (c - 32) % 16, row = (c - 32) / 16;
            float u0 = (float)(col * kCellW) / kAtlasW, v0 = (float)(row * kCellH) / kAtlasH;
            float u1 = (float)(col * kCellW + kCellW) / kAtlasW;
            float v1 = (float)(row * kCellH + kCellH) / kAtlasH;
            Quad(x, y, x + kCellW, y + kCellH, u0, v0, u1, v1, color);
        }
        x += (float)g_glyphAdvance;
    }
}

float TextWidth(const char* text) { return (float)(lstrlenA(text) * g_glyphAdvance); }

float TextHeight() { return (float)kCellH; }
