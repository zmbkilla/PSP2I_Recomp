/* Direct3D 11 backend for the GE.
 *
 * The runtime (psprecomp/src/hle/gpu.c) does all vertex work and hands each
 * draw over as a screen-space triangle list plus decoded state; this file
 * rasterizes it on the GPU. The software rasterizer remains the reference
 * (`--renderer software`).
 *
 * ## Keeping VRAM coherent
 *
 * PSP games render into VRAM and then *read it back*: sampling an offscreen
 * buffer as a texture (glow and blur passes), displaying it, copying it with
 * block transfers, occasionally reading it from the CPU. So each colour buffer
 * the game draws to becomes a GPU render target keyed by (VRAM offset, stride,
 * format), seeded from emulated VRAM, and the two copies are reconciled:
 *
 *   - GPU -> VRAM ("readback") only for the rows drawn since the last sync, and
 *     only when something is about to read VRAM: the display (each flip), a
 *     block transfer, a texture that cannot be sampled from the GPU copy, or a
 *     software fallback draw. Reading back only drawn rows matters: the two
 *     framebuffers of a double-buffered game sit within 512 rows of each other.
 *   - VRAM -> GPU ("upload") when emulated VRAM changed behind the GPU's back:
 *     checked by hash on the first draw into a target each frame, and forced
 *     when the runtime reports a write (vram_written).
 *
 * ## Approximations (counted, see d3d11_report)
 *
 * Colour math runs in float rather than the GE's 8-bit integer math, so pixels
 * can differ by about one step. Blend factors with doubled alpha, an "absolute
 * difference" equation, two unrelated fixed colours, and partial per-bit write
 * masks have no D3D11 equivalent and are approximated. Depth buffers are not
 * read back to VRAM.
 */
#ifdef _WIN32

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psprecomp/hle.h>
#include <psprecomp/mem.h>
#include <psprecomp/render.h>

#define VRAM_SIZE 0x200000u
#define MAX_RT    32
#define MAX_DEPTH 8
#define MAX_TEX   512
#define VB_BYTES  (16u << 20)

static ID3D11Device        *g_dev;
static ID3D11DeviceContext *g_ctx;
static ID3D11VertexShader  *g_vs;
static ID3D11PixelShader   *g_ps;
static ID3D11InputLayout   *g_layout;
static ID3D11Buffer        *g_vb, *g_vcb, *g_pcb;
static ID3D11RasterizerState *g_rs;
static uint32_t g_vb_pos;

typedef struct {
    int used;
    uint32_t off, stride, w, h;          /* VRAM offset, pixels per row, texture size */
    int fmt;
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    ID3D11Texture2D *staging;
    int dirty;                           /* GPU has rows newer than VRAM */
    uint32_t dy0, dy1;                   /* rows drawn since the last sync */
    uint32_t rows;                       /* rows in use (seeded/compared) */
    uint64_t synced_hash;
    uint64_t check_frame;
    uint64_t last_use;
} rt_entry;

typedef struct {
    int used;
    uint32_t off, w, h;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
    uint64_t last_use;
} depth_entry;

typedef struct {
    int used;
    uint64_t key, hash, check_frame, last_use;
    uint32_t w, h;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} tex_entry;

static rt_entry    g_rt[MAX_RT];
static depth_entry g_depth[MAX_DEPTH];
static tex_entry   g_tex[MAX_TEX];
static ID3D11Texture2D *g_scratch;       /* copy of a target sampled while drawn to */
static ID3D11ShaderResourceView *g_scratch_srv;
static uint32_t g_scratch_w, g_scratch_h;
static uint64_t g_use;

static uint64_t n_draws, n_tris, n_uploads, n_readbacks, n_tex_decodes, n_rt_tex, n_approx;

/* ---- helpers ------------------------------------------------------------------- */

static uint8_t *vram(void) { return (uint8_t *)psp_mem_ptr(PSP_VRAM_BASE, VRAM_SIZE); }
static uint64_t frame_no(void) { return psp_display_flips(); }

static uint32_t vram_off(uint32_t addr) { return addr & 0x1FFFFFu; }

