/* OpenGL 3.3 core backend for the GE -- the portable counterpart of d3d11.c.
 *
 * Same design, same invariants, same names: the runtime (psprecomp/src/hle/
 * gpu.c) does the vertex work and hands each draw over as a screen-space
 * triangle list plus decoded state; this file rasterizes it on the GPU and
 * keeps emulated VRAM coherent with the render targets it holds (see the long
 * comments in d3d11.c, which apply here unchanged):
 *
 *   - each colour buffer the game draws into is a GL texture + framebuffer,
 *     keyed by (VRAM offset, stride, format), seeded from VRAM;
 *   - dirty rows are read back only when something is about to read VRAM, and
 *     a CPU access to VRAM reaches gl_cpu_access through psp_mem_set_vram_hook;
 *   - overlapping targets hand their newer rows to each other on the GPU
 *     (rt_xfer) instead of round-tripping through VRAM;
 *   - the displayed frame is copied asynchronously (pixel buffer + fence), so
 *     presenting never waits for the GPU.
 *
 * Rows: GL's images start at the bottom, so the convention here is that
 * texture row y IS PSP row y (row 0 first in memory, at window y = 0). The
 * vertex shader maps PSP y straight to NDC without the flip D3D needs, and
 * scissor, viewport, uploads and readbacks all use PSP rows unchanged.
 *
 * Nothing here is platform-specific: the platform makes a 3.3 core context
 * current and loads the entry points (gl_load), then calls gl_ge_init. */

#include "gl_load.h"
#include "gl_ge.h"

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
#define VB_VERTS  (400u * 1024u)                 /* ring of psp_gpu_vertex (16 MB) */

static int    g_ok;
static GLuint g_prog, g_xprog, g_vao, g_xvao, g_vb;
static GLint  u_vp, u_env, u_tf, u_at, u_misc, u_tex;
static GLint  u_xdst, u_xsrc, u_xrng, u_xtex;
static GLuint g_samp[8];
static uint32_t g_vb_pos;                        /* in vertices */

typedef struct {
    int used;
    uint32_t off, stride, w, h;
    int fmt;
    GLuint tex, fbo, depth_rb;                   /* depth_rb: the depth buffer attached now */
    int dirty;
    uint32_t dy0, dy1;
    uint64_t drow[8];
    uint32_t rows;
    uint64_t synced_hash;
    uint64_t check_frame;
    uint64_t last_use;
} rt_entry;

typedef struct {
    int used, cleared;
    uint32_t off, w, h;
    GLuint rb;
    uint64_t last_use;
} depth_entry;

typedef struct {
    int used;
    uint64_t key, hash, check_frame, last_use;
    uint32_t w, h;
    GLuint tex;
} tex_entry;

static rt_entry    g_rt[MAX_RT];
static depth_entry g_depth[MAX_DEPTH];
static tex_entry   g_tex[MAX_TEX];
static GLuint g_scratch, g_scratch_fbo;          /* copy of a target sampled while drawn to */
static uint32_t g_scratch_w, g_scratch_h;
static uint64_t g_use;

static uint64_t n_draws, n_tris, n_uploads, n_readbacks, n_tex_decodes, n_rt_tex, n_approx;

/* ---- helpers (as d3d11.c) ----------------------------------------------------------- */

static uint8_t *vram(void) { return psp_mem.vram; }
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
static int overlaps(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) { return a0 < b1 && b0 < a1; }
static uint32_t rt_end(const rt_entry *t) { return t->off + t->rows * t->stride * (uint32_t)bpp_of(t->fmt); }

/* ---- coherence on CPU access (as d3d11.c) ------------------------------------------- */

#define PG_SHIFT 12
#define NPG (VRAM_SIZE >> PG_SHIFT)
static uint8_t g_pg_dirty[NPG], g_pg_armed[NPG];

static void pages_set(uint8_t *map, uint32_t a0, uint32_t a1) {
    if (a1 > VRAM_SIZE) a1 = VRAM_SIZE;
    if (a0 >= a1) return;
    for (uint32_t p = a0 >> PG_SHIFT; p <= (a1 - 1) >> PG_SHIFT; p++) map[p] = 1;
}

static uint32_t row_addr(const rt_entry *t, uint32_t y) { return t->off + y * t->stride * (uint32_t)bpp_of(t->fmt); }
static int row_dirty(const rt_entry *t, uint32_t y) { return y < 512 && ((t->drow[y >> 6] >> (y & 63)) & 1); }

static void rows_bounds(rt_entry *t) {
    uint32_t lo = 512, hi = 0;
    for (uint32_t y = 0; y < 512; y++) if (row_dirty(t, y)) { if (y < lo) lo = y; hi = y + 1; }
    if (lo >= hi) { t->dirty = 0; t->dy0 = t->dy1 = 0; memset(t->drow, 0, sizeof t->drow); }
    else { t->dirty = 1; t->dy0 = lo; t->dy1 = hi; }
}

static void rows_mark(rt_entry *t, uint32_t y0, uint32_t y1) {
    if (y1 > 512) y1 = 512;
    if (y0 >= y1) return;
    for (uint32_t y = y0; y < y1; y++) t->drow[y >> 6] |= 1ull << (y & 63);
    if (!t->dirty) { t->dy0 = y0; t->dy1 = y1; }
    else { if (y0 < t->dy0) t->dy0 = y0; if (y1 > t->dy1) t->dy1 = y1; }
    t->dirty = 1;
    pages_set(g_pg_dirty, row_addr(t, y0), row_addr(t, y1));
}

static void rows_clear(rt_entry *t, uint32_t y0, uint32_t y1) {
    if (y1 > 512) y1 = 512;
    for (uint32_t y = y0; y < y1; y++) t->drow[y >> 6] &= ~(1ull << (y & 63));
    rows_bounds(t);
}

/* ---- shaders (the HLSL of d3d11.c in GLSL; the y axis is not flipped) ---------------- */

static const char *VS_SRC =
"#version 330 core\n"
"uniform vec4 vp;\n"
"in vec4 a_p; in vec2 a_uv; in vec4 a_c;\n"
"out vec2 v_uv; out vec4 v_c;\n"
"void main() {\n"
"  float w = 1.0 / a_p.w;\n"
"  float nx = a_p.x / vp.x * 2.0 - 1.0;\n"
"  float ny = a_p.y / vp.y * 2.0 - 1.0;\n"
"  float nz = clamp(a_p.z / 65535.0, 0.0, 1.0);\n"
"  gl_Position = vec4(nx * w, ny * w, (nz * 2.0 - 1.0) * w, w);\n"     /* window depth = nz */
"  v_uv = a_uv * vp.zw; v_c = a_c / 255.0;\n"
"}\n";

