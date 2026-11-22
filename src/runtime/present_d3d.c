/*
 * present_d3d: the picture through Direct3D 11 instead of GDI.
 *
 * present.c's GDI path converts and scales the frame on the CPU and copies
 * it into the window with SetDIBitsToDevice: at 2560x1440 a whole new frame
 * is ~2.5 ms of the game thread, and GDI has no notion of the display's
 * refresh. Here the CPU only converts the rows that changed (scaler.c at
 * 1:1) and uploads them to a texture; the GPU scales it into the letterbox
 * rectangle of a flip-model swap chain, and DWM shows the newest frame at
 * each refresh, without tearing.
 *
 * Filters, as present.c's: sharp (a pixel shader: nearest inside a texel, a
 * one-pixel blend at its edges, the same arithmetic as scaler.c's axis
 * tables), smooth (bilinear), nearest and integer (point sampling; integer's
 * rectangle is already whole multiples).
 *
 * Anything that fails (no Direct3D 11 or DXGI 1.2, no d3dcompiler_47.dll, a
 * window on a desktop without a display, a lost device that will not come
 * back) turns it off for the run and present.c uses GDI: present_d3d_draw
 * returns 0.
 *
 * Opt-in (--present d3d11): measured on the real screen at 2560x1440 (1:1
 * and scaled up, still and scrolling) it costs the game thread the same as
 * present.c's change-only GDI path (docs/performance.md), so GDI stays the
 * default and this is there for flip-model presentation, 4K upscaling, and
 * the machines where GDI is slower.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

#include "present.h"
#include "present_d3d.h"

int present_d3d_enabled;              /* --present d3d11 */

static int g_failed;                  /* gave up: GDI for the rest of the run */
static HWND g_wnd;
static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static IDXGISwapChain1* g_sc;
static ID3D11RenderTargetView* g_rtv;
static ID3D11Texture2D* g_tex;
static ID3D11ShaderResourceView* g_srv;
static ID3D11VertexShader* g_vs;
static ID3D11PixelShader* g_ps;
static ID3D11SamplerState* g_point, *g_linear;
static ID3D11Buffer* g_cb;
static int g_sw, g_sh;                /* swap chain size */
static int g_tw, g_th;                /* texture size */

#define REL(p) do { if (p) { IUnknown_Release((IUnknown*)(p)); (p) = NULL; } } while (0)

static const char k_hlsl[] =
    "Texture2D t : register(t0);\n"
    "SamplerState s : register(s0);\n"
    "cbuffer C : register(b0) { float4 p; };   /* source w, h; sharp: 1; scale */\n"
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) {\n"
    "    V o; float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o;\n"
    "}\n"
    "float4 ps(V i) : SV_Target {\n"
    "    float2 uv = i.uv;\n"
    "    if (p.z > 0.5) {   /* sharp: the texel's own colour, blended over one picture pixel at its edges */\n"
    "        float2 x = uv * p.xy - 0.5, b = floor(x);\n"
    "        float2 f = saturate((x - b - 0.5) * p.w + 0.5);\n"
    "        uv = (b + 0.5 + f) / p.xy;\n"
    "    }\n"
    "    return float4(t.Sample(s, uv).rgb, 1);\n"
    "}\n";

typedef HRESULT (WINAPI *compile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                    UINT, UINT, ID3DBlob**, ID3DBlob**);

static void drop_all(void) {
    if (g_ctx) ID3D11DeviceContext_ClearState(g_ctx);
    REL(g_srv); REL(g_tex); REL(g_rtv); REL(g_sc); REL(g_cb); REL(g_point); REL(g_linear);
    REL(g_vs); REL(g_ps); REL(g_ctx); REL(g_dev);
    g_wnd = NULL;
    g_sw = g_sh = g_tw = g_th = 0;
}

static int fail(const char* what, HRESULT hr) {
    fprintf(stderr, "[present] Direct3D 11: %s failed (0x%08lX); drawing with GDI\n", what, (unsigned long)hr);
    drop_all();
    g_failed = 1;
    return 0;
}

static int make_rtv(void) {
    ID3D11Texture2D* bb = NULL;
    HRESULT hr = IDXGISwapChain1_GetBuffer(g_sc, 0, &IID_ID3D11Texture2D, (void**)&bb);
    if (FAILED(hr)) return fail("GetBuffer", hr);
    hr = ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource*)bb, NULL, &g_rtv);
    REL(bb);
    return SUCCEEDED(hr) ? 1 : fail("CreateRenderTargetView", hr);
}

