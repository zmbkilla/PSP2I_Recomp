/* Window output through a D3D11 swap chain. See present.h.
 *
 * GDI's StretchDIBits scales on the CPU, on the game's thread: at 1080p and
 * above that costs milliseconds per frame, which 60 fps does not have to
 * spare. Here the frame (480x272) is uploaded once and a single triangle
 * scales it on the GPU. Nearest-texel sampling keeps the pixels sharp. */
#include "present.h"

void present_fit(int cw, int ch, int fw, int fh, int *x, int *y, int *w, int *h) {
    if (cw <= 0 || ch <= 0 || fw <= 0 || fh <= 0) { *x = *y = *w = *h = 0; return; }
    /* cw/ch vs fw/fh without floats: wider window -> bars left and right. */
    if ((long long)cw * fh > (long long)ch * fw) {
        *h = ch;
        *w = (int)((long long)ch * fw / fh);
    } else {
        *w = cw;
        *h = (int)((long long)cw * fh / fw);
    }
    *x = (cw - *w) / 2;
    *y = (ch - *h) / 2;
}

#ifdef _WIN32
#define COBJMACROS
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

static HWND                      g_wnd;
static ID3D11Device             *g_dev;
static ID3D11DeviceContext      *g_ctx;
static IDXGISwapChain           *g_sc;
static ID3D11RenderTargetView   *g_rtv;
static ID3D11Texture2D          *g_tex;
static ID3D11ShaderResourceView *g_srv;
static ID3D11VertexShader       *g_vs;
static ID3D11PixelShader        *g_ps;
static ID3D11SamplerState       *g_smp;
static ID3D11RasterizerState    *g_rs;
static int g_tex_w, g_tex_h, g_buf_w, g_buf_h, g_resize, g_ok;

/* One triangle covering the viewport; the viewport is the letterboxed rect. */
static const char BLIT_SRC[] =
    "Texture2D t : register(t0);\n"
    "SamplerState s : register(s0);\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vsb(uint id : SV_VertexID) {\n"
    "    V o; float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    o.uv = uv; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;\n"
    "}\n"
    "float4 psb(V i) : SV_Target { return float4(t.Sample(s, i.uv).rgb, 1); }\n";

static ID3DBlob *compile(const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(BLIT_SRC, sizeof BLIT_SRC - 1, "present", NULL, NULL, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "present: shader %s: %s\n", entry, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static void release_rtv(void) {
    if (g_rtv) { ID3D11RenderTargetView_Release(g_rtv); g_rtv = NULL; }
}

static int make_rtv(void) {
    ID3D11Texture2D *back = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(g_sc, 0, &IID_ID3D11Texture2D, (void **)&back))) return -1;
    HRESULT hr = ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)back, NULL, &g_rtv);
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(back, &d);
    g_buf_w = (int)d.Width; g_buf_h = (int)d.Height;
    ID3D11Texture2D_Release(back);
    return FAILED(hr) ? -1 : 0;
}

static int make_texture(int w, int h) {
    if (g_srv) { ID3D11ShaderResourceView_Release(g_srv); g_srv = NULL; }
    if (g_tex) { ID3D11Texture2D_Release(g_tex); g_tex = NULL; }
    D3D11_TEXTURE2D_DESC d;
    memset(&d, 0, sizeof d);
    d.Width = (UINT)w; d.Height = (UINT)h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   /* 0xAARRGGBB little-endian = B, G, R, A */
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &g_tex))) return -1;
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_tex, NULL, &g_srv))) return -1;
    g_tex_w = w; g_tex_h = h;
    return 0;
}