static const char *FS_SRC =
"#version 330 core\n"
"uniform vec4 env; uniform uvec4 tf; uniform uvec4 at; uniform uvec4 misc;\n"
"uniform sampler2D T;\n"
"in vec2 v_uv; in vec4 v_c; out vec4 o_c;\n"
"void main() {\n"
"  vec4 c = v_c;\n"
"  if (misc.x != 0u) { o_c = c; return; }\n"
"  if (tf.x != 0u) {\n"
"    vec4 t = texture(T, v_uv);\n"
"    vec3 rgb; float a = c.a;\n"
"    uint f = tf.y;\n"
"    if (f == 0u) { rgb = c.rgb * t.rgb; if (tf.z != 0u) a = c.a * t.a; }\n"
"    else if (f == 1u) { rgb = tf.z != 0u ? mix(c.rgb, t.rgb, t.a) : t.rgb; }\n"
"    else if (f == 2u) { rgb = mix(c.rgb, env.rgb, t.rgb); if (tf.z != 0u) a = c.a * t.a; }\n"
"    else if (f == 3u) { rgb = t.rgb; if (tf.z != 0u) a = t.a; }\n"
"    else { rgb = c.rgb + t.rgb; if (tf.z != 0u) a = c.a * t.a; }\n"
"    if (tf.w != 0u) rgb *= 2.0;\n"
"    c = vec4(clamp(rgb, 0.0, 1.0), a);\n"
"  }\n"
"  if (at.x != 0u) {\n"
"    uint ai = uint(round(c.a * 255.0)) & at.w; uint r = at.z & at.w; bool ok;\n"
"    uint fn = at.y;\n"
"    if (fn == 0u) ok = false; else if (fn == 1u) ok = true;\n"
"    else if (fn == 2u) ok = ai == r; else if (fn == 3u) ok = ai != r;\n"
"    else if (fn == 4u) ok = ai < r; else if (fn == 5u) ok = ai <= r;\n"
"    else if (fn == 6u) ok = ai > r; else ok = ai >= r;\n"
"    if (!ok) discard;\n"
"  }\n"
"  o_c = c;\n"
"}\n";

/* One target's bytes as another target's pixels (d3d11.c XFER_SRC). */
static const char *XVS_SRC =
"#version 330 core\n"
"void main() {\n"
"  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
"  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"}\n";

static const char *XFS_SRC =
"#version 330 core\n"
"uniform uvec4 dst; uniform uvec4 src; uniform uvec4 rng;\n"   /* off, stride, fmt, bpp; ...; lo, hi */
"uniform sampler2D Y;\n"
"out vec4 o_c;\n"
"uint ld(uint i) {\n"
"  uvec4 b = uvec4(round(clamp(texelFetch(Y, ivec2(int(i % src.y), int(i / src.y)), 0), 0.0, 1.0) * 255.0));\n"
"  return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24);\n"
"}\n"
"uint pack16(uint f, uint c) {\n"
"  uint r = c & 255u, g = (c >> 8) & 255u, b = (c >> 16) & 255u, a = c >> 24;\n"
"  if (f == 0u) return (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11);\n"
"  if (f == 1u) return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15);\n"
"  return (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12);\n"
"}\n"
"uint expand16(uint f, uint p) {\n"
"  uint r, g, b, a;\n"
"  if (f == 0u) { r = p & 31u; g = (p >> 5) & 63u; b = (p >> 11) & 31u;\n"
"    return 0xFF000000u | (((b << 3) | (b >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((r << 3) | (r >> 2)); }\n"
"  if (f == 1u) { r = p & 31u; g = (p >> 5) & 31u; b = (p >> 10) & 31u;\n"
"    return ((p & 0x8000u) != 0u ? 0xFF000000u : 0u) | (((b << 3) | (b >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((r << 3) | (r >> 2)); }\n"
"  r = p & 15u; g = (p >> 4) & 15u; b = (p >> 8) & 15u; a = (p >> 12) & 15u;\n"
"  return ((a * 17u) << 24) | ((b * 17u) << 16) | ((g * 17u) << 8) | (r * 17u);\n"
"}\n"
"vec4 bytes4(uint w) { return vec4(float(w & 255u), float((w >> 8) & 255u), float((w >> 16) & 255u), float(w >> 24)) / 255.0; }\n"
"void main() {\n"
"  uint x = uint(gl_FragCoord.x), y = uint(gl_FragCoord.y);\n"
"  uint A = dst.x + (y * dst.y + x) * dst.w;\n"
"  if (A < rng.x || A + dst.w > rng.y) discard;\n"
"  uint rel = A - src.x;\n"
"  if (dst.w == 4u) {\n"
"    uint w = src.w == 4u ? ld(rel >> 2) : (pack16(src.z, ld(rel >> 1)) | (pack16(src.z, ld((rel >> 1) + 1u)) << 16));\n"
"    o_c = bytes4(w); return;\n"
"  }\n"
"  uint p;\n"
"  if (src.w == 4u) { uint w = ld(rel >> 2); p = ((rel >> 1) & 1u) != 0u ? (w >> 16) : (w & 0xFFFFu); }\n"
"  else p = pack16(src.z, ld(rel >> 1));\n"
"  o_c = bytes4(expand16(dst.z, p));\n"
"}\n";

/* ---- render targets ------------------------------------------------------------------ */

static void rt_free(rt_entry *t) {
    if (t->fbo) glDeleteFramebuffers(1, &t->fbo);
    if (t->tex) glDeleteTextures(1, &t->tex);
    memset(t, 0, sizeof *t);
}

/* Readbacks by cause (gl_ge_report), as d3d11.c. */
static uint64_t n_rb_reason[7];
static int g_rb_reason;
static void rt_readback_inner(rt_entry *t);
static int trace_on(void);
static void rt_readback(rt_entry *t) {
    if (t->dirty) n_rb_reason[g_rb_reason]++;
    if (t->dirty && trace_on())
        fprintf(stderr, "readback: 0x%06X/%u fmt %d rows %u-%u reason %d\n", t->off, t->stride, t->fmt, t->dy0, t->dy1, g_rb_reason);
    rt_readback_inner(t);
}

static uint32_t *g_rb_buf;                       /* rows read back from a target */
static size_t g_rb_cap;