static uint64_t hash_bytes(const uint8_t *p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static uint32_t expand16(int fmt, uint16_t p) {
    uint32_t r, g, b, a;
    switch (fmt) {
    case 0: r = p & 0x1F; g = (p >> 5) & 0x3F; b = (p >> 11) & 0x1F;
            return 0xFF000000u | (((b << 3) | (b >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((r << 3) | (r >> 2));
    case 1: r = p & 0x1F; g = (p >> 5) & 0x1F; b = (p >> 10) & 0x1F;
            return ((p & 0x8000) ? 0xFF000000u : 0) | (((b << 3) | (b >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((r << 3) | (r >> 2));
    default: r = p & 0xF; g = (p >> 4) & 0xF; b = (p >> 8) & 0xF; a = (p >> 12) & 0xF;
            return ((a * 17) << 24) | ((b * 17) << 16) | ((g * 17) << 8) | (r * 17);
    }
}

static uint16_t pack16(int fmt, uint32_t c) {
    uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
    switch (fmt) {
    case 0:  return (uint16_t)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
    case 1:  return (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15));
    default: return (uint16_t)((r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12));
    }
}

static int bpp_of(int fmt) { return fmt == 3 ? 4 : 2; }

/* ---- shaders ------------------------------------------------------------------- */

static const char *SHADER_SRC =
"cbuffer V : register(b0) { float4 vp; };\n"
"struct VI { float4 p : POSITION; float2 uv : TEXCOORD0; float4 c : COLOR0; };\n"
"struct VO { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 c : COLOR0; };\n"
"VO vsmain(VI i) {\n"
"  VO o; float w = 1.0 / i.p.w;\n"
"  float nx = i.p.x / vp.x * 2.0 - 1.0;\n"
"  float ny = 1.0 - i.p.y / vp.y * 2.0;\n"
"  float nz = saturate(i.p.z / 65535.0);\n"
"  o.p = float4(nx * w, ny * w, nz * w, w);\n"
"  o.uv = i.uv * vp.zw; o.c = i.c / 255.0; return o;\n"
"}\n"
"cbuffer P : register(b1) { float4 env; uint4 tf; uint4 at; uint4 misc; };\n"
"Texture2D T : register(t0); SamplerState S : register(s0);\n"
"float4 psmain(VO i) : SV_Target {\n"
"  float4 c = i.c;\n"
"  if (misc.x != 0) return c;\n"                       /* clear mode */
"  if (tf.x != 0) {\n"
"    float4 t = T.Sample(S, i.uv);\n"
"    float3 rgb; float a = c.a;\n"
"    uint f = tf.y;\n"
"    if (f == 0) { rgb = c.rgb * t.rgb; if (tf.z) a = c.a * t.a; }\n"
"    else if (f == 1) { rgb = tf.z ? lerp(c.rgb, t.rgb, t.a) : t.rgb; }\n"
"    else if (f == 2) { rgb = lerp(c.rgb, env.rgb, t.rgb); if (tf.z) a = c.a * t.a; }\n"
"    else if (f == 3) { rgb = t.rgb; if (tf.z) a = t.a; }\n"
"    else { rgb = c.rgb + t.rgb; if (tf.z) a = c.a * t.a; }\n"
"    if (tf.w) rgb *= 2.0;\n"
"    c = float4(saturate(rgb), a);\n"
"  }\n"
"  if (at.x != 0) {\n"
"    uint ai = ((uint)round(c.a * 255.0)) & at.w; uint r = at.z & at.w; bool ok;\n"
"    uint fn = at.y;\n"
"    if (fn == 0) ok = false; else if (fn == 1) ok = true;\n"
"    else if (fn == 2) ok = ai == r; else if (fn == 3) ok = ai != r;\n"
"    else if (fn == 4) ok = ai < r; else if (fn == 5) ok = ai <= r;\n"
"    else if (fn == 6) ok = ai > r; else ok = ai >= r;\n"
"    if (!ok) discard;\n"
"  }\n"
"  return c;\n"
"}\n";

typedef struct { float vp[4]; } vs_consts;
typedef struct { float env[4]; uint32_t tf[4], at[4], misc[4]; } ps_consts;

static ID3DBlob *compile(const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(SHADER_SRC, strlen(SHADER_SRC), "ge", NULL, NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "d3d11: shader %s failed: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

/* ---- render targets ---------------------------------------------------------------- */

static void rt_free(rt_entry *t) {
    if (t->srv) ID3D11ShaderResourceView_Release(t->srv);
    if (t->rtv) ID3D11RenderTargetView_Release(t->rtv);
    if (t->tex) ID3D11Texture2D_Release(t->tex);
    if (t->staging) ID3D11Texture2D_Release(t->staging);
    memset(t, 0, sizeof *t);
}

/* Copy rows [y0, y1) of the target into emulated VRAM. */
static void rt_readback(rt_entry *t) {
    if (!t->dirty) return;
    uint8_t *v = vram();
    uint32_t y0 = t->dy0, y1 = t->dy1 > t->h ? t->h : t->dy1;
    if (!v || y0 >= y1) { t->dirty = 0; return; }
    if (!t->staging) {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc(t->tex, &d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &t->staging))) return;
    }
    D3D11_BOX box = { 0, y0, 0, t->w, y1, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)t->staging, 0, 0, y0, 0,
                                              (ID3D11Resource *)t->tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)t->staging, 0, D3D11_MAP_READ, 0, &m))) return;
    const int bpp = bpp_of(t->fmt);
    for (uint32_t y = y0; y < y1; y++) {
        uint32_t o = t->off + y * t->stride * (uint32_t)bpp;
        if (o + t->stride * (uint32_t)bpp > VRAM_SIZE) break;
        const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
        if (bpp == 4) memcpy(v + o, src, t->stride * 4u);
        else for (uint32_t x = 0; x < t->stride; x++) {
            uint16_t p = pack16(t->fmt, src[x]);
            memcpy(v + o + x * 2, &p, 2);
        }
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)t->staging, 0);
    t->dirty = 0;
    t->dy0 = t->dy1 = 0;
    uint32_t bytes = t->rows * t->stride * (uint32_t)bpp;
    if (t->off + bytes > VRAM_SIZE) bytes = VRAM_SIZE - t->off;
    t->synced_hash = hash_bytes(v + t->off, bytes);
    n_readbacks++;
}

/* Seed rows [0, rows) of the target from emulated VRAM. */
static void rt_upload(rt_entry *t) {
    uint8_t *v = vram();
    if (!v) return;
    static int trace = -1;
    if (trace < 0) trace = getenv("PSP2I_RT_TRACE") != NULL;
    if (trace) fprintf(stderr, "d3d11: upload VRAM+0x%06X stride %u fmt %d rows %u (flip %llu, %s)\n",
                       t->off, t->stride, t->fmt, t->rows, (unsigned long long)frame_no(),
                       t->dirty ? "DIRTY" : "clean");
    const int bpp = bpp_of(t->fmt);
    uint32_t *px = (uint32_t *)malloc((size_t)t->stride * t->rows * 4);
    if (!px) return;
    for (uint32_t y = 0; y < t->rows; y++) {
        uint32_t o = t->off + y * t->stride * (uint32_t)bpp;
        for (uint32_t x = 0; x < t->stride; x++) {
            uint32_t c = 0;
            if (o + (x + 1) * (uint32_t)bpp <= VRAM_SIZE) {
                if (bpp == 4) memcpy(&c, v + o + x * 4, 4);
                else { uint16_t p; memcpy(&p, v + o + x * 2, 2); c = expand16(t->fmt, p); }
            }
            px[y * t->stride + x] = c;
        }
    }
    D3D11_BOX box = { 0, 0, 0, t->stride, t->rows, 1 };
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)t->tex, 0, &box, px, t->stride * 4, 0);
    free(px);
    uint32_t bytes = t->rows * t->stride * (uint32_t)bpp;
    if (t->off + bytes > VRAM_SIZE) bytes = VRAM_SIZE - t->off;
    t->synced_hash = hash_bytes(v + t->off, bytes);
    n_uploads++;
}