int present_init(HWND wnd) {
    g_wnd = wnd;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   NULL, 0, D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) { fprintf(stderr, "present: no D3D11 device (0x%08lX); using GDI\n", (unsigned long)hr); return -1; }

    IDXGIDevice *xd = NULL; IDXGIAdapter *ad = NULL; IDXGIFactory *fac = NULL;
    if (FAILED(ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void **)&xd)) ||
        FAILED(IDXGIDevice_GetAdapter(xd, &ad)) ||
        FAILED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&fac))) {
        fprintf(stderr, "present: no DXGI factory; using GDI\n");
        if (xd) IDXGIDevice_Release(xd);
        if (ad) IDXGIAdapter_Release(ad);
        return -1;
    }
    DXGI_SWAP_CHAIN_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   /* width/height 0: the window's client size */
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = wnd;
    sd.Windowed = TRUE;                                  /* fullscreen is a borderless window */
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;       /* Windows 10 */
    hr = IDXGIFactory_CreateSwapChain(fac, (IUnknown *)g_dev, &sd, &g_sc);
    if (FAILED(hr)) {                                    /* older Windows */
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; sd.BufferCount = 1;
        hr = IDXGIFactory_CreateSwapChain(fac, (IUnknown *)g_dev, &sd, &g_sc);
    }
    if (SUCCEEDED(hr)) IDXGIFactory_MakeWindowAssociation(fac, wnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    IDXGIFactory_Release(fac); IDXGIAdapter_Release(ad); IDXGIDevice_Release(xd);
    if (FAILED(hr)) { fprintf(stderr, "present: no swap chain (0x%08lX); using GDI\n", (unsigned long)hr); return -1; }

    ID3DBlob *vsb = compile("vsb", "vs_4_0"), *psb = compile("psb", "ps_4_0");
    if (!vsb || !psb) return -1;
    ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &g_vs);
    ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &g_ps);
    ID3D10Blob_Release(vsb); ID3D10Blob_Release(psb);

    D3D11_SAMPLER_DESC smp;
    memset(&smp, 0, sizeof smp);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(g_dev, &smp, &g_smp);
    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_rs);
    if (!g_vs || !g_ps || !g_smp || !g_rs || make_rtv() != 0) {
        fprintf(stderr, "present: setup failed; using GDI\n");
        return -1;
    }
    g_ok = 1;
    fprintf(stderr, "present: window output on the GPU (swap chain %dx%d)\n", g_buf_w, g_buf_h);
    return 0;
}

void present_resized(void) { g_resize = 1; }
int present_active(void) { return g_ok; }

int present_frame(const uint32_t *px, int w, int h) {
    if (!g_ok) return -1;
    RECT rc;
    GetClientRect(g_wnd, &rc);
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return 0;                    /* minimised */
    if (g_resize || cw != g_buf_w || ch != g_buf_h) {
        g_resize = 0;
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 0, NULL, NULL);
        release_rtv();
        if (FAILED(IDXGISwapChain_ResizeBuffers(g_sc, 0, (UINT)cw, (UINT)ch, DXGI_FORMAT_UNKNOWN, 0)) || make_rtv() != 0) {
            fprintf(stderr, "present: resize failed; using GDI\n");
            g_ok = 0;
            return -1;
        }
    }
    if ((w != g_tex_w || h != g_tex_h) && make_texture(w, h) != 0) { g_ok = 0; return -1; }

    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return 0;
    for (int y = 0; y < h; y++) memcpy((uint8_t *)m.pData + (size_t)y * m.RowPitch, px + (size_t)y * w, (size_t)w * 4);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_tex, 0);

    const float black[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_rtv, black);
    int x, y, vw, vh;
    present_fit(g_buf_w, g_buf_h, w, h, &x, &y, &vw, &vh);
    D3D11_VIEWPORT vp = { (float)x, (float)y, (float)vw, (float)vh, 0.0f, 1.0f };
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(g_ctx, g_rs);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &g_srv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_smp);
    ID3D11DeviceContext_Draw(g_ctx, 3, 0);
    /* No vsync wait here: the scheduler already paces frames to the vblank. */
    HRESULT hr = IDXGISwapChain_Present(g_sc, 0, 0);
    if (FAILED(hr) && hr != DXGI_STATUS_OCCLUDED) {
        fprintf(stderr, "present: Present failed (0x%08lX); using GDI\n", (unsigned long)hr);
        g_ok = 0;
        return -1;
    }
    return 0;
}
#endif