static const uint32_t *read_rows(const rt_entry *t, uint32_t y0, uint32_t y1) {
    const size_t need = (size_t)t->w * (y1 - y0);
    if (need > g_rb_cap) {
        uint32_t *p = (uint32_t *)realloc(g_rb_buf, need * 4);
        if (!p) return NULL;
        g_rb_buf = p; g_rb_cap = need;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
    glReadPixels(0, (GLint)y0, (GLsizei)t->w, (GLsizei)(y1 - y0), GL_RGBA, GL_UNSIGNED_BYTE, g_rb_buf);
    return g_rb_buf;
}

static void rt_readback_inner(rt_entry *t) {
    if (!t->dirty) return;
    uint8_t *v = vram();
    uint32_t y0 = t->dy0, y1 = t->dy1 > t->h ? t->h : t->dy1;
    if (!v || y0 >= y1) { t->dirty = 0; memset(t->drow, 0, sizeof t->drow); return; }
    const uint32_t *px = read_rows(t, y0, y1);
    if (!px) return;
    const int bpp = bpp_of(t->fmt);
    for (uint32_t y = y0; y < y1; y++) {
        if (!row_dirty(t, y)) continue;
        uint32_t o = t->off + y * t->stride * (uint32_t)bpp;
        if (o + t->stride * (uint32_t)bpp > VRAM_SIZE) break;
        const uint32_t *src = px + (size_t)(y - y0) * t->w;
        if (bpp == 4) memcpy(v + o, src, t->stride * 4u);
        else for (uint32_t x = 0; x < t->stride; x++) {
            uint16_t p = pack16(t->fmt, src[x]);
            memcpy(v + o + x * 2, &p, 2);
        }
    }
    t->dirty = 0;
    t->dy0 = t->dy1 = 0;
    memset(t->drow, 0, sizeof t->drow);
    uint32_t bytes = t->rows * t->stride * (uint32_t)bpp;
    if (t->off + bytes > VRAM_SIZE) bytes = VRAM_SIZE - t->off;
    t->synced_hash = hash_bytes(v + t->off, bytes);
    for (int i = 0; i < MAX_RT; i++)
        if (g_rt[i].used && &g_rt[i] != t && overlaps(t->off, t->off + bytes, g_rt[i].off, rt_end(&g_rt[i])))
            g_rt[i].check_frame = (uint64_t)-1;
    n_readbacks++;
}

static void rt_upload(rt_entry *t) {
    uint8_t *v = vram();
    if (!v) return;
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
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)t->stride, (GLsizei)t->rows, GL_RGBA, GL_UNSIGNED_BYTE, px);
    free(px);
    uint32_t bytes = t->rows * t->stride * (uint32_t)bpp;
    if (t->off + bytes > VRAM_SIZE) bytes = VRAM_SIZE - t->off;
    t->synced_hash = hash_bytes(v + t->off, bytes);
    pages_set(g_pg_armed, t->off, t->off + bytes);
    n_uploads++;
}

static GLuint new_texture(uint32_t w, uint32_t h) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    return tex;
}

static GLuint new_fbo(GLuint tex) {
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &fbo);
        return 0;
    }
    return fbo;
}

static rt_entry *rt_find(uint32_t off, uint32_t stride, int fmt) {
    for (int i = 0; i < MAX_RT; i++)
        if (g_rt[i].used && g_rt[i].off == off && g_rt[i].stride == stride && g_rt[i].fmt == fmt) return &g_rt[i];
    return NULL;
}

static rt_entry *rt_get(uint32_t off, uint32_t stride, int fmt, uint32_t rows) {
    rt_entry *t = rt_find(off, stride, fmt);
    if (!t) {
        for (int i = 0; i < MAX_RT && !t; i++) if (!g_rt[i].used) t = &g_rt[i];
        if (!t) {
            t = &g_rt[0];
            for (int i = 1; i < MAX_RT; i++) if (g_rt[i].last_use < t->last_use) t = &g_rt[i];
            g_rb_reason = 1; rt_readback(t);
            rt_free(t);
        }
        t->off = off; t->stride = stride; t->fmt = fmt;
        t->w = stride; t->h = 512;
        t->tex = new_texture(t->w, t->h);
        t->fbo = new_fbo(t->tex);
        if (!t->fbo) { if (t->tex) glDeleteTextures(1, &t->tex); memset(t, 0, sizeof *t); return NULL; }
        t->used = 1;
        t->rows = rows;
        if (getenv("PSP2I_RT_TRACE"))
            fprintf(stderr, "gl: render target VRAM+0x%06X stride %u format %d rows %u (flip %llu)\n",
                    off, stride, fmt, rows, (unsigned long long)frame_no());
        rt_upload(t);
        t->check_frame = frame_no();
    }
    if (rows > t->rows) {
        if (t->dirty) { g_rb_reason = 2; rt_readback(t); }
        t->rows = rows;
        rt_upload(t);
    }
    t->last_use = ++g_use;
    return t;
}

static void rt_check(rt_entry *t) {
    uint64_t f = frame_no();
    if (t->check_frame == f || t->dirty) return;
    t->check_frame = f;
    uint8_t *v = vram();
    uint32_t bytes = t->rows * t->stride * (uint32_t)bpp_of(t->fmt);
    if (t->off + bytes > VRAM_SIZE) bytes = VRAM_SIZE - t->off;
    if (v && hash_bytes(v + t->off, bytes) != t->synced_hash) rt_upload(t);
    pages_set(g_pg_armed, t->off, t->off + bytes);
}

/* ---- depth buffers ---------------------------------------------------------------------- */

static depth_entry *depth_get(uint32_t off, uint32_t w, uint32_t h) {
    depth_entry *d = NULL;
    for (int i = 0; i < MAX_DEPTH; i++)
        if (g_depth[i].used && g_depth[i].off == off && g_depth[i].w == w && g_depth[i].h == h) d = &g_depth[i];
    if (!d) {
        for (int i = 0; i < MAX_DEPTH && !d; i++) if (!g_depth[i].used) d = &g_depth[i];
        if (!d) {
            d = &g_depth[0];
            for (int i = 1; i < MAX_DEPTH; i++) if (g_depth[i].last_use < d->last_use) d = &g_depth[i];
            for (int i = 0; i < MAX_RT; i++) if (g_rt[i].used && g_rt[i].depth_rb == d->rb) g_rt[i].depth_rb = (GLuint)-1;
            if (d->rb) glDeleteRenderbuffers(1, &d->rb);
            memset(d, 0, sizeof *d);
        }
        glGenRenderbuffers(1, &d->rb);
        glBindRenderbuffer(GL_RENDERBUFFER, d->rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, (GLsizei)w, (GLsizei)h);
        d->off = off; d->w = w; d->h = h; d->used = 1; d->cleared = 0;
    }
    d->last_use = ++g_use;
    return d;
}