static rt_entry *rt_find(uint32_t off) {
    for (int i = 0; i < MAX_RT; i++) if (g_rt[i].used && g_rt[i].off == off) return &g_rt[i];
    return NULL;
}

static rt_entry *rt_get(uint32_t off, uint32_t stride, int fmt, uint32_t rows) {
    rt_entry *t = rt_find(off);
    if (t && (t->stride != stride || t->fmt != fmt)) { rt_readback(t); rt_free(t); t = NULL; }
    if (!t) {
        for (int i = 0; i < MAX_RT && !t; i++) if (!g_rt[i].used) t = &g_rt[i];
        if (!t) {                                       /* evict the least recently used */
            t = &g_rt[0];
            for (int i = 1; i < MAX_RT; i++) if (g_rt[i].last_use < t->last_use) t = &g_rt[i];
            rt_readback(t);
            rt_free(t);
        }
        t->off = off; t->stride = stride; t->fmt = fmt;
        t->w = stride; t->h = 512;
        D3D11_TEXTURE2D_DESC d;
        memset(&d, 0, sizeof d);
        d.Width = t->w; d.Height = t->h; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &t->tex))) { memset(t, 0, sizeof *t); return NULL; }
        ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)t->tex, NULL, &t->rtv);
        ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t->tex, NULL, &t->srv);
        t->used = 1;
        t->rows = rows;
        if (getenv("PSP2I_RT_TRACE"))
            fprintf(stderr, "d3d11: render target VRAM+0x%06X stride %u format %d rows %u (flip %llu)\n",
                    off, stride, fmt, rows, (unsigned long long)frame_no());
        rt_upload(t);
        t->check_frame = frame_no();
    }
    if (rows > t->rows) {                               /* drawing further down than seeded */
        if (t->dirty) rt_readback(t);
        t->rows = rows;
        rt_upload(t);
    }
    t->last_use = ++g_use;
    return t;
}