static int init(HWND wnd, int cw, int ch) {
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, 3, D3D11_SDK_VERSION, &g_dev, NULL, &g_ctx);
    if (FAILED(hr)) return fail("D3D11CreateDevice", hr);

    IDXGIDevice* xd = NULL;
    IDXGIAdapter* ad = NULL;
    IDXGIFactory2* fac = NULL;
    hr = ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void**)&xd);
    if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(xd, &ad);
    if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void**)&fac);
    REL(ad); REL(xd);
    if (FAILED(hr)) return fail("IDXGIFactory2", hr);
    DXGI_SWAP_CHAIN_DESC1 d;
    memset(&d, 0, sizeof d);
    d.Width = cw; d.Height = ch;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_NONE;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown*)g_dev, wnd, &d, NULL, NULL, &g_sc);
    if (SUCCEEDED(hr)) IDXGIFactory2_MakeWindowAssociation(fac, wnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    REL(fac);
    if (FAILED(hr)) return fail("CreateSwapChainForHwnd", hr);
    g_sw = cw; g_sh = ch;
    if (!make_rtv()) return 0;

    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    compile_t compile = dc ? (compile_t)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!compile) return fail("d3dcompiler_47.dll", E_NOINTERFACE);
    ID3DBlob* vb = NULL, *pb = NULL, *err = NULL;
    hr = compile(k_hlsl, sizeof k_hlsl - 1, "present", NULL, NULL, "vs", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vb, &err);
    if (SUCCEEDED(hr)) hr = compile(k_hlsl, sizeof k_hlsl - 1, "present", NULL, NULL, "ps", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pb, &err);
    if (err) { fprintf(stderr, "[present] %s\n", (const char*)ID3D10Blob_GetBufferPointer(err)); REL(err); }
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &g_vs);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &g_ps);
    REL(vb); REL(pb);
    if (FAILED(hr)) return fail("shaders", hr);

    D3D11_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    hr = ID3D11Device_CreateSamplerState(g_dev, &sd, &g_point);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateSamplerState(g_dev, &sd, &g_linear);
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_cb);
    if (FAILED(hr)) return fail("samplers", hr);
    g_wnd = wnd;
    fprintf(stderr, "[present] Direct3D 11: flip-model swap chain %dx%d\n", cw, ch);
    return 1;
}

static int make_texture(int w, int h) {
    REL(g_srv); REL(g_tex);
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8X8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_tex);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource*)g_tex, NULL, &g_srv);
    if (FAILED(hr)) return fail("CreateTexture2D", hr);
    g_tw = w; g_th = h;
    return 1;
}

int present_d3d_ready(HWND wnd) {
    return present_d3d_enabled && !g_failed && wnd && (!g_wnd || g_wnd == wnd);
}

int present_d3d_draw(HWND wnd, const uint32_t* pic, int w, int h, int y0, int y1, int cw, int ch, const RECT* r,
                     int filter, int full) {
    HRESULT hr;
    if (!present_d3d_ready(wnd)) return 0;
    if (!g_dev && !init(wnd, cw, ch)) return 0;
    if (cw != g_sw || ch != g_sh) {   /* resized, fullscreen: the buffers follow the client area */
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 0, NULL, NULL);
        REL(g_rtv);
        hr = IDXGISwapChain1_ResizeBuffers(g_sc, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) return fail("ResizeBuffers", hr);
        g_sw = cw; g_sh = ch;
        if (!make_rtv()) return 0;
        full = 1;
    }
    if (w != g_tw || h != g_th) {
        if (!make_texture(w, h)) return 0;
        y0 = 0; y1 = h;
    }
    if (full) { y0 = 0; y1 = h; }
    if (y0 < y1) {
        D3D11_BOX box = { 0, (UINT)y0, 0, (UINT)w, (UINT)y1, 1 };
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource*)g_tex, 0, &box, pic + (size_t)y0 * w, w * 4, 0);
    }
    float black[4] = { 0, 0, 0, 1 };
    int dw = r->right - r->left, dh = r->bottom - r->top;
    float cb[4] = { (float)w, (float)h, filter == PRES_SHARP && dw > w ? 1.0f : 0.0f, (float)dw / w };
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource*)g_cb, 0, NULL, cb, 0, 0);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_rtv, black);
    D3D11_VIEWPORT vp = { (float)r->left, (float)r->top, (float)dw, (float)dh, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &g_srv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, filter == PRES_NEAREST || filter == PRES_INTEGER ? &g_point : &g_linear);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_cb);
    ID3D11DeviceContext_Draw(g_ctx, 3, 0);
    /* 0: no wait for the refresh. present.c already shows one frame a refresh,
     * and in the flip model DWM shows the newest at each one. */
    hr = IDXGISwapChain1_Present(g_sc, 0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        static int lost;
        fprintf(stderr, "[present] Direct3D 11: device lost (0x%08lX)%s\n", (unsigned long)hr, lost ? "" : "; starting again");
        drop_all();
        if (lost++) g_failed = 1;   /* once more, then GDI */
        return 0;
    }
    if (FAILED(hr)) return fail("Present", hr);
    return 1;
}