/* Bind t's framebuffer with depth buffer d (or none), clearing a new one to 0
 * as d3d11.c does when it creates it. */
static void bind_target(rt_entry *t, depth_entry *d) {
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    const GLuint rb = d ? d->rb : 0;
    if (t->depth_rb != rb) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
        t->depth_rb = rb;
    }
    if (d && !d->cleared) {
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glClearDepth(0.0);
        glClear(GL_DEPTH_BUFFER_BIT);
        d->cleared = 1;
    }
}

/* ---- textures ----------------------------------------------------------------------------- */

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
        if (e->used && (e->w != st->tex_w || e->h != st->tex_h)) { glDeleteTextures(1, &e->tex); e->tex = 0; }
        if (!e->tex) e->tex = new_texture(st->tex_w, st->tex_h);
        e->key = st->tex_key; e->w = st->tex_w; e->h = st->tex_h; e->used = 1;
    }
    uint32_t *px = (uint32_t *)malloc((size_t)st->tex_w * st->tex_h * 4);
    if (!px) return NULL;
    psp_gpu_decode_texture(px);
    glBindTexture(GL_TEXTURE_2D, e->tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)st->tex_w, (GLsizei)st->tex_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    free(px);
    e->hash = h; e->check_frame = f; e->last_use = ++g_use;
    n_tex_decodes++;
    return e;
}

/* ---- blend / depth / sampler state -------------------------------------------------------- */

static GLenum map_factor(int f, int is_src, uint32_t fix, uint32_t *blend_factor, int *approx) {
    switch (f) {
    case 0:  return is_src ? GL_DST_COLOR : GL_SRC_COLOR;
    case 1:  return is_src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 2:  return GL_SRC_ALPHA;
    case 3:  return GL_ONE_MINUS_SRC_ALPHA;
    case 4:  return GL_DST_ALPHA;
    case 5:  return GL_ONE_MINUS_DST_ALPHA;
    case 6:  *approx = 1; return GL_SRC_ALPHA;            /* 2 x src alpha */
    case 7:  *approx = 1; return GL_ONE_MINUS_SRC_ALPHA;
    case 8:  *approx = 1; return GL_DST_ALPHA;
    case 9:  *approx = 1; return GL_ONE_MINUS_DST_ALPHA;
    default:
        if (fix == 0xFFFFFF) return GL_ONE;
        if (fix == 0) return GL_ZERO;
        if (*blend_factor == 0xFFFFFFFFu || *blend_factor == fix) { *blend_factor = fix; return GL_CONSTANT_COLOR; }
        if (*blend_factor == (~fix & 0xFFFFFF)) return GL_ONE_MINUS_CONSTANT_COLOR;
        *approx = 1;
        return GL_CONSTANT_COLOR;
    }
}

static void apply_blend(const psp_gpu_state *st, int *approx) {
    GLboolean wr = 0, wg = 0, wb = 0, wa = 0;
    if (st->clear) {
        if (st->clear_which & 1) wr = wg = wb = 1;
        if (st->clear_which & 2) wa = 1;
    } else {
        GLboolean *w[3] = { &wr, &wg, &wb };
        for (int ch = 0; ch < 3; ch++) {
            uint32_t m = (st->mask_rgb >> (ch * 8)) & 0xFF;
            if (m != 0xFF) *w[ch] = 1;
            if (m != 0 && m != 0xFF) *approx = 1;
        }
        if (st->mask_a != 0xFF) wa = 1;
        if (st->mask_a != 0 && st->mask_a != 0xFF) *approx = 1;
    }
    if (st->fb_fmt == 0) wa = 0;                          /* 565 has no alpha */
    glColorMask(wr, wg, wb, wa);
    uint32_t bf = 0xFFFFFFFFu;
    if (st->blend && !st->clear) {
        const GLenum s = map_factor(st->bsrc, 1, st->fixa, &bf, approx);
        const GLenum d = map_factor(st->bdst, 0, st->fixb, &bf, approx);
        static const GLenum OPS[6] = { GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX, GL_MAX };
        if (st->beq >= 5) *approx = 1;
        glEnable(GL_BLEND);
        glBlendFuncSeparate(s, d, GL_ONE, GL_ZERO);       /* the result keeps the source alpha */
        glBlendEquationSeparate(OPS[st->beq < 6 ? st->beq : 0], GL_FUNC_ADD);
    } else glDisable(GL_BLEND);
    if (bf == 0xFFFFFFFFu) bf = 0;
    glBlendColor((float)(bf & 0xFF) / 255.0f, (float)((bf >> 8) & 0xFF) / 255.0f, (float)((bf >> 16) & 0xFF) / 255.0f, 1.0f);
}

static void apply_depth(const psp_gpu_state *st, int have_depth) {
    static const GLenum F[8] = { GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL };
    int enable, write;
    GLenum fn;
    if (st->clear) { enable = (st->clear_which & 4) != 0; write = 1; fn = GL_ALWAYS; }
    else { enable = st->ztest; write = st->zwrite; fn = F[st->zfunc & 7]; }
    if (!have_depth) enable = 0;                          /* no depth buffer: D3D11 binds no DSV */
    if (enable) { glEnable(GL_DEPTH_TEST); glDepthFunc(fn); glDepthMask(write ? GL_TRUE : GL_FALSE); }
    else { glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); }
}