/* On the first draw of a frame, pick up CPU writes to VRAM. */
static void rt_check(rt_entry *t) {
    uint64_t f = frame_no();
    if (t->check_frame == f || t->dirty) return;
    t->check_frame = f;
    uint8_t *v = vram();
    uint32_t bytes = t->rows * t->stride * (uint32_t)bpp_of(t->fmt);
    if (t->off + bytes > VRAM_SIZE) bytes = VRAM_SIZE - t->off;
    if (v && hash_bytes(v + t->off, bytes) != t->synced_hash) rt_upload(t);
}

static int overlaps(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) { return a0 < b1 && b0 < a1; }
static uint32_t rt_end(const rt_entry *t) { return t->off + t->rows * t->stride * (uint32_t)bpp_of(t->fmt); }

/* ---- depth buffers ------------------------------------------------------------------ */

static depth_entry *depth_get(uint32_t off, uint32_t w, uint32_t h) {
    depth_entry *d = NULL;
    for (int i = 0; i < MAX_DEPTH; i++)
        if (g_depth[i].used && g_depth[i].off == off && g_depth[i].w == w && g_depth[i].h == h) d = &g_depth[i];
    if (!d) {
        for (int i = 0; i < MAX_DEPTH && !d; i++) if (!g_depth[i].used) d = &g_depth[i];
        if (!d) {
            d = &g_depth[0];
            for (int i = 1; i < MAX_DEPTH; i++) if (g_depth[i].last_use < d->last_use) d = &g_depth[i];
            if (d->dsv) ID3D11DepthStencilView_Release(d->dsv);
            if (d->tex) ID3D11Texture2D_Release(d->tex);
            memset(d, 0, sizeof *d);
        }
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_D16_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &d->tex))) return NULL;
        ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)d->tex, NULL, &d->dsv);
        ID3D11DeviceContext_ClearDepthStencilView(g_ctx, d->dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
        d->off = off; d->w = w; d->h = h; d->used = 1;
    }
    d->last_use = ++g_use;
    return d;
}

/* ---- textures ------------------------------------------------------------------------ */

static tex_entry *tex_get(const psp_gpu_state *st) {
    tex_entry *e = NULL;
    for (int i = 0; i < MAX_TEX; i++) if (g_tex[i].used && g_tex[i].key == st->tex_key) { e = &g_tex[i]; break; }
    uint64_t f = frame_no();
    if (e && e->check_frame == f) { e->last_use = ++g_use; return e; }
    uint64_t h = psp_gpu_texture_hash();
    if (e && e->hash == h) { e->check_frame = f; e->last_use = ++g_use; return e; }
    if (!e) {
        for (int i = 0; i < MAX_TEX && !e; i++) if (!g_tex[i].used) e = &g_tex[i];
        if (!e) {
            e = &g_tex[0];
            for (int i = 1; i < MAX_TEX; i++) if (g_tex[i].last_use < e->last_use) e = &g_tex[i];
        }
        if (e->used && (e->w != st->tex_w || e->h != st->tex_h)) {
            ID3D11ShaderResourceView_Release(e->srv); ID3D11Texture2D_Release(e->tex);
            e->srv = NULL; e->tex = NULL;
        }
        if (!e->tex) {
            D3D11_TEXTURE2D_DESC d;
            memset(&d, 0, sizeof d);
            d.Width = st->tex_w; d.Height = st->tex_h; d.MipLevels = 1; d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &e->tex))) return NULL;
            ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)e->tex, NULL, &e->srv);
        }
        e->key = st->tex_key; e->w = st->tex_w; e->h = st->tex_h; e->used = 1;
    }
    uint32_t *px = (uint32_t *)malloc((size_t)st->tex_w * st->tex_h * 4);
    if (!px) return NULL;
    psp_gpu_decode_texture(px);
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)e->tex, 0, NULL, px, st->tex_w * 4, 0);
    free(px);
    e->hash = h; e->check_frame = f; e->last_use = ++g_use;
    n_tex_decodes++;
    return e;
}

/* ---- state objects (created on demand, cached by description) ------------------------ */

typedef struct { uint64_t key; void *obj; } state_slot;
static state_slot g_blend[256], g_dss[64], g_samp[16];