static GLuint sampler(int linear, int cu, int cv) {
    const int k = (linear ? 1 : 0) | (cu ? 2 : 0) | (cv ? 4 : 0);
    if (!g_samp[k]) {
        glGenSamplers(1, &g_samp[k]);
        glSamplerParameteri(g_samp[k], GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        glSamplerParameteri(g_samp[k], GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        glSamplerParameteri(g_samp[k], GL_TEXTURE_WRAP_S, cu ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glSamplerParameteri(g_samp[k], GL_TEXTURE_WRAP_T, cv ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    }
    return g_samp[k];
}

/* ---- sampling a target ---------------------------------------------------------------------- */

static int rt_resolve_copy(rt_entry *r);
static int xfer_enabled(void);

/* A copy of target t to sample while t is being drawn to. */
static GLuint scratch_copy(const rt_entry *t) {
    if (!g_scratch || g_scratch_w != t->w || g_scratch_h != t->h) {
        if (g_scratch_fbo) glDeleteFramebuffers(1, &g_scratch_fbo);
        if (g_scratch) glDeleteTextures(1, &g_scratch);
        g_scratch = new_texture(t->w, t->h);
        g_scratch_fbo = new_fbo(g_scratch);
        g_scratch_w = t->w; g_scratch_h = t->h;
    }
    glDisable(GL_SCISSOR_TEST);                           /* blits obey the scissor */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_scratch_fbo);
    glBlitFramebuffer(0, 0, (GLint)t->w, (GLint)t->h, 0, 0, (GLint)t->w, (GLint)t->h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    return g_scratch;
}

static GLuint texture_view(const psp_gpu_state *st, rt_entry *target, float scale[2]) {
    const uint32_t toff = vram_off(st->tex_addr);
    const int in_vram = (st->tex_addr & 0x0F000000u) == 0x04000000u;
    if (in_vram) {
        static const int tbpp[11] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8 };
        uint32_t tend = toff + st->tex_bufw * st->tex_h * (uint32_t)tbpp[st->tex_psm > 10 ? 3 : st->tex_psm] / 8;
        if (xfer_enabled() && st->tex_psm <= 3 && !st->tex_swz) {
            rt_entry *r = rt_find(toff, st->tex_bufw, st->tex_psm);
            if (r && tend <= rt_end(r) && rt_resolve_copy(r)) {
                n_rt_tex++;
                scale[0] = 1.0f / (float)r->w;
                scale[1] = 1.0f / (float)r->h;
                return r != target ? r->tex : scratch_copy(r);
            }
        }
        for (int i = 0; i < MAX_RT; i++) {
            rt_entry *t = &g_rt[i];
            if (!t->used || !overlaps(toff, tend, t->off, rt_end(t))) continue;
            const int same_fmt = st->tex_psm <= 3 && st->tex_psm == t->fmt && !st->tex_swz;
            if (t->dirty && t->off == toff && st->tex_bufw == t->stride && same_fmt) {
                n_rt_tex++;
                scale[0] = 1.0f / (float)t->w;
                scale[1] = 1.0f / (float)t->h;
                return t != target ? t->tex : scratch_copy(t);
            }
            if (t->dirty && overlaps(toff, tend, row_addr(t, t->dy0), row_addr(t, t->dy1))) { g_rb_reason = 3; rt_readback(t); }
        }
    }
    tex_entry *e = tex_get(st);
    scale[0] = 1.0f / (float)st->tex_w;
    scale[1] = 1.0f / (float)st->tex_h;
    return e ? e->tex : 0;
}

static uint64_t g_trace_flip = (uint64_t)-1;
static int trace_on(void) {
    static int init;
    if (!init) { const char *e = getenv("PSP2I_DRAW_TRACE"); if (e) g_trace_flip = strtoull(e, NULL, 0); init = 1; }
    return frame_no() == g_trace_flip;
}

/* ---- moving bytes between overlapping targets on the GPU (as d3d11.c) ------------------------- */

static uint64_t n_xfer;

static int xfer_enabled(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("PSP2I_XFER"); on = !(e && e[0] == '0'); }
    return on && g_xprog;
}

static uint32_t rt_rowbytes(const rt_entry *t) { return t->stride * (uint32_t)bpp_of(t->fmt); }

static int xfer_rows(const rt_entry *o, const rt_entry *t, uint32_t *r0, uint32_t *r1) {
    const uint32_t ro = rt_rowbytes(o), oe = o->off + o->h * ro;
    const uint32_t lo = o->off > t->off ? o->off : t->off, e = rt_end(t), hi = oe < e ? oe : e;
    *r0 = *r1 = 0;
    if (lo >= hi) return 1;
    uint32_t a = (lo - o->off) / ro, b = (hi - o->off + ro - 1) / ro;
    if (b > o->h) b = o->h;
    if ((lo - o->off) % ro && row_dirty(o, a)) return 0;
    if ((hi - o->off) % ro && row_dirty(o, b - 1)) return 0;
    if ((lo - o->off) % ro) a++;
    if ((hi - o->off) % ro) b--;
    if (((row_addr(o, a) - t->off) % (uint32_t)bpp_of(t->fmt))) return 0;
    *r0 = a; *r1 = b > a ? b : a;
    return 1;
}

static void rt_xfer(rt_entry *t, rt_entry *o, uint32_t a, uint32_t b, int own) {
    const uint32_t i0 = row_addr(o, a), i1 = row_addr(o, b);
    if (i0 >= i1) return;
    const uint32_t rt = rt_rowbytes(t);
    uint32_t y0 = (i0 - t->off) / rt, y1 = (i1 - t->off + rt - 1) / rt;
    if (y1 > t->h) y1 = t->h;
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, (GLsizei)t->w, (GLsizei)t->h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, (GLint)y0, (GLsizei)t->w, (GLsizei)(y1 - y0));
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glColorMask(1, 1, 1, 1);
    glUseProgram(g_xprog);
    glUniform4ui(u_xdst, t->off, t->stride, (GLuint)t->fmt, (GLuint)bpp_of(t->fmt));
    glUniform4ui(u_xsrc, o->off, o->stride, (GLuint)o->fmt, (GLuint)bpp_of(o->fmt));
    glUniform4ui(u_xrng, i0, i1, 0, 0);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, o->tex);
    glBindSampler(1, 0);
    glBindVertexArray(g_xvao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    if (own) rows_mark(t, y0, y1);
    n_xfer++;
}

static int rt_resolve_copy(rt_entry *r) {
    const uint32_t r0 = r->off, r1 = rt_end(r);
    int flushed = 0, newer = r->dirty;
    for (int i = 0; i < MAX_RT; i++) {
        rt_entry *o = &g_rt[i];
        if (!o->used || o == r || !o->dirty || !overlaps(r0, r1, row_addr(o, o->dy0), row_addr(o, o->dy1))) continue;
        uint32_t a, b;
        if (xfer_rows(o, r, &a, &b)) continue;
        g_rb_reason = 3; rt_readback(o); flushed = 1;
    }
    if (flushed && !r->dirty) r->check_frame = (uint64_t)-1;
    rt_check(r);
    for (int i = 0; i < MAX_RT; i++) {
        rt_entry *o = &g_rt[i];
        if (!o->used || o == r || !o->dirty || !overlaps(r0, r1, row_addr(o, o->dy0), row_addr(o, o->dy1))) continue;
        uint32_t a, b;
        if (!xfer_rows(o, r, &a, &b) || a >= b) continue;
        for (uint32_t y = a; y < b; ) {
            if (!row_dirty(o, y)) { y++; continue; }
            uint32_t z = y;
            while (z < b && row_dirty(o, z)) z++;
            rt_xfer(r, o, y, z, 0);
            newer = 1;
            y = z;
        }
    }
    return newer;
}

/* ---- the draw ------------------------------------------------------------------------------------ */

static int gl_draw(const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts) {
    if (!g_ok || (st->fb_addr & 0x0F000000u) != 0x04000000u || !st->fb_stride || nverts <= 0) return 1;
    if ((uint32_t)nverts > VB_VERTS) return 1;
    const uint32_t off = vram_off(st->fb_addr);
    uint32_t rows = (uint32_t)st->sy1 + 1;
    if (rows > 512) rows = 512;
    rt_entry *t = rt_get(off, st->fb_stride, st->fb_fmt, rows);
    if (!t) return 1;
    {   /* overlapping targets: see d3d11.c d3d_draw */
        const uint32_t t0 = t->off, t1 = rt_end(t);
        int flushed = 0;
        for (int i = 0; i < MAX_RT; i++) {
            rt_entry *o = &g_rt[i];
            if (!o->used || o == t || !o->dirty || !overlaps(t0, t1, o->off, rt_end(o))) continue;
            uint32_t a, b;
            if (xfer_enabled() && xfer_rows(o, t, &a, &b)) continue;
            g_rb_reason = 4; rt_readback(o); flushed = 1;
        }
        if (flushed && !t->dirty) t->check_frame = (uint64_t)-1;
        rt_check(t);
        for (int i = 0; i < MAX_RT; i++) {
            rt_entry *o = &g_rt[i];
            if (!o->used || o == t || !overlaps(t0, t1, o->off, rt_end(o))) continue;
            o->check_frame = (uint64_t)-1;
            uint32_t a, b;
            if (!o->dirty || !xfer_rows(o, t, &a, &b) || a >= b) continue;
            for (uint32_t y = a; y < b; ) {
                if (!row_dirty(o, y)) { y++; continue; }
                uint32_t z = y;
                while (z < b && row_dirty(o, z)) z++;
                rt_xfer(t, o, y, z, 1);
                y = z;
            }
            rows_clear(o, a, b);
        }
    }

    depth_entry *d = NULL;
    if (st->zb_stride && (st->ztest || (st->clear && (st->clear_which & 4))))
        d = depth_get(vram_off(st->zb_addr), t->w, t->h);

    GLuint tex = 0;
    float uvs[2] = { 1, 1 };
    if (st->tex) {
        tex = texture_view(st, t, uvs);
        if (!tex) return 1;
    }

    /* Vertices into the ring. glBufferSubData, not glMapBufferRange: a map
     * hands memory back to the caller, which makes a threaded driver (NVIDIA's
     * default) wait for its worker thread on every draw -- 35% of the game
     * thread in a profile of the lobby, at ~13,000 draws a second. A sub-data
     * upload is queued like any other command. */
    GLint first;
    glBindBuffer(GL_ARRAY_BUFFER, g_vb);
    if (g_vb_pos + (uint32_t)nverts > VB_VERTS) {            /* wrap: orphan the old storage */
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)VB_VERTS * (GLsizeiptr)sizeof(psp_gpu_vertex), NULL, GL_STREAM_DRAW);
        g_vb_pos = 0;
    }
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)g_vb_pos * (GLintptr)sizeof(psp_gpu_vertex),
                    (GLsizeiptr)nverts * (GLsizeiptr)sizeof(psp_gpu_vertex), v);
    first = (GLint)g_vb_pos;
    g_vb_pos += (uint32_t)nverts;

    bind_target(t, d);
    glViewport(0, 0, (GLsizei)t->w, (GLsizei)t->h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(st->sx0, st->sy0, st->sx1 - st->sx0 + 1, st->sy1 - st->sy0 + 1);
    int approx = 0;
    apply_blend(st, &approx);
    if (approx) n_approx++;
    apply_depth(st, d != NULL);

    glUseProgram(g_prog);
    glUniform4f(u_vp, (float)t->w, (float)t->h, uvs[0], uvs[1]);
    glUniform4f(u_env, (float)(st->env & 0xFF) / 255.0f, (float)((st->env >> 8) & 0xFF) / 255.0f,
                (float)((st->env >> 16) & 0xFF) / 255.0f, 0.0f);
    glUniform4ui(u_tf, (GLuint)(st->tex != 0), (GLuint)st->tfunc, (GLuint)st->trgba, (GLuint)st->tdbl);
    glUniform4ui(u_at, (GLuint)(st->atest && !st->clear), (GLuint)st->afunc, st->aref, st->amask);
    glUniform4ui(u_misc, (GLuint)st->clear, 0, 0, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, tex ? sampler(st->tex_linear, st->tex_clamp_u, st->tex_clamp_v) : 0);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, first, nverts);

    float ymin = 1e9f, ymax = -1e9f;
    for (int i = 0; i < nverts; i++) { if (v[i].y < ymin) ymin = v[i].y; if (v[i].y > ymax) ymax = v[i].y; }
    int y0 = (int)ymin - 1, y1 = (int)ymax + 2;
    if (y0 < st->sy0) y0 = st->sy0;
    if (y1 > st->sy1 + 1) y1 = st->sy1 + 1;
    if (y0 < y1) rows_mark(t, (uint32_t)y0, (uint32_t)y1);
    n_draws++;
    n_tris += (uint64_t)nverts / 3;
    return 0;
}

static void gl_sync_vram(uint32_t addr, uint32_t bytes) {
    if ((addr & 0x0F000000u) != 0x04000000u) return;
    uint32_t a0 = vram_off(addr), a1 = a0 + bytes;
    for (int i = 0; i < MAX_RT; i++)
        if (g_rt[i].used && g_rt[i].dirty && overlaps(a0, a1, g_rt[i].off, rt_end(&g_rt[i])))
            { g_rb_reason = 5; rt_readback(&g_rt[i]); }
}

static void gl_vram_written(uint32_t addr, uint32_t bytes) {
    if ((addr & 0x0F000000u) != 0x04000000u) return;
    uint32_t a0 = vram_off(addr), a1 = a0 + bytes;
    for (int i = 0; i < MAX_RT; i++)
        if (g_rt[i].used && overlaps(a0, a1, g_rt[i].off, rt_end(&g_rt[i])))
            g_rt[i].check_frame = (uint64_t)-1;
}

static void gl_cpu_access(uint32_t off, uint32_t size) {
    const uint32_t a0 = off, a1 = off + size > VRAM_SIZE ? VRAM_SIZE : off + size;
    if (a0 >= a1) return;
    const uint32_t p0 = a0 >> PG_SHIFT, p1 = (a1 - 1) >> PG_SHIFT;
    int dirty = 0, armed = 0;
    for (uint32_t p = p0; p <= p1; p++) { dirty |= g_pg_dirty[p]; armed |= g_pg_armed[p]; }
    if (!dirty && !armed) return;
    const uint32_t q0 = p0 << PG_SHIFT, q1 = (p1 + 1) << PG_SHIFT;
    if (dirty) {
        for (int i = 0; i < MAX_RT; i++)
            if (g_rt[i].used && g_rt[i].dirty && overlaps(a0, a1, g_rt[i].off, rt_end(&g_rt[i])))
                { g_rb_reason = 6; rt_readback(&g_rt[i]); g_rt[i].check_frame = (uint64_t)-1; }
        memset(g_pg_dirty + p0, 0, p1 - p0 + 1);
        for (int i = 0; i < MAX_RT; i++) {
            const rt_entry *t = &g_rt[i];
            if (!t->used || !t->dirty) continue;
            uint32_t d0 = row_addr(t, t->dy0), d1 = row_addr(t, t->dy1);
            if (d0 < q0) d0 = q0;
            if (d1 > q1) d1 = q1;
            pages_set(g_pg_dirty, d0, d1);
        }
    }
    if (armed) {
        for (int i = 0; i < MAX_RT; i++)
            if (g_rt[i].used && overlaps(a0, a1, g_rt[i].off, rt_end(&g_rt[i])))
                g_rt[i].check_frame = (uint64_t)-1;
        memset(g_pg_armed + p0, 0, p1 - p0 + 1);
        for (int i = 0; i < MAX_RT; i++) {
            const rt_entry *t = &g_rt[i];
            if (!t->used || t->check_frame == (uint64_t)-1) continue;
            uint32_t d0 = t->off, d1 = rt_end(t);
            if (d0 < q0) d0 = q0;
            if (d1 > q1) d1 = q1;
            pages_set(g_pg_armed, d0, d1);
        }
    }
}