static void *cache_find(state_slot *c, int n, uint64_t key) {
    for (int i = 0; i < n; i++) if (c[i].obj && c[i].key == key) return c[i].obj;
    return NULL;
}
static void cache_put(state_slot *c, int n, uint64_t key, void *obj) {
    for (int i = 0; i < n; i++) if (!c[i].obj) { c[i].key = key; c[i].obj = obj; return; }
    c[n - 1].key = key; c[n - 1].obj = obj;          /* full: replace the last (leaks one object) */
}

/* A PSP blend factor as a D3D11 one. `fix` is that side's fixed colour. */
static D3D11_BLEND map_factor(int f, int is_src, uint32_t fix, uint32_t *blend_factor, int *approx) {
    switch (f) {
    case 0:  return is_src ? D3D11_BLEND_DEST_COLOR : D3D11_BLEND_SRC_COLOR;
    case 1:  return is_src ? D3D11_BLEND_INV_DEST_COLOR : D3D11_BLEND_INV_SRC_COLOR;
    case 2:  return D3D11_BLEND_SRC_ALPHA;
    case 3:  return D3D11_BLEND_INV_SRC_ALPHA;
    case 4:  return D3D11_BLEND_DEST_ALPHA;
    case 5:  return D3D11_BLEND_INV_DEST_ALPHA;
    case 6:  *approx = 1; return D3D11_BLEND_SRC_ALPHA;       /* 2 x src alpha */
    case 7:  *approx = 1; return D3D11_BLEND_INV_SRC_ALPHA;
    case 8:  *approx = 1; return D3D11_BLEND_DEST_ALPHA;
    case 9:  *approx = 1; return D3D11_BLEND_INV_DEST_ALPHA;
    default:
        if (fix == 0xFFFFFF) return D3D11_BLEND_ONE;
        if (fix == 0) return D3D11_BLEND_ZERO;
        if (*blend_factor == 0xFFFFFFFFu || *blend_factor == fix) { *blend_factor = fix; return D3D11_BLEND_BLEND_FACTOR; }
        if (*blend_factor == (~fix & 0xFFFFFF)) return D3D11_BLEND_INV_BLEND_FACTOR;
        *approx = 1;
        return D3D11_BLEND_BLEND_FACTOR;
    }
}

static ID3D11BlendState *blend_state(const psp_gpu_state *st, float factor[4], int *approx) {
    D3D11_BLEND_DESC d;
    memset(&d, 0, sizeof d);
    D3D11_RENDER_TARGET_BLEND_DESC *b = &d.RenderTarget[0];
    uint8_t wm = 0;
    if (st->clear) {
        if (st->clear_which & 1) wm |= D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        if (st->clear_which & 2) wm |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    } else {
        for (int ch = 0; ch < 3; ch++) {
            uint32_t m = (st->mask_rgb >> (ch * 8)) & 0xFF;
            if (m != 0xFF) wm |= (uint8_t)(1u << ch);
            if (m != 0 && m != 0xFF) *approx = 1;
        }
        if (st->mask_a != 0xFF) wm |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
        if (st->mask_a != 0 && st->mask_a != 0xFF) *approx = 1;
    }
    if (st->fb_fmt == 0) wm &= (uint8_t)~D3D11_COLOR_WRITE_ENABLE_ALPHA;   /* 565 has no alpha */
    b->RenderTargetWriteMask = wm;
    uint32_t bf = 0xFFFFFFFFu;
    if (st->blend && !st->clear) {
        b->BlendEnable = TRUE;
        b->SrcBlend = map_factor(st->bsrc, 1, st->fixa, &bf, approx);
        b->DestBlend = map_factor(st->bdst, 0, st->fixb, &bf, approx);
        static const D3D11_BLEND_OP OPS[6] = {
            D3D11_BLEND_OP_ADD, D3D11_BLEND_OP_SUBTRACT, D3D11_BLEND_OP_REV_SUBTRACT,
            D3D11_BLEND_OP_MIN, D3D11_BLEND_OP_MAX, D3D11_BLEND_OP_MAX };
        if (st->beq >= 5) *approx = 1;
        b->BlendOp = OPS[st->beq < 6 ? st->beq : 0];
        b->SrcBlendAlpha = D3D11_BLEND_ONE;               /* the result keeps the source alpha */
        b->DestBlendAlpha = D3D11_BLEND_ZERO;
        b->BlendOpAlpha = D3D11_BLEND_OP_ADD;
    }
    if (bf == 0xFFFFFFFFu) bf = 0;
    factor[0] = (float)(bf & 0xFF) / 255.0f;
    factor[1] = (float)((bf >> 8) & 0xFF) / 255.0f;
    factor[2] = (float)((bf >> 16) & 0xFF) / 255.0f;
    factor[3] = 1.0f;
    uint64_t key = (uint64_t)wm | (uint64_t)b->BlendEnable << 8 | (uint64_t)b->SrcBlend << 9 |
                   (uint64_t)b->DestBlend << 14 | (uint64_t)b->BlendOp << 19;
    ID3D11BlendState *s = (ID3D11BlendState *)cache_find(g_blend, 256, key);
    if (!s && SUCCEEDED(ID3D11Device_CreateBlendState(g_dev, &d, &s))) cache_put(g_blend, 256, key, s);
    return s;
}