/* ---- presenting without a stall (as d3d11_present) ------------------------------------------------
 * While the display target is ahead of VRAM, each vblank queues an
 * asynchronous read of it into one of PRESENT_RING pixel buffers, with a
 * fence, and shows the newest one the GPU has finished. */
#define PRESENT_RING 3
static GLuint g_pbo[PRESENT_RING];
static GLsync g_fence[PRESENT_RING];
static size_t g_pbo_bytes[PRESENT_RING];
static int g_present_fmt[PRESENT_RING], g_present_pending[PRESENT_RING], g_present_w[PRESENT_RING];
static uint64_t g_present_seq[PRESENT_RING], g_present_next;
static int g_present_head;
static uint64_t n_present_async, n_present_vram, n_present_kept;

static void fence_drop(int k) {
    if (g_fence[k]) { glDeleteSync(g_fence[k]); g_fence[k] = NULL; }
}

int gl_ge_present(uint32_t addr, uint32_t stride, int fmt, uint32_t *out, int w, int h) {
    if (!g_ok || (addr & 0x0F000000u) != 0x04000000u) return 0;
    rt_entry *t = rt_find(vram_off(addr), stride, fmt);
    const int ahead = t && t->dirty && t->stride == stride && t->fmt == fmt && t->w >= (uint32_t)w;
    if (!ahead) {
        for (int k = 0; k < PRESENT_RING; k++) { g_present_pending[k] = 0; fence_drop(k); }
        n_present_vram++;
        return 0;
    }
    {   /* queue a copy of this frame */
        const int k = g_present_head;
        g_present_head = (g_present_head + 1) % PRESENT_RING;
        const size_t bytes = (size_t)w * (size_t)h * 4;
        if (!g_pbo[k]) glGenBuffers(1, &g_pbo[k]);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbo[k]);
        if (g_pbo_bytes[k] != bytes) { glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)bytes, NULL, GL_STREAM_READ); g_pbo_bytes[k] = bytes; }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (void *)0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        fence_drop(k);
        g_fence[k] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();                                        /* start it now */
        g_present_fmt[k] = fmt;
        g_present_w[k] = w;
        g_present_pending[k] = 1;
        g_present_seq[k] = ++g_present_next;
    }
    static int verify = -1;
    if (verify < 0) verify = getenv("PSP2I_PRESENT_VERIFY") != NULL;
    int shown = 2;
    for (int pass = 0; pass < PRESENT_RING; pass++) {
        int best = -1;
        for (int k = 0; k < PRESENT_RING; k++)
            if (g_present_pending[k] == 1 && (best < 0 || g_present_seq[k] > g_present_seq[best])) best = k;
        if (best < 0) break;
        const GLenum r = glClientWaitSync(g_fence[best], verify ? GL_SYNC_FLUSH_COMMANDS_BIT : 0, verify ? 1000000000ull : 0);
        if (r == GL_TIMEOUT_EXPIRED) { g_present_pending[best] = 2; continue; }
        if (r == GL_WAIT_FAILED) break;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbo[best]);
        const uint8_t *m = (const uint8_t *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)g_pbo_bytes[best], GL_MAP_READ_BIT);
        if (!m) { glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); break; }
        static uint32_t lut[4][3][256];
        static int lut_ready;
        if (!lut_ready) {
            for (int fm = 0; fm < 4; fm++)
                for (uint32_t c = 0; c < 256; c++) {
                    uint32_t rr, gg, bb;
                    switch (fm) {
                    case 0:  rr = (c >> 3) * 255 / 31; gg = (c >> 2) * 255 / 63; bb = (c >> 3) * 255 / 31; break;
                    case 1:  rr = gg = bb = (c >> 3) * 255 / 31; break;
                    case 2:  rr = gg = bb = (c >> 4) * 17; break;
                    default: rr = gg = bb = c; break;
                    }
                    lut[fm][0][c] = 0xFF000000u | (rr << 16);
                    lut[fm][1][c] = gg << 8;
                    lut[fm][2][c] = bb;
                }
            lut_ready = 1;
        }
        const uint32_t (*L)[256] = lut[g_present_fmt[best] & 3];
        const int pw = g_present_w[best];
        for (int y = 0; y < h; y++) {
            const uint32_t *src = (const uint32_t *)(m + (size_t)y * (size_t)pw * 4);
            uint32_t *dst = out + (size_t)y * (size_t)w;
            for (int x = 0; x < w; x++) {
                const uint32_t c = src[x];
                dst[x] = L[0][c & 0xFF] | L[1][(c >> 8) & 0xFF] | L[2][(c >> 16) & 0xFF];
            }
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        const uint64_t s = g_present_seq[best];
        for (int k = 0; k < PRESENT_RING; k++)
            if (g_present_pending[k] && g_present_seq[k] <= s) { g_present_pending[k] = 0; fence_drop(k); }
        shown = 1;
        n_present_async++;
        break;
    }
    for (int k = 0; k < PRESENT_RING; k++) if (g_present_pending[k] == 2) g_present_pending[k] = 1;
    if (shown == 2) n_present_kept++;
    return shown;
}