static ID3D11DepthStencilState *depth_state(const psp_gpu_state *st) {
    static const D3D11_COMPARISON_FUNC F[8] = {
        D3D11_COMPARISON_NEVER, D3D11_COMPARISON_ALWAYS, D3D11_COMPARISON_EQUAL, D3D11_COMPARISON_NOT_EQUAL,
        D3D11_COMPARISON_LESS, D3D11_COMPARISON_LESS_EQUAL, D3D11_COMPARISON_GREATER, D3D11_COMPARISON_GREATER_EQUAL };
    D3D11_DEPTH_STENCIL_DESC d;
    memset(&d, 0, sizeof d);
    if (st->clear) {
        d.DepthEnable = (st->clear_which & 4) ? TRUE : FALSE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        d.DepthFunc = D3D11_COMPARISON_ALWAYS;
    } else {
        d.DepthEnable = st->ztest ? TRUE : FALSE;
        d.DepthWriteMask = st->zwrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = F[st->zfunc & 7];
    }
    uint64_t key = (uint64_t)d.DepthEnable | (uint64_t)d.DepthWriteMask << 1 | (uint64_t)d.DepthFunc << 2;
    ID3D11DepthStencilState *s = (ID3D11DepthStencilState *)cache_find(g_dss, 64, key);
    if (!s && SUCCEEDED(ID3D11Device_CreateDepthStencilState(g_dev, &d, &s))) cache_put(g_dss, 64, key, s);
    return s;
}

static ID3D11SamplerState *sampler(int linear, int cu, int cv) {
    uint64_t key = (uint64_t)linear | (uint64_t)cu << 1 | (uint64_t)cv << 2;
    ID3D11SamplerState *s = (ID3D11SamplerState *)cache_find(g_samp, 16, key);
    if (s) return s;
    D3D11_SAMPLER_DESC d;
    memset(&d, 0, sizeof d);
    d.Filter = linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    d.AddressU = cu ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
    d.AddressV = cv ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
    d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(ID3D11Device_CreateSamplerState(g_dev, &d, &s))) cache_put(g_samp, 16, key, s);
    return s;
}

/* ---- the draw ------------------------------------------------------------------------- */

/* The view to sample for this draw's texture, and the texel-to-UV scale. */
static ID3D11ShaderResourceView *texture_view(const psp_gpu_state *st, rt_entry *target, float scale[2]) {
    const uint32_t toff = vram_off(st->tex_addr);
    const int in_vram = (st->tex_addr & 0x0F000000u) == 0x04000000u;
    if (in_vram) {
        static const int tbpp[11] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8 };
        uint32_t tend = toff + st->tex_bufw * st->tex_h * (uint32_t)tbpp[st->tex_psm > 10 ? 3 : st->tex_psm] / 8;
        for (int i = 0; i < MAX_RT; i++) {
            rt_entry *t = &g_rt[i];
            if (!t->used || !overlaps(toff, tend, t->off, rt_end(t))) continue;
            const int same_fmt = st->tex_psm <= 3 && st->tex_psm == t->fmt && !st->tex_swz;
            if (t->dirty && t->off == toff && st->tex_bufw == t->stride && same_fmt) {
                /* Render-to-texture: sample the GPU copy directly. */
                n_rt_tex++;
                scale[0] = 1.0f / (float)t->w;
                scale[1] = 1.0f / (float)t->h;
                if (t != target) return t->srv;
                /* Sampled while being drawn to: sample a copy. */
                if (!g_scratch || g_scratch_w != t->w || g_scratch_h != t->h) {
                    if (g_scratch_srv) ID3D11ShaderResourceView_Release(g_scratch_srv);
                    if (g_scratch) ID3D11Texture2D_Release(g_scratch);
                    D3D11_TEXTURE2D_DESC d;
                    ID3D11Texture2D_GetDesc(t->tex, &d);
                    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &g_scratch);
                    ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_scratch, NULL, &g_scratch_srv);
                    g_scratch_w = t->w; g_scratch_h = t->h;
                }
                ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_scratch, (ID3D11Resource *)t->tex);
                return g_scratch_srv;
            }
            /* Read in another shape: make VRAM current and decode normally. */
            rt_readback(t);
        }
    }
    tex_entry *e = tex_get(st);
    scale[0] = 1.0f / (float)st->tex_w;
    scale[1] = 1.0f / (float)st->tex_h;
    return e ? e->srv : NULL;
}

static int d3d_draw(const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts) {
    if ((st->fb_addr & 0x0F000000u) != 0x04000000u || !st->fb_stride || nverts <= 0) return 1;
    const uint32_t off = vram_off(st->fb_addr);
    uint32_t rows = (uint32_t)st->sy1 + 1;
    if (rows > 512) rows = 512;
    rt_entry *t = rt_get(off, st->fb_stride, st->fb_fmt, rows);
    if (!t) return 1;
    /* Targets that share VRAM with this one at a different start or layout
     * (PSP2i blurs its bloom in two 256-wide buffers that sit inside the
     * 512-wide display buffer, which the final copy then overwrites). VRAM
     * holds one truth, so at most one of an overlapping set may be ahead of
     * it: flush any other dirty one before drawing here, take its bytes, and
     * make the others re-read VRAM before their next draw. Without this a
     * stale blur buffer was read back over the finished frame at display
     * time, as a striped band. */
    {
        const uint32_t t0 = t->off, t1 = rt_end(t);
        int flushed = 0;
        for (int i = 0; i < MAX_RT; i++) {
            rt_entry *o = &g_rt[i];
            if (!o->used || o == t || !overlaps(t0, t1, o->off, rt_end(o))) continue;
            if (o->dirty) { rt_readback(o); flushed = 1; }
            o->check_frame = (uint64_t)-1;
        }
        if (flushed && !t->dirty) t->check_frame = (uint64_t)-1;
    }
    rt_check(t);

    ID3D11DepthStencilView *dsv = NULL;
    if (st->zb_stride && (st->ztest || (st->clear && (st->clear_which & 4)))) {
        depth_entry *d = depth_get(vram_off(st->zb_addr), t->w, t->h);
        if (d) dsv = d->dsv;
    }

    ID3D11ShaderResourceView *srv = NULL;
    float uvs[2] = { 1, 1 };
    if (st->tex) {
        srv = texture_view(st, t, uvs);
        if (!srv) return 1;
    }

    /* Vertices into the ring buffer. */
    UINT bytes = (UINT)nverts * (UINT)sizeof(psp_gpu_vertex);
    if (bytes > VB_BYTES) return 1;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (g_vb_pos + bytes > VB_BYTES) { g_vb_pos = 0; mode = D3D11_MAP_WRITE_DISCARD; }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_vb, 0, mode, 0, &m))) return 1;
    memcpy((uint8_t *)m.pData + g_vb_pos, v, bytes);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_vb, 0);
    UINT stride = sizeof(psp_gpu_vertex), vboff = g_vb_pos;
    g_vb_pos += bytes;

    vs_consts vc = { { (float)t->w, (float)t->h, uvs[0], uvs[1] } };
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_vcb, 0, NULL, &vc, 0, 0);
    ps_consts pc;
    memset(&pc, 0, sizeof pc);
    pc.env[0] = (float)(st->env & 0xFF) / 255.0f;
    pc.env[1] = (float)((st->env >> 8) & 0xFF) / 255.0f;
    pc.env[2] = (float)((st->env >> 16) & 0xFF) / 255.0f;
    pc.tf[0] = (uint32_t)(st->tex != 0); pc.tf[1] = (uint32_t)st->tfunc;
    pc.tf[2] = (uint32_t)st->trgba; pc.tf[3] = (uint32_t)st->tdbl;
    pc.at[0] = (uint32_t)(st->atest && !st->clear); pc.at[1] = (uint32_t)st->afunc;
    pc.at[2] = st->aref; pc.at[3] = st->amask;
    pc.misc[0] = (uint32_t)st->clear;
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_pcb, 0, NULL, &pc, 0, 0);

    int approx = 0;
    float bf[4];
    ID3D11BlendState *bs = blend_state(st, bf, &approx);
    if (approx) n_approx++;

    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &t->rtv, dsv);
    D3D11_VIEWPORT vp = { 0, 0, (float)t->w, (float)t->h, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    D3D11_RECT sc = { st->sx0, st->sy0, st->sx1 + 1, st->sy1 + 1 };
    ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &sc);
    ID3D11DeviceContext_RSSetState(g_ctx, g_rs);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, bs, bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, depth_state(st), 0);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, g_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &g_vb, &stride, &vboff);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &g_vcb);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 1, 1, &g_pcb);
    if (srv) {
        ID3D11SamplerState *ss = sampler(st->tex_linear, st->tex_clamp_u, st->tex_clamp_v);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &srv);
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &ss);
    }
    ID3D11DeviceContext_Draw(g_ctx, (UINT)nverts, 0);
    if (srv) {                                          /* unbind, so the view can become a target */
        ID3D11ShaderResourceView *none = NULL;
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &none);
    }

    /* Rows touched: the scissor, narrowed to the geometry's extent. */
    float ymin = 1e9f, ymax = -1e9f;
    for (int i = 0; i < nverts; i++) { if (v[i].y < ymin) ymin = v[i].y; if (v[i].y > ymax) ymax = v[i].y; }
    int y0 = (int)ymin - 1, y1 = (int)ymax + 2;
    if (y0 < st->sy0) y0 = st->sy0;
    if (y1 > st->sy1 + 1) y1 = st->sy1 + 1;
    if (y0 < y1) {
        if (!t->dirty) { t->dy0 = (uint32_t)y0; t->dy1 = (uint32_t)y1; }
        else { if ((uint32_t)y0 < t->dy0) t->dy0 = (uint32_t)y0; if ((uint32_t)y1 > t->dy1) t->dy1 = (uint32_t)y1; }
        t->dirty = 1;
    }
    n_draws++;
    n_tris += (uint64_t)nverts / 3;
    return 0;
}