/* ---- the window: the composed frame scaled into the default framebuffer --------------------------- */

static GLuint g_bprog, g_btex;
static int g_btex_w, g_btex_h;

static const char *BVS_SRC =
"#version 330 core\n"
"out vec2 v_uv;\n"
"void main() {\n"
"  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
"  v_uv = vec2(p.x, 1.0 - p.y);\n"                    /* frame row 0 at the top of the window */
"  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"}\n";
static const char *BFS_SRC =
"#version 330 core\n"
"uniform sampler2D F; in vec2 v_uv; out vec4 o_c;\n"
"void main() { o_c = vec4(texture(F, v_uv).rgb, 1.0); }\n";

int gl_blit_frame(const uint32_t *px, int w, int h, int x, int y, int vw, int vh, int cw, int ch) {
    if (!g_ok) return -1;
    if (!g_bprog) {
        g_bprog = gl_program("present", BVS_SRC, BFS_SRC);
        if (!g_bprog) return -1;
    }
    if (!g_btex || g_btex_w != w || g_btex_h != h) {
        if (g_btex) glDeleteTextures(1, &g_btex);
        g_btex = new_texture((uint32_t)w, (uint32_t)h);
        g_btex_w = w; g_btex_h = h;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_btex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, px);   /* 0xAARRGGBB */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, cw, ch);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(x, ch - y - vh, vw, vh);                   /* window y from the bottom */
    glUseProgram(g_bprog);
    glBindSampler(0, sampler(0, 1, 1));
    glBindVertexArray(g_xvao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    return 0;
}

/* ---- setup ------------------------------------------------------------------------------------------- */

static const psp_gpu_backend GL_BACKEND = { "gl", gl_draw, gl_sync_vram, gl_vram_written, 1 };

int gl_ge_init(void) {
    g_prog = gl_program("ge", VS_SRC, FS_SRC);
    if (!g_prog) return -1;
    u_vp = glGetUniformLocation(g_prog, "vp");
    u_env = glGetUniformLocation(g_prog, "env");
    u_tf = glGetUniformLocation(g_prog, "tf");
    u_at = glGetUniformLocation(g_prog, "at");
    u_misc = glGetUniformLocation(g_prog, "misc");
    u_tex = glGetUniformLocation(g_prog, "T");
    glUseProgram(g_prog);
    glUniform1i(u_tex, 0);
    g_xprog = gl_program("transfer", XVS_SRC, XFS_SRC);
    if (g_xprog) {
        u_xdst = glGetUniformLocation(g_xprog, "dst");
        u_xsrc = glGetUniformLocation(g_xprog, "src");
        u_xrng = glGetUniformLocation(g_xprog, "rng");
        u_xtex = glGetUniformLocation(g_xprog, "Y");
        glUseProgram(g_xprog);
        glUniform1i(u_xtex, 1);
    } else fprintf(stderr, "gl: transfer shader unavailable; overlaps go through VRAM\n");
    glUseProgram(0);

    glGenBuffers(1, &g_vb);
    glBindBuffer(GL_ARRAY_BUFFER, g_vb);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)VB_VERTS * (GLsizeiptr)sizeof(psp_gpu_vertex), NULL, GL_STREAM_DRAW);
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(psp_gpu_vertex), (const void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(psp_gpu_vertex), (const void *)16);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(psp_gpu_vertex), (const void *)24);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glGenVertexArrays(1, &g_xvao);                        /* attribute-less draws */
    glBindVertexArray(0);

    glDisable(GL_CULL_FACE);                              /* culling is done before submission */
    glDisable(GL_DITHER);
    glEnable(GL_DEPTH_CLAMP);                             /* as DepthClipEnable = FALSE */
    glDepthRange(0.0, 1.0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (glGetError() != GL_NO_ERROR) { fprintf(stderr, "gl: setup failed\n"); return -1; }
    g_ok = 1;
    psp_gpu_set_backend(&GL_BACKEND);
    psp_mem_set_vram_hook(gl_cpu_access);
    fprintf(stderr, "gl: rendering on the GPU (%s; %s)\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    return 0;
}

void gl_ge_report(FILE *out) {
    if (!g_ok) return;
    fprintf(out, "  gl                  %llu draws, %llu triangles; %llu VRAM uploads, %llu readbacks; "
                 "%llu texture decodes, %llu render-to-texture samples; %llu draws approximated\n",
            (unsigned long long)n_draws, (unsigned long long)n_tris, (unsigned long long)n_uploads,
            (unsigned long long)n_readbacks, (unsigned long long)n_tex_decodes,
            (unsigned long long)n_rt_tex, (unsigned long long)n_approx);
    fprintf(out, "  gl readbacks by cause: relayout %llu, evict %llu, grow %llu, texture %llu, overlap %llu, sync %llu, cpu access %llu\n",
            (unsigned long long)n_rb_reason[0], (unsigned long long)n_rb_reason[1], (unsigned long long)n_rb_reason[2],
            (unsigned long long)n_rb_reason[3], (unsigned long long)n_rb_reason[4], (unsigned long long)n_rb_reason[5],
            (unsigned long long)n_rb_reason[6]);
    fprintf(out, "  gl transfers        %llu GPU-side target-to-target transfers\n", (unsigned long long)n_xfer);
    fprintf(out, "  gl present          %llu frames from asynchronous copies, %llu from VRAM, %llu vblanks kept the last picture\n",
            (unsigned long long)n_present_async, (unsigned long long)n_present_vram, (unsigned long long)n_present_kept);
}