static void d3d_sync_vram(uint32_t addr, uint32_t bytes) {
    if ((addr & 0x0F000000u) != 0x04000000u) return;
    uint32_t a0 = vram_off(addr), a1 = a0 + bytes;
    for (int i = 0; i < MAX_RT; i++)
        if (g_rt[i].used && g_rt[i].dirty && overlaps(a0, a1, g_rt[i].off, rt_end(&g_rt[i])))
            rt_readback(&g_rt[i]);
}

static void d3d_vram_written(uint32_t addr, uint32_t bytes) {
    if ((addr & 0x0F000000u) != 0x04000000u) return;
    uint32_t a0 = vram_off(addr), a1 = a0 + bytes;
    for (int i = 0; i < MAX_RT; i++)
        if (g_rt[i].used && overlaps(a0, a1, g_rt[i].off, rt_end(&g_rt[i])))
            g_rt[i].check_frame = (uint64_t)-1;          /* re-hash on the next draw */
}

static const psp_gpu_backend D3D11_BACKEND = { "d3d11", d3d_draw, d3d_sync_vram, d3d_vram_written };

/* ---- setup ------------------------------------------------------------------------------ */

int d3d11_init(void) {
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                                   &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) { fprintf(stderr, "d3d11: no hardware device (0x%08lX)\n", (unsigned long)hr); return -1; }

    ID3DBlob *vsb = compile("vsmain", "vs_4_0"), *psb = compile("psmain", "ps_4_0");
    if (!vsb || !psb) return -1;
    ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &g_vs);
    ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &g_ps);
    D3D11_INPUT_ELEMENT_DESC il[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11Device_CreateInputLayout(g_dev, il, 3, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &g_layout);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);

    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = VB_BYTES; bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_vb);
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(vs_consts); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_vcb);
    bd.ByteWidth = sizeof(ps_consts);
    ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_pcb);

    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;  /* culling is done before submission */
    rd.ScissorEnable = TRUE; rd.DepthClipEnable = FALSE;
    ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_rs);

    if (!g_vs || !g_ps || !g_layout || !g_vb || !g_vcb || !g_pcb || !g_rs) {
        fprintf(stderr, "d3d11: device object creation failed\n");
        return -1;
    }
    psp_gpu_set_backend(&D3D11_BACKEND);
    fprintf(stderr, "d3d11: rendering on the GPU (feature level %x.%x)\n", (fl >> 12) & 0xF, (fl >> 8) & 0xF);
    return 0;
}

void d3d11_report(FILE *out) {
    if (!g_dev) return;
    fprintf(out, "  d3d11               %llu draws, %llu triangles; %llu VRAM uploads, %llu readbacks; "
                 "%llu texture decodes, %llu render-to-texture samples; %llu draws approximated\n",
            (unsigned long long)n_draws, (unsigned long long)n_tris, (unsigned long long)n_uploads,
            (unsigned long long)n_readbacks, (unsigned long long)n_tex_decodes,
            (unsigned long long)n_rt_tex, (unsigned long long)n_approx);
}

#endif
