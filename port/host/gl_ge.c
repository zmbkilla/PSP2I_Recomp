/* OpenGL 3.3 core backend for the GE -- the portable counterpart of d3d11.c.
 *
 * Same design, same invariants, same names: the runtime (psprecomp/src/hle/
 * gpu.c) does the vertex work and hands each draw over as a screen-space
 * triangle list plus decoded state -- or, for 3D triangles, hands over the
 * game's vertex bytes for TLVS_SRC to transform and light (gl_draw_tl);
 * this file rasterizes it on the GPU and
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
 * current and loads the entry points (gl_load), then calls gl_ge_init. Built
 * with PSP2I_GLES it runs on OpenGL ES 3.0 (Android) instead: the shaders get
 * a GLSL ES header, and there is no depth clamp (depth is clamped in the
 * vertex shader anyway) and no BGRA upload (the frame texture swizzles). */

#include "gl_load.h"
#include "gl_ge.h"
#include "postfx.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psprecomp/hle.h>
#include <psprecomp/mem.h>
#include <psprecomp/render.h>

/* ---- GL state cache ---------------------------------------------------------------------
 * Every draw sets its whole state -- target, viewport, scissor, blending,
 * depth, program, uniforms, texture, sampler -- about 20 calls, ~450 draws a
 * frame. Most of them repeat what the previous draw set, and each still
 * costs the driver (at 120 fps the driver's thread fell behind: the swap
 * waited ~4.5 ms a frame for it). So the state-setting calls of this file go
 * through this cache and reach GL only when they change something. The
 * cache is this file's view of the context: code elsewhere that sets state
 * on it calls gl_ge_state_reset afterwards. */
static struct {
    int valid;
    int en_scissor, en_blend, en_depth, en_cull;         /* -1 unknown */
    GLint vp[4], sc[4];
    GLboolean cmask[4];
    int dmask;
    GLenum dfunc, bf[4], beq[2];
    GLfloat bcol[4];
    GLuint prog, rfbo, dfbo, vao, abuf, tbuf, pbuf;
    GLenum unit;
    GLuint tex2d[4], texbuf[4], samp[4];
    GLenum front, cullf;
} S;
static uint64_t n_gl_calls, n_gl_skipped;

void gl_ge_state_reset(void) { memset(&S, 0, sizeof S); }
void gl_ge_state_counts(uint64_t *made, uint64_t *skipped) { *made = n_gl_calls; *skipped = n_gl_skipped; }
static void sc_valid(void) {
    if (S.valid) return;
    memset(&S, 0xFF, sizeof S);                           /* every cached value unknown */
    S.valid = 1;
}
#define SC_SAME(cond) do { sc_valid(); if (cond) { n_gl_skipped++; return; } n_gl_calls++; } while (0)

static int *sc_cap(GLenum cap) {
    switch (cap) {
    case GL_SCISSOR_TEST: return &S.en_scissor;
    case GL_BLEND:        return &S.en_blend;
    case GL_DEPTH_TEST:   return &S.en_depth;
    case GL_CULL_FACE:    return &S.en_cull;
    default:              return NULL;
    }
}
static void sc_enable(GLenum cap) {
    sc_valid();
    int *c = sc_cap(cap);
    if (c && *c == 1) { n_gl_skipped++; return; }
    n_gl_calls++;
    if (c) *c = 1;
    p_glEnable(cap);
}
static void sc_disable(GLenum cap) {
    sc_valid();
    int *c = sc_cap(cap);
    if (c && *c == 0) { n_gl_skipped++; return; }
    n_gl_calls++;
    if (c) *c = 0;
    p_glDisable(cap);
}
static void sc_viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    SC_SAME(S.vp[0] == x && S.vp[1] == y && S.vp[2] == w && S.vp[3] == h);
    S.vp[0] = x; S.vp[1] = y; S.vp[2] = w; S.vp[3] = h;
    p_glViewport(x, y, w, h);
}
static void sc_scissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    SC_SAME(S.sc[0] == x && S.sc[1] == y && S.sc[2] == w && S.sc[3] == h);
    S.sc[0] = x; S.sc[1] = y; S.sc[2] = w; S.sc[3] = h;
    p_glScissor(x, y, w, h);
}
static void sc_color_mask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    SC_SAME(S.cmask[0] == r && S.cmask[1] == g && S.cmask[2] == b && S.cmask[3] == a);
    S.cmask[0] = r; S.cmask[1] = g; S.cmask[2] = b; S.cmask[3] = a;
    p_glColorMask(r, g, b, a);
}
static void sc_depth_mask(GLboolean m) { SC_SAME(S.dmask == (int)m); S.dmask = m; p_glDepthMask(m); }
static void sc_depth_func(GLenum f) { SC_SAME(S.dfunc == f); S.dfunc = f; p_glDepthFunc(f); }
static void sc_blend_func(GLenum a, GLenum b, GLenum c, GLenum d) {
    SC_SAME(S.bf[0] == a && S.bf[1] == b && S.bf[2] == c && S.bf[3] == d);
    S.bf[0] = a; S.bf[1] = b; S.bf[2] = c; S.bf[3] = d;
    p_glBlendFuncSeparate(a, b, c, d);
}
static void sc_blend_eq(GLenum a, GLenum b) {
    SC_SAME(S.beq[0] == a && S.beq[1] == b);
    S.beq[0] = a; S.beq[1] = b;
    p_glBlendEquationSeparate(a, b);
}
static void sc_blend_color(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    SC_SAME(!memcmp(S.bcol, (GLfloat[4]){ r, g, b, a }, sizeof S.bcol));
    S.bcol[0] = r; S.bcol[1] = g; S.bcol[2] = b; S.bcol[3] = a;
    p_glBlendColor(r, g, b, a);
}
static void sc_front_face(GLenum m) { SC_SAME(S.front == m); S.front = m; p_glFrontFace(m); }
static void sc_cull_face(GLenum m) { SC_SAME(S.cullf == m); S.cullf = m; p_glCullFace(m); }

/* Uniforms, per program and location (the small locations of the draw
 * programs; others go straight through). */
#define SC_PROGS 16
#define SC_LOCS  64
static GLuint g_sc_prog_id[SC_PROGS];
static uint32_t g_sc_uni[SC_PROGS][SC_LOCS][5];           /* 4 words + 1 = set */
static int sc_prog_slot(GLuint prog) {
    for (int i = 0; i < SC_PROGS; i++) if (g_sc_prog_id[i] == prog) return i;
    for (int i = 0; i < SC_PROGS; i++)
        if (!g_sc_prog_id[i]) { g_sc_prog_id[i] = prog; memset(g_sc_uni[i], 0, sizeof g_sc_uni[i]); return i; }
    return -1;
}
static int g_sc_slot = -1;
static void sc_use_program(GLuint p) {
    SC_SAME(S.prog == p);
    S.prog = p;
    g_sc_slot = p ? sc_prog_slot(p) : -1;
    p_glUseProgram(p);
}
static int sc_uni_same(GLint loc, const uint32_t w[4]) {
    sc_valid();
    if (g_sc_slot < 0 || S.prog == (GLuint)-1 || loc < 0 || loc >= SC_LOCS) return 0;
    uint32_t *c = g_sc_uni[g_sc_slot][loc];
    if (c[4] && !memcmp(c, w, 16)) return 1;
    memcpy(c, w, 16); c[4] = 1;
    return 0;
}
static void sc_uniform4f(GLint loc, GLfloat a, GLfloat b, GLfloat c, GLfloat d) {
    const GLfloat v[4] = { a, b, c, d };
    uint32_t w[4];
    memcpy(w, v, 16);
    if (sc_uni_same(loc, w)) { n_gl_skipped++; return; }
    n_gl_calls++;
    p_glUniform4f(loc, a, b, c, d);
}
static void sc_uniform4ui(GLint loc, GLuint a, GLuint b, GLuint c, GLuint d) {
    const uint32_t w[4] = { a, b, c, d };
    if (sc_uni_same(loc, w)) { n_gl_skipped++; return; }
    n_gl_calls++;
    p_glUniform4ui(loc, a, b, c, d);
}

static void sc_bind_framebuffer(GLenum target, GLuint f) {
    sc_valid();
    const int r = target != GL_DRAW_FRAMEBUFFER, d = target != GL_READ_FRAMEBUFFER;
    if ((!r || S.rfbo == f) && (!d || S.dfbo == f)) { n_gl_skipped++; return; }
    n_gl_calls++;
    if (r) S.rfbo = f;
    if (d) S.dfbo = f;
    p_glBindFramebuffer(target, f);
}
static void sc_bind_vertex_array(GLuint v) { SC_SAME(S.vao == v); S.vao = v; p_glBindVertexArray(v); }
static void sc_bind_buffer(GLenum target, GLuint b) {
    sc_valid();
    GLuint *c = target == GL_ARRAY_BUFFER ? &S.abuf : target == GL_TEXTURE_BUFFER ? &S.tbuf
              : target == GL_PIXEL_PACK_BUFFER ? &S.pbuf : NULL;   /* the element buffer is the VAO's */
    if (c && *c == b) { n_gl_skipped++; return; }
    n_gl_calls++;
    if (c) *c = b;
    p_glBindBuffer(target, b);
}
static void sc_active_texture(GLenum u) { SC_SAME(S.unit == u); S.unit = u; p_glActiveTexture(u); }
static void sc_bind_texture(GLenum target, GLuint t) {
    sc_valid();
    const GLuint u = S.unit == (GLenum)-1 ? 99u : S.unit - GL_TEXTURE0;
    GLuint *c = u < 4 ? (target == GL_TEXTURE_2D ? &S.tex2d[u] : target == GL_TEXTURE_BUFFER ? &S.texbuf[u] : NULL) : NULL;
    if (c && *c == t) { n_gl_skipped++; return; }
    n_gl_calls++;
    if (c) *c = t;
    p_glBindTexture(target, t);
}
static void sc_bind_sampler(GLuint unit, GLuint s) {
    sc_valid();
    if (unit < 4 && S.samp[unit] == s) { n_gl_skipped++; return; }
    n_gl_calls++;
    if (unit < 4) S.samp[unit] = s;
    p_glBindSampler(unit, s);
}

/* Deleting a bound object unbinds it: forget those bindings. */
static void sc_forget(GLuint *slot, GLuint name) { if (*slot == name) *slot = (GLuint)-1; }
static void sc_delete_textures(GLsizei n, const GLuint *t) {
    for (GLsizei i = 0; i < n; i++) for (int u = 0; u < 4; u++) { sc_forget(&S.tex2d[u], t[i]); sc_forget(&S.texbuf[u], t[i]); }
    p_glDeleteTextures(n, t);
}
static void sc_delete_framebuffers(GLsizei n, const GLuint *f) {
    for (GLsizei i = 0; i < n; i++) { sc_forget(&S.rfbo, f[i]); sc_forget(&S.dfbo, f[i]); }
    p_glDeleteFramebuffers(n, f);
}
static void sc_delete_buffers(GLsizei n, const GLuint *b) {
    for (GLsizei i = 0; i < n; i++) { sc_forget(&S.abuf, b[i]); sc_forget(&S.tbuf, b[i]); sc_forget(&S.pbuf, b[i]); }
    p_glDeleteBuffers(n, b);
}

#undef glEnable
#undef glDisable
#undef glViewport
#undef glScissor
#undef glColorMask
#undef glDepthMask
#undef glDepthFunc
#undef glBlendFuncSeparate
#undef glBlendEquationSeparate
#undef glBlendColor
#undef glFrontFace
#undef glCullFace
#undef glUseProgram
#undef glUniform4f
#undef glUniform4ui
#undef glBindFramebuffer
#undef glBindVertexArray
#undef glBindBuffer
#undef glActiveTexture
#undef glBindTexture
#undef glBindSampler
#undef glDeleteTextures
#undef glDeleteFramebuffers
#undef glDeleteBuffers
#define glEnable                sc_enable
#define glDisable               sc_disable
#define glViewport              sc_viewport
#define glScissor               sc_scissor
#define glColorMask             sc_color_mask
#define glDepthMask             sc_depth_mask
#define glDepthFunc             sc_depth_func
#define glBlendFuncSeparate     sc_blend_func
#define glBlendEquationSeparate sc_blend_eq
#define glBlendColor            sc_blend_color
#define glFrontFace             sc_front_face
#define glCullFace              sc_cull_face
#define glUseProgram            sc_use_program
#define glUniform4f             sc_uniform4f
#define glUniform4ui            sc_uniform4ui
#define glBindFramebuffer       sc_bind_framebuffer
#define glBindVertexArray       sc_bind_vertex_array
#define glBindBuffer            sc_bind_buffer
#define glActiveTexture         sc_active_texture
#define glBindTexture           sc_bind_texture
#define glBindSampler           sc_bind_sampler
#define glDeleteTextures        sc_delete_textures
#define glDeleteFramebuffers    sc_delete_framebuffers
#define glDeleteBuffers         sc_delete_buffers

#define VRAM_SIZE 0x200000u
#define MAX_RT    32
#define MAX_DEPTH 8
#define MAX_TEX   512
#define VB_VERTS  (400u * 1024u)                 /* ring of psp_gpu_vertex (16 MB) */

static int    g_ok;
static GLuint g_prog, g_xprog, g_vao, g_xvao, g_vb;
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
    uint64_t wstamp;                             /* changes whenever its pixels may change (gl_ge_present) */
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
    int all = t->dirty && y0 >= t->dy0 && y1 <= t->dy1;   /* already marked? (draw_tl marks the scissor every draw) */
    for (uint32_t y = y0; all && y < y1; y++) all = (t->drow[y >> 6] >> (y & 63)) & 1;
    if (all) return;
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
GLSL_HEADER
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
GLSL_HEADER
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

/* The vertex work of gpu.c's psp_gpu_prim for a transformed triangle draw
 * (render.h psp_gpu_tl): the game's vertex bytes are a buffer texture of
 * 32-bit words (VB); each vertex is decoded by its vertex type (LI[0].z),
 * skinned, transformed, lit (ge_vertex.c psp_ge_light) and given its texture
 * coordinates, then taken through the projection and viewport onto the
 * target: the numbers the CPU path hands gl_draw, as clip-space positions,
 * so GL clips them and interpolates with perspective. Depth is clamped per
 * pixel (GL_DEPTH_CLAMP). */
static const char *TLVS_SRC =
GLSL_HEADER
"uniform vec4 U[81]; uniform ivec4 LI[4]; uniform vec4 vp;\n"
"uniform usamplerBuffer VB;\n"
"out vec2 v_uv; out vec4 v_c;\n"
"uint wd(int a) { return texelFetch(VB, a >> 2).r; }\n"
"uint b8(int a) { return (wd(a) >> uint((a & 3) * 8)) & 0xFFu; }\n"
"uint b16(int a) { return (wd(a) >> uint((a & 2) * 8)) & 0xFFFFu; }\n"
"int csz(int f) { return f == 1 ? 1 : f == 2 ? 2 : f == 3 ? 4 : 0; }\n"
"float cmp(int a, int f, bool sg) {\n"
"  if (f == 1) { uint b = b8(a); return (sg ? float(int(b << 24u) >> 24) : float(b)) * (1.0 / 128.0); }\n"
"  if (f == 2) { uint h = b16(a); return (sg ? float(int(h << 16u) >> 16) : float(h)) * (1.0 / 32768.0); }\n"
"  if (f == 3) return uintBitsToFloat(wd(a));\n"
"  return 0.0;\n"
"}\n"
"vec3 m43(int r, vec3 p) { vec4 q = vec4(p, 1.0); return vec3(dot(U[r], q), dot(U[r + 1], q), dot(U[r + 2], q)); }\n"
"vec3 m33(int r, vec3 p) { return vec3(dot(U[r].xyz, p), dot(U[r + 1].xyz, p), dot(U[r + 2].xyz, p)); }\n"
"float pw(float x, float y) { return x > 0.0 ? pow(x, y) : (y == 0.0 ? 1.0 : 0.0); }\n"
"vec4 light(vec4 color, vec3 pos, vec3 nrm) {\n"
"  int mu = LI[3].y;\n"
"  vec4 amb = (mu & 1) != 0 ? color : U[42];\n"
"  vec3 dif = (mu & 2) != 0 ? color.rgb : U[43].rgb;\n"
"  vec3 spc = (mu & 4) != 0 ? color.rgb : U[44].rgb;\n"
"  vec3 sum = U[41].rgb + U[45].rgb * amb.rgb;\n"
"  float sa = U[45].a * amb.a;\n"
"  vec3 spec = vec3(0.0);\n"
"  float sh = U[46].x;\n"
"  for (int l = 0; l < 4; l++) {\n"
"    int bits = (LI[3].x >> (8 * l)) & 0xFF;\n"
"    if ((bits & 1) == 0) continue;\n"
"    int type = (bits >> 1) & 3, comp = (bits >> 3) & 3, b = 47 + 8 * l;\n"
"    vec3 to = type == 0 ? U[b].xyz : U[b].xyz - pos;\n"
"    float dist = sqrt(dot(to, to)), d = 0.0;\n"
"    if (dist > 0.0) { to /= dist; d = dot(to, nrm); }\n"
"    if (d < 0.0) d = 0.0;\n"
"    if (comp == 2) d = pw(d, sh);\n"
"    float scale = 1.0;\n"
"    if (type != 0) {\n"
"      vec3 at = U[b + 2].xyz;\n"
"      float a = at.x + at.y * dist + at.z * dist * dist;\n"
"      scale = clamp(a > 0.0 ? 1.0 / a : 1.0, 0.0, 1.0);\n"
"      if (type >= 2) {\n"
"        float len = sqrt(dot(U[b + 1].xyz, U[b + 1].xyz));\n"
"        float ang = len > 0.0 ? dot(U[b + 1].xyz, to) / len : 0.0;\n"
"        scale = ang >= U[b + 6].y ? scale * pw(ang, U[b + 6].x) : 0.0;\n"
"      }\n"
"    }\n"
"    sum += (U[b + 3].rgb * amb.rgb + U[b + 4].rgb * dif * d) * scale;\n"
"    if (comp == 1) {\n"
"      vec3 h = vec3(to.x, to.y, to.z + 1.0);\n"
"      float hl = sqrt(dot(h, h)), s = hl > 0.0 ? dot(h, nrm) / hl : 0.0;\n"
"      if (s > 0.0) spec += U[b + 5].rgb * spc * (pw(s, sh) * scale);\n"
"    }\n"
"  }\n"
"  sum += spec;\n"
"  return clamp(vec4(sum, sa), 0.0, 1.0);\n"
"}\n"
"void main() {\n"
"  int base = LI[0].y + gl_VertexID * LI[0].x, vt = LI[0].z, nw = LI[0].w;\n"
"  int ft = vt & 3, fc = (vt >> 2) & 7, fn = (vt >> 5) & 3, fp = (vt >> 7) & 3, fw = (vt >> 9) & 3;\n"
"  int wo = LI[1].x, to = LI[1].y, co = LI[1].z, no = LI[1].w, po = LI[2].x, flags = LI[2].y;\n"
"  vec3 pos = vec3(0.0), nrm = vec3(0.0); vec2 uv = vec2(0.0); vec4 col;\n"
"  if (to >= 0) { uv.x = cmp(base + to, ft, false); uv.y = cmp(base + to + csz(ft), ft, false); }\n"
"  if (co >= 0) {\n"
"    if (fc == 4) { uint h = b16(base + co); uint r = h & 31u, g = (h >> 5) & 63u, bl = (h >> 11) & 31u;\n"
"      col = vec4(float((r << 3) | (r >> 2)), float((g << 2) | (g >> 4)), float((bl << 3) | (bl >> 2)), 255.0); }\n"
"    else if (fc == 5) { uint h = b16(base + co); uint r = h & 31u, g = (h >> 5) & 31u, bl = (h >> 10) & 31u;\n"
"      col = vec4(float((r << 3) | (r >> 2)), float((g << 3) | (g >> 2)), float((bl << 3) | (bl >> 2)), (h & 0x8000u) != 0u ? 255.0 : 0.0); }\n"
"    else if (fc == 6) { uint h = b16(base + co);\n"
"      col = vec4(float(h & 15u), float((h >> 4) & 15u), float((h >> 8) & 15u), float((h >> 12) & 15u)) * 17.0; }\n"
"    else { uint w = wd(base + co); col = vec4(float(w & 255u), float((w >> 8) & 255u), float((w >> 16) & 255u), float(w >> 24)); }\n"
"  } else col = U[42] * 255.0;\n"
"  if (no >= 0) for (int k = 0; k < 3; k++) nrm[k] = cmp(base + no + k * csz(fn), fn, true);\n"
"  for (int k = 0; k < 3; k++) pos[k] = cmp(base + po + k * csz(fp), fp, true);\n"
"  vec3 mp = pos, mn = nrm;\n"
"  if ((flags & 2) != 0) {\n"
"    mp = vec3(0.0); mn = vec3(0.0);\n"
"    for (int i = 0; i < nw; i++) {\n"
"      float w = cmp(base + wo + i * csz(fw), fw, false);\n"
"      if (w == 0.0) continue;\n"
"      mp += w * m43(13 + 3 * i, pos);\n"
"      mn += w * m33(13 + 3 * i, nrm);\n"
"    }\n"
"  }\n"
"  vec3 wp = m43(0, mp), wn = vec3(0.0, 0.0, 1.0);\n"
"  bool lit = (flags & 1) != 0;\n"
"  int mapm = LI[2].z;\n"
"  if (lit || mapm == 2) {\n"
"    if ((flags & 4) != 0) mn = -mn;\n"
"    wn = m33(0, mn);\n"
"    float l = sqrt(dot(wn, wn));\n"
"    if (l > 0.0) wn /= l;\n"
"  }\n"
"  if (lit) col = light(col / 255.0, wp, wn) * 255.0;\n"
"  vec2 t;\n"
"  if (mapm == 1) {\n"
"    int src = LI[2].w;\n"
"    vec3 s3;\n"
"    if (src == 0) s3 = pos;\n"
"    else if (src == 1) s3 = vec3(uv, 0.0);\n"
"    else if (src == 2) { float l = sqrt(dot(nrm, nrm)); s3 = l > 0.0 ? nrm / l : vec3(0.0, 0.0, 1.0); }\n"
"    else s3 = nrm;\n"
"    vec3 q3 = m43(10, s3);\n"
"    float q = q3.z != 0.0 ? q3.z : 1.0;\n"
"    t = q3.xy / q * U[39].xy * U[40].xy;\n"
"  } else if (mapm == 2) {\n"
"    t = vec2(1.0 + dot(U[79].xyz, wn), 1.0 + dot(U[80].xyz, wn)) * 0.5 * U[39].xy * U[40].xy;\n"
"  } else t = (uv * U[39].xy + U[39].zw) * U[40].xy;\n"
"  vec4 q = vec4(m43(3, wp), 1.0);\n"
"  vec4 cl = vec4(dot(U[6], q), dot(U[7], q), dot(U[8], q), dot(U[9], q));\n"
"  vec2 s2 = 2.0 / vp.xy;\n"
"  gl_Position = vec4(cl.x * U[37].x * s2.x + cl.w * (U[38].x * s2.x - 1.0), cl.y * U[37].y * s2.y + cl.w * (U[38].y * s2.y - 1.0), (cl.z * U[37].z + cl.w * U[38].z) * (2.0 / 65535.0) - cl.w, cl.w);\n"
"  v_uv = t * vp.zw; v_c = col / 255.0;\n"
"}\n";

/* One target's bytes as another target's pixels (d3d11.c XFER_SRC). */
static const char *XVS_SRC =
GLSL_HEADER
"void main() {\n"
"  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
"  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"}\n";

static const char *XFS_SRC =
GLSL_HEADER
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
    t->wstamp = ++g_use;
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
    t->last_use = t->wstamp = ++g_use;
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

/* key -> slot index, so a lookup costs the same with 5 or 512 textures
 * cached (the linear scan grew with play time as the cache filled). Open
 * addressing; slots hold index + 1, 0 = empty; rebuilt when an entry is
 * replaced (a miss with the cache full). */
#define TEX_INDEX 2048
static uint16_t g_tex_index[TEX_INDEX];

static uint32_t tex_slot(uint64_t key) { return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 53) & (TEX_INDEX - 1); }

static void tex_index_add(int i) {
    uint32_t s = tex_slot(g_tex[i].key);
    while (g_tex_index[s]) s = (s + 1) & (TEX_INDEX - 1);
    g_tex_index[s] = (uint16_t)(i + 1);
}

static void tex_index_rebuild(void) {
    memset(g_tex_index, 0, sizeof g_tex_index);
    for (int i = 0; i < MAX_TEX; i++) if (g_tex[i].used) tex_index_add(i);
}

static tex_entry *tex_find(uint64_t key) {
    for (uint32_t s = tex_slot(key); g_tex_index[s]; s = (s + 1) & (TEX_INDEX - 1)) {
        tex_entry *t = &g_tex[g_tex_index[s] - 1];
        if (t->used && t->key == key) return t;
    }
    return NULL;
}

/* Texture replacement (gl_ge_set_texture_source): the host's RGBA for a
 * texture's pixel address, at any size -- uploaded once per image and used in
 * place of the GE texture. UVs are in the GE texture's texels (texture_view
 * scales by its size), so a bigger image maps the same. */
static const uint8_t *(*g_tex_source)(uint32_t addr, int *w, int *h);
static unsigned (*g_tex_source_gen)(void);
void gl_ge_set_texture_source(const uint8_t *(*fn)(uint32_t addr, int *w, int *h), unsigned (*gen)(void)) {
    g_tex_source = fn;
    g_tex_source_gen = gen;
}
#define REP_CACHE 1024
static struct { const uint8_t *img; GLuint tex; } g_rep_tex[REP_CACHE];
static unsigned g_rep_seen_gen;
static uint64_t n_tex_replaced;

static tex_entry *tex_replacement(const psp_gpu_state *st) {
    int w = 0, h = 0;
    const uint8_t *img = g_tex_source(st->tex_addr, &w, &h);
    if (!img || w <= 0 || h <= 0) return NULL;
    const unsigned gen = g_tex_source_gen ? g_tex_source_gen() : 0;
    if (gen != g_rep_seen_gen) {                         /* images replaced or moved: upload again */
        for (int i = 0; i < REP_CACHE; i++) { if (g_rep_tex[i].tex) glDeleteTextures(1, &g_rep_tex[i].tex); g_rep_tex[i].tex = 0; g_rep_tex[i].img = NULL; }
        g_rep_seen_gen = gen;
    }
    static tex_entry e;
    int slot = -1;
    for (int i = 0; i < REP_CACHE && slot < 0; i++) if (g_rep_tex[i].img == img) slot = i;
    if (slot < 0) {
        for (int i = 0; i < REP_CACHE && slot < 0; i++) if (!g_rep_tex[i].img) slot = i;
        if (slot < 0) return NULL;
        g_rep_tex[slot].img = img;
        g_rep_tex[slot].tex = new_texture((uint32_t)w, (uint32_t)h);
        glBindTexture(GL_TEXTURE_2D, g_rep_tex[slot].tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, img);
        n_tex_replaced++;
    }
    e.tex = g_rep_tex[slot].tex;
    e.w = (uint32_t)w; e.h = (uint32_t)h;
    return &e;
}

static tex_entry *tex_get(const psp_gpu_state *st) {
    if (g_tex_source) {
        tex_entry *r = tex_replacement(st);
        if (r) return r;
    }
    tex_entry *e = tex_find(st->tex_key);
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
        const int evicted = e->used;
        if (e->used && (e->w != st->tex_w || e->h != st->tex_h)) { glDeleteTextures(1, &e->tex); e->tex = 0; }
        if (!e->tex) e->tex = new_texture(st->tex_w, st->tex_h);
        e->key = st->tex_key; e->w = st->tex_w; e->h = st->tex_h; e->used = 1;
        if (evicted) tex_index_rebuild(); else tex_index_add((int)(e - g_tex));
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
static int g_tex_self;                  /* the last texture_view sampled the target being drawn to */

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
                if (r == target) g_tex_self = 1;
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
                if (t == target) g_tex_self = 1;
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
    t->wstamp = ++g_use;
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

typedef struct { GLuint prog; GLint vp, env, tf, at, misc; } draw_prog;
static draw_prog g_dp, g_tlp;           /* screen-space vertices (VS_SRC); the vertex work on the GPU (TLVS_SRC) */
static GLint  u_tl_u, u_tl_li;
/* The uniforms last sent, in groups that change independently (render.h
 * PSP_TL_U_*): only a changed group is uploaded. At ~450 draws a frame,
 * sending all 81 vec4s each time was the driver's largest cost. */
static const int TL_GROUPS[][2] = { { 0, 13 }, { 13, 24 }, { 37, 10 }, { 47, 34 } };   /* first, count */
static GLint u_tl_group[4];
static float g_tl_sent_u[PSP_TL_NU * 4];
static GLint g_tl_sent_i[PSP_TL_NI * 4];
static int g_tl_sent;
static GLuint g_tlvao, g_tlvb, g_tlib, g_tltex;
static uint32_t g_tlvb_pos, g_tlib_pos;  /* bytes */
static uint64_t n_tl_draws;
static uint32_t g_tlvb_bytes = 16u << 20;   /* ring of game vertex bytes (at most the buffer-texture limit) */
#define TLIB_BYTES (8u << 20)             /* ring of 32-bit indices */

/* The target a draw goes to, made current with VRAM and with the targets it
 * overlaps; its depth buffer and texture. NULL: not drawable here. */
static rt_entry *draw_target(const psp_gpu_state *st, depth_entry **dp, GLuint *texp, float uvs[2]) {
    if (!g_ok || (st->fb_addr & 0x0F000000u) != 0x04000000u || !st->fb_stride) return NULL;
    const uint32_t off = vram_off(st->fb_addr);
    uint32_t rows = (uint32_t)st->sy1 + 1;
    if (rows > 512) rows = 512;
    rt_entry *t = rt_get(off, st->fb_stride, st->fb_fmt, rows);
    if (!t) return NULL;
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
    uvs[0] = uvs[1] = 1.0f;
    if (st->tex) {
        tex = texture_view(st, t, uvs);
        if (!tex) return NULL;
    }
    *dp = d;
    *texp = tex;
    return t;
}

/* Target, viewport, scissor, blending, depth, and the program with the
 * fragment stage's state. */
static void draw_state(const psp_gpu_state *st, rt_entry *t, depth_entry *d, GLuint tex, const draw_prog *p,
                       const float uvs[2]) {
    bind_target(t, d);
    glViewport(0, 0, (GLsizei)t->w, (GLsizei)t->h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(st->sx0, st->sy0, st->sx1 - st->sx0 + 1, st->sy1 - st->sy0 + 1);
    int approx = 0;
    apply_blend(st, &approx);
    if (approx) n_approx++;
    apply_depth(st, d != NULL);

    glUseProgram(p->prog);
    glUniform4f(p->vp, (float)t->w, (float)t->h, uvs[0], uvs[1]);
    glUniform4f(p->env, (float)(st->env & 0xFF) / 255.0f, (float)((st->env >> 8) & 0xFF) / 255.0f,
                (float)((st->env >> 16) & 0xFF) / 255.0f, 0.0f);
    glUniform4ui(p->tf, (GLuint)(st->tex != 0), (GLuint)st->tfunc, (GLuint)st->trgba, (GLuint)st->tdbl);
    glUniform4ui(p->at, (GLuint)(st->atest && !st->clear), (GLuint)st->afunc, st->aref, st->amask);
    glUniform4ui(p->misc, (GLuint)st->clear, 0, 0, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, tex ? sampler(st->tex_linear, st->tex_clamp_u, st->tex_clamp_v) : 0);
}

/* Rows a draw may have written: the scissor, narrowed to [ymin, ymax]. */
static void draw_rows(rt_entry *t, const psp_gpu_state *st, float ymin, float ymax) {
    int y0 = (int)ymin - 1, y1 = (int)ymax + 2;
    if (y0 < st->sy0) y0 = st->sy0;
    if (y1 > st->sy1 + 1) y1 = st->sy1 + 1;
    if (y0 < y1) rows_mark(t, (uint32_t)y0, (uint32_t)y1);
}

static void tl_flush(void);
static int gl_draw(const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts) {
    tl_flush();
    if (nverts <= 0 || (uint32_t)nverts > VB_VERTS) return 1;
    depth_entry *d;
    GLuint tex;
    float uvs[2];
    rt_entry *t = draw_target(st, &d, &tex, uvs);
    if (!t) return 1;

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
#ifdef PSP2I_GLES
    /* Mobile drivers (Mali, Adreno) treat a sub-data upload into a buffer the
     * GPU may still read as a hazard: they copy the whole buffer or wait. The
     * ring never rewrites a region before the wrap orphans it, so an
     * unsynchronised map is safe and costs only the copy. */
    {
        const GLsizeiptr n = (GLsizeiptr)nverts * (GLsizeiptr)sizeof(psp_gpu_vertex);
        void *dst = glMapBufferRange(GL_ARRAY_BUFFER, (GLintptr)g_vb_pos * (GLintptr)sizeof(psp_gpu_vertex), n,
                                     GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
        if (dst) {
            memcpy(dst, v, (size_t)n);
            glUnmapBuffer(GL_ARRAY_BUFFER);
        } else {
            glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)g_vb_pos * (GLintptr)sizeof(psp_gpu_vertex), n, v);
        }
    }
#else
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)g_vb_pos * (GLintptr)sizeof(psp_gpu_vertex),
                    (GLsizeiptr)nverts * (GLsizeiptr)sizeof(psp_gpu_vertex), v);
#endif
    first = (GLint)g_vb_pos;
    g_vb_pos += (uint32_t)nverts;

    draw_state(st, t, d, tex, &g_dp, uvs);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, first, nverts);

    float ymin = 1e9f, ymax = -1e9f;
    for (int i = 0; i < nverts; i++) { if (v[i].y < ymin) ymin = v[i].y; if (v[i].y > ymax) ymax = v[i].y; }
    draw_rows(t, st, ymin, ymax);
    n_draws++;
    n_tris += (uint64_t)nverts / 3;
    return 0;
}

/* A transformed triangle draw with the vertex work in TLVS_SRC (render.h
 * psp_gpu_tl): the game's vertex bytes and the triangle list into their
 * rings, the GE state as uniforms, culling by GL. Where the triangles land
 * is not known here, so the whole scissor counts as written.
 *
 * Draws are merged, as PPSSPP's draw engine does: a draw whose state,
 * matrices, lights and culling equal the previous draw's is appended to it
 * -- its vertex bytes after the previous ones (placed a whole number of
 * vertices on, so one base address serves both), its indices moved by that
 * many vertices -- and the batch becomes one GL draw when something differs
 * or something is about to look at the targets (tl_flush). GL draws a draw's
 * triangles in order, so the pixels are the same; what goes is the per-draw
 * cost (target and texture lookups, uploads, ~20 GL calls): the shops, with
 * the lobby in view, made ~520 draws of ~57 triangles a frame. A draw that
 * samples the target it draws into (texture_view's scratch copy) is never
 * merged: the next one must see what it drew. */
#define TLB_VBYTES (4u << 20)
#define TLB_IDX    (1u << 20)
static uint8_t  g_tlb_v[TLB_VBYTES];
static uint32_t g_tlb_i[TLB_IDX];
static struct {
    int on, self, cull;
    psp_gpu_state st;
    float u[PSP_TL_NU * 4];
    int32_t i[PSP_TL_NI * 4];            /* with the misalignment (LAYOUT0.y) zeroed */
    rt_entry *t;
    depth_entry *d;
    GLuint tex;
    float uvs[2];
    uint32_t base, stride, vend;         /* staging: the byte vertex 0 is at; vertex size; bytes used */
    int nidx;
} B;
static uint64_t n_tl_gl_draws, n_tl_merged;

static void tl_flush(void) {
    if (!B.on) return;
    B.on = 0;
    const uint32_t vb = (B.vend + 3u) & ~3u, ib = (uint32_t)B.nidx * 4u;
    /* Sub-data uploads into rings, orphaned on wrap (as gl_draw's vertices). */
    glBindBuffer(GL_TEXTURE_BUFFER, g_tlvb);
    if (g_tlvb_pos + vb > g_tlvb_bytes) {
        glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)g_tlvb_bytes, NULL, GL_STREAM_DRAW);
        g_tlvb_pos = 0;
    }
    glBufferSubData(GL_TEXTURE_BUFFER, (GLintptr)g_tlvb_pos, (GLsizeiptr)vb, g_tlb_v);
    const uint32_t vfirst = g_tlvb_pos;
    g_tlvb_pos += vb;
    glBindVertexArray(g_tlvao);                     /* holds the index buffer binding */
    if (g_tlib_pos + ib > TLIB_BYTES) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)TLIB_BYTES, NULL, GL_STREAM_DRAW);
        g_tlib_pos = 0;
    }
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)g_tlib_pos, (GLsizeiptr)ib, g_tlb_i);
    const uint32_t ifirst = g_tlib_pos;
    g_tlib_pos += ib;

    draw_state(&B.st, B.t, B.d, B.tex, &g_tlp, B.uvs);
    GLint li[PSP_TL_NI * 4];
    memcpy(li, B.i, sizeof li);
    li[4 * PSP_TL_I_LAYOUT0 + 1] = (GLint)(vfirst + B.base);   /* where vertex 0 is in the ring */
    for (int k = 0; k < 4; k++) {
        const int first = TL_GROUPS[k][0], n = TL_GROUPS[k][1];
        const size_t bytes = (size_t)n * 4 * sizeof(float);
        if (g_tl_sent && !memcmp(g_tl_sent_u + 4 * first, B.u + 4 * first, bytes)) continue;
        glUniform4fv(u_tl_group[k], n, B.u + 4 * first);
        memcpy(g_tl_sent_u + 4 * first, B.u + 4 * first, bytes);
    }
    if (!g_tl_sent || memcmp(g_tl_sent_i, li, sizeof li)) {
        glUniform4iv(u_tl_li, PSP_TL_NI, li);
        memcpy(g_tl_sent_i, li, sizeof li);
    }
    g_tl_sent = 1;
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_BUFFER, g_tltex);
    glActiveTexture(GL_TEXTURE0);
    /* GL's window y is PSP y here (no flip), so GL's counter-clockwise --
     * positive signed area -- is render.h's cull 1. */
    if (B.cull) {
        glEnable(GL_CULL_FACE);
        glFrontFace(GL_CCW);
        glCullFace(B.cull == 1 ? GL_FRONT : GL_BACK);
    }
    glDrawElements(GL_TRIANGLES, B.nidx, GL_UNSIGNED_INT, (const void *)(uintptr_t)ifirst);
    if (B.cull) glDisable(GL_CULL_FACE);
    n_tl_gl_draws++;
}

/* Where the next draw's vertex bytes go in the batch so that its vertex 0 is
 * vertex *k of the batch: a word-aligned byte offset past what is there. */
static int tl_place(uint32_t misalign, uint32_t vbytes, uint32_t *off, uint32_t *k) {
    const uint32_t s = B.stride;
    uint32_t kk = (B.vend + misalign - B.base + s - 1) / s;
    for (int t = 0; t < 4; t++, kk++) {
        const uint32_t o = B.base + kk * s - misalign;
        if ((o & 3u) == 0 && o >= B.vend && o + vbytes <= TLB_VBYTES) { *off = o; *k = kk; return 1; }
    }
    return 0;
}

static int gl_draw_tl(const psp_gpu_state *st, const psp_gpu_tl *tl) {
    if (!g_tlp.prog || tl->nidx <= 0) return 1;
    const uint32_t vb = tl->vbytes, ib = (uint32_t)tl->nidx * 4u;
    if (vb > g_tlvb_bytes || vb > TLB_VBYTES || ib > TLIB_BYTES || (uint32_t)tl->nidx > TLB_IDX) return 1;
    const uint32_t misalign = (uint32_t)tl->i[4 * PSP_TL_I_LAYOUT0 + 1], stride = (uint32_t)tl->i[4 * PSP_TL_I_LAYOUT0 + 0];
    if (B.on) {
        int same = !B.self && B.cull == tl->cull && B.stride == stride && stride &&
                   (uint32_t)(B.nidx + tl->nidx) <= TLB_IDX && !memcmp(&B.st, st, sizeof *st) &&
                   !memcmp(B.u, tl->u, sizeof B.u);
        if (same) {
            int32_t ci[PSP_TL_NI * 4];
            memcpy(ci, tl->i, sizeof ci);
            ci[4 * PSP_TL_I_LAYOUT0 + 1] = 0;
            same = !memcmp(B.i, ci, sizeof ci);
        }
        uint32_t off, k;
        if (same && tl_place(misalign, vb, &off, &k)) {
            memcpy(g_tlb_v + off, tl->vdata, vb);
            B.vend = off + vb;
            for (int j = 0; j < tl->nidx; j++) g_tlb_i[B.nidx + j] = tl->idx[j] + k;
            B.nidx += tl->nidx;
            n_draws++;
            n_tl_draws++;
            n_tl_merged++;
            n_tris += (uint64_t)tl->nidx / 3;
            return 0;
        }
        tl_flush();
    }
    depth_entry *d;
    GLuint tex;
    float uvs[2];
    g_tex_self = 0;
    rt_entry *t = draw_target(st, &d, &tex, uvs);
    if (!t) return 1;
    B.on = 1;
    B.self = g_tex_self;
    B.cull = tl->cull;
    B.st = *st;
    memcpy(B.u, tl->u, sizeof B.u);
    memcpy(B.i, tl->i, sizeof B.i);
    B.i[4 * PSP_TL_I_LAYOUT0 + 1] = 0;
    B.t = t; B.d = d; B.tex = tex; B.uvs[0] = uvs[0]; B.uvs[1] = uvs[1];
    memcpy(g_tlb_v, tl->vdata, vb);
    B.base = misalign; B.stride = stride; B.vend = vb;
    memcpy(g_tlb_i, tl->idx, ib);
    B.nidx = tl->nidx;
    draw_rows(t, st, (float)st->sy0, (float)st->sy1);
    n_draws++;
    n_tl_draws++;
    n_tris += (uint64_t)tl->nidx / 3;
    return 0;
}

static void gl_sync_vram(uint32_t addr, uint32_t bytes) {
    tl_flush();
    if ((addr & 0x0F000000u) != 0x04000000u) return;
    uint32_t a0 = vram_off(addr), a1 = a0 + bytes;
    for (int i = 0; i < MAX_RT; i++)
        if (g_rt[i].used && g_rt[i].dirty && overlaps(a0, a1, g_rt[i].off, rt_end(&g_rt[i])))
            { g_rb_reason = 5; rt_readback(&g_rt[i]); }
}

static void gl_vram_written(uint32_t addr, uint32_t bytes) {
    tl_flush();
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
    tl_flush();
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

/* With the GE thread (psp_ge_thread_start) the targets live there: a game
 * access to VRAM first waits for the GE to finish what it was handed -- what
 * it would have seen without the thread -- and then, if it touches a target's
 * pages, has the readback done there. The GE's own VRAM reads (textures) are
 * on that thread already. */
static void cpu_access_job(void *p) { const uint32_t *a = (const uint32_t *)p; gl_cpu_access(a[0], a[1]); }
static uint64_t n_cpu_access_calls, n_vram_waits, vram_wait_us;
static struct { uint32_t ra; uint64_t n, us; } g_vw[8];

/* For the perf line: game VRAM accesses that waited for the GE thread
 * (count, us) and the 3 call sites that waited longest, "ra:ms" each. */
void gl_ge_vram_waits(uint64_t *n, uint64_t *us, char *top, size_t cap) {
    *n = n_vram_waits; *us = vram_wait_us;
    top[0] = 0;
    int used[8] = { 0 };
    size_t len = 0;
    for (int r = 0; r < 3; r++) {
        int b = -1;
        for (int k = 0; k < 8; k++) if (g_vw[k].ra && !used[k] && (b < 0 || g_vw[k].us > g_vw[b].us)) b = k;
        if (b < 0) break;
        used[b] = 1;
        len += (size_t)snprintf(top + len, len < cap ? cap - len : 0, "%s%08X:%.0f", r ? " " : "", g_vw[b].ra, g_vw[b].us / 1000.0);
        if (len >= cap) break;
    }
}
static void gl_vram_hook(uint32_t off, uint32_t size) {
    if (!psp_ge_threaded() || psp_ge_on_thread()) { gl_cpu_access(off, size); return; }
    {   /* how long game accesses to VRAM wait for the GE thread, and from where (perf line) */
        const uint64_t t0 = psp_sched_now_us();
        psp_ge_wait_idle();
        const uint64_t dt = psp_sched_now_us() - t0;
        if (dt >= 20) {
            const uint32_t ra = psp_cpu.r[31];
            n_vram_waits++; vram_wait_us += dt;
            int k = 0;
            while (k < 8 && g_vw[k].ra && g_vw[k].ra != ra) k++;
            if (k == 8) { k = 0; for (int j = 1; j < 8; j++) if (g_vw[j].us < g_vw[k].us) k = j; g_vw[k].ra = ra; g_vw[k].n = 0; g_vw[k].us = 0; }
            g_vw[k].ra = ra; g_vw[k].n++; g_vw[k].us += dt;
        }
    }
    const uint32_t a1 = off + size > VRAM_SIZE ? VRAM_SIZE : off + size;
    if (off >= a1) return;
    int hit = 0;
    for (uint32_t p = off >> PG_SHIFT; p <= (a1 - 1) >> PG_SHIFT; p++) hit |= g_pg_dirty[p] | g_pg_armed[p];
    if (!hit) return;
    uint32_t a[2] = { off, size };
    n_cpu_access_calls++;
    psp_ge_call(cpu_access_job, a, 1);
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
static uint64_t n_present_async, n_present_vram, n_present_kept, n_present_direct;

static void fence_drop(int k) {
    if (g_fence[k]) { glDeleteSync(g_fence[k]); g_fence[k] = NULL; }
}

static const rt_entry *g_q_t;                  /* the target and its stamp at the last copy queued */
static uint64_t g_q_stamp;

int gl_ge_present(uint32_t addr, uint32_t stride, int fmt, uint32_t *out, int w, int h) {
    tl_flush();
    if (!g_ok || (addr & 0x0F000000u) != 0x04000000u) return 0;
    rt_entry *t = rt_find(vram_off(addr), stride, fmt);
    const int ahead = t && t->dirty && t->stride == stride && t->fmt == fmt && t->w >= (uint32_t)w;
    if (!ahead) {
        for (int k = 0; k < PRESENT_RING; k++) { g_present_pending[k] = 0; fence_drop(k); }
        g_q_t = NULL;
        n_present_vram++;
        return 0;
    }
    /* Queue a copy only when the target changed since the last one queued:
     * presenting runs every vblank (120 a second at 120 fps) and the game
     * draws a new frame into the other buffer, so most vblanks have nothing
     * new to copy -- the copies already queued are still collected below. */
    if (t != g_q_t || t->wstamp != g_q_stamp) {
        g_q_t = t;
        g_q_stamp = t->wstamp;
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
static GLint u_bscale, u_bp0, u_bp1, u_bts;
static int g_btex_w, g_btex_h;

static const char *BVS_SRC =
GLSL_HEADER
"uniform vec4 S; out vec2 v_uv;\n"                   /* S.xy: the frame's part of the texture */
"void main() {\n"
"  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
"  v_uv = vec2(p.x, 1.0 - p.y) * S.xy;\n"             /* frame row 0 at the top of the window */
"  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"}\n";
/* With the post-processing (postfx.h, as present.c): P0 = (scaling, fxaa,
 * sharpen, scanlines), P1 = (brightness, saturation, window pixels per frame
 * pixel), TS = the texture's size in texels. P0 = 0, P1 = (1, 1, 1) is the
 * plain picture (overlays). */
static const char *BFS_SRC =
GLSL_HEADER
"uniform sampler2D F; uniform vec4 P0; uniform vec4 P1; uniform vec4 TSv;\n"
"in vec2 v_uv; out vec4 o_c;\n"
"const vec3 L = vec3(0.299, 0.587, 0.114);\n"
"vec3 tap(vec2 uv) { return texture(F, uv).rgb; }\n"
"vec3 frame(vec2 uv) {\n"
"  if (P0.x > 1.5) {\n"
"    vec2 x = uv * TSv.xy, i = floor(x), f = x - i;\n"
"    f = clamp((f - 0.5) * P1.z + 0.5, 0.0, 1.0);\n"
"    return tap((i + f) / TSv.xy);\n"
"  }\n"
"  return tap(uv);\n"
"}\n"
"vec3 fxaa(vec2 uv, vec2 px) {\n"
"  vec3 nw = tap(uv + vec2(-1.0, -1.0) * px), ne = tap(uv + vec2(1.0, -1.0) * px);\n"
"  vec3 sw = tap(uv + vec2(-1.0, 1.0) * px), se = tap(uv + vec2(1.0, 1.0) * px), m = frame(uv);\n"
"  float lnw = dot(nw, L), lne = dot(ne, L), lsw = dot(sw, L), lse = dot(se, L), lm = dot(m, L);\n"
"  float lmin = min(lm, min(min(lnw, lne), min(lsw, lse))), lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));\n"
"  vec2 d = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));\n"
"  float red = max((lnw + lne + lsw + lse) * 0.03125, 1.0 / 128.0);\n"
"  d = clamp(d / (min(abs(d.x), abs(d.y)) + red), -8.0, 8.0) * px;\n"
"  vec3 a = 0.5 * (tap(uv + d * (1.0 / 3.0 - 0.5)) + tap(uv + d * (2.0 / 3.0 - 0.5)));\n"
"  vec3 b = a * 0.5 + 0.25 * (tap(uv - d * 0.5) + tap(uv + d * 0.5));\n"
"  float lb = dot(b, L);\n"
"  return (lb < lmin || lb > lmax) ? a : b;\n"
"}\n"
"void main() {\n"
"  vec2 px = 1.0 / TSv.xy;\n"
"  vec3 c = P0.y > 0.5 ? fxaa(v_uv, px) : frame(v_uv);\n"
"  if (P0.z > 0.0) {\n"
"    vec3 n = tap(v_uv + vec2(0.0, -px.y)) + tap(v_uv + vec2(0.0, px.y)) + tap(v_uv + vec2(-px.x, 0.0)) + tap(v_uv + vec2(px.x, 0.0));\n"
"    c = clamp(c + P0.z * (c * 4.0 - n) * 0.25, 0.0, 1.0);\n"
"  }\n"
"  if (P0.w > 0.0) {\n"
"    float y = fract(v_uv.y * TSv.xy.y);\n"
"    c *= 1.0 - P0.w * (1.0 - 4.0 * y * (1.0 - y));\n"
"  }\n"
"  c *= P1.x;\n"
"  c = mix(vec3(dot(c, L)), c, P1.y);\n"
"  o_c = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
"}\n";

/* The plain picture: no post-processing (overlays drawn with this program). */
static void blit_plain(void) {
    glUniform4f(u_bp0, 0.0f, 0.0f, 0.0f, 0.0f);
    glUniform4f(u_bp1, 1.0f, 1.0f, 1.0f, 0.0f);
    glUniform4f(u_bts, 1.0f, 1.0f, 0.0f, 0.0f);
}

static int blit_ready(void) {
    if (!g_ok) return 0;
    if (!g_bprog) {
        g_bprog = gl_program("present", BVS_SRC, BFS_SRC);
        if (!g_bprog) return 0;
        u_bscale = glGetUniformLocation(g_bprog, "S");
        u_bp0 = glGetUniformLocation(g_bprog, "P0");
        u_bp1 = glGetUniformLocation(g_bprog, "P1");
        u_bts = glGetUniformLocation(g_bprog, "TSv");
    }
    return 1;
}

/* The texture bound to unit 0 (tw x th texels), its top-left sx x sy part --
 * a frame fw pixels wide -- into the window, post-processed (postfx.h). */
static void blit_draw(float sx, float sy, int tw, int th, int fw, int x, int y, int vw, int vh, int cw, int ch) {
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
    glUniform4f(u_bscale, sx, sy, 0.0f, 0.0f);
    float p0[4], p1[4];
    int linear = 0;
    postfx_params(fw, vw, p0, p1, &linear);
    glUniform4f(u_bp0, p0[0], p0[1], p0[2], p0[3]);
    glUniform4f(u_bp1, p1[0], p1[1], p1[2], p1[3]);
    glUniform4f(u_bts, (float)tw, (float)th, 0.0f, 0.0f);
    glBindSampler(0, sampler(linear, 1, 1));
    glBindVertexArray(g_xvao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* The displayed frame straight from its render target, when the GPU's copy
 * is the current one: no readback, no CPU conversion, no upload (on a phone
 * that round trip cost more than the frame itself). 0: the CPU's copy is the
 * current one (e.g. a video); use gl_ge_present + gl_blit_frame. */
int gl_ge_present_direct(uint32_t addr, uint32_t stride, int fmt, int w, int h,
                         int x, int y, int vw, int vh, int cw, int ch) {
    tl_flush();
    if (!blit_ready() || (addr & 0x0F000000u) != 0x04000000u) return 0;
    rt_entry *t = rt_find(vram_off(addr), stride, fmt);
    if (!t || !t->dirty || t->w < (uint32_t)w || t->h < (uint32_t)h) return 0;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    blit_draw((float)w / (float)t->w, (float)h / (float)t->h, (int)t->w, (int)t->h, w, x, y, vw, vh, cw, ch);
    n_present_direct++;
    return 1;
}

/* A small 0xAARRGGBB image drawn over the window at x, y (top-left origin),
 * scaled to vw x vh, without clearing (the FPS counter). */
int gl_draw_overlay(const uint32_t *px, int w, int h, int x, int y, int vw, int vh, int cw, int ch) {
    tl_flush();
    static GLuint tex;
    static int tw, th;
    if (!blit_ready()) return -1;
    if (!tex || tw != w || th != h) {
        if (tex) glDeleteTextures(1, &tex);
        tex = new_texture((uint32_t)w, (uint32_t)h);
        tw = w; th = h;
        glBindTexture(GL_TEXTURE_2D, tex);               /* bytes B, G, R, A read as R, G, B, A */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glColorMask(1, 1, 1, 1);
    glViewport(x, ch - y - vh, vw, vh);
    (void)cw;
    glUseProgram(g_bprog);
    glUniform4f(u_bscale, 1.0f, 1.0f, 0.0f, 0.0f);
    blit_plain();
    glBindSampler(0, sampler(0, 1, 1));
    glBindVertexArray(g_xvao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    return 0;
}

int gl_blit_frame(const uint32_t *px, int w, int h, int x, int y, int vw, int vh, int cw, int ch) {
    tl_flush();
    if (!blit_ready()) return -1;
    if (!g_btex || g_btex_w != w || g_btex_h != h) {
        if (g_btex) glDeleteTextures(1, &g_btex);
        g_btex = new_texture((uint32_t)w, (uint32_t)h);
        g_btex_w = w; g_btex_h = h;
#ifdef PSP2I_GLES
        glBindTexture(GL_TEXTURE_2D, g_btex);            /* bytes B, G, R, A read as R, G, B, A */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
#endif
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_btex);
#ifdef PSP2I_GLES
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
#else
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, px);   /* 0xAARRGGBB */
#endif
    blit_draw(1.0f, 1.0f, w, h, w, x, y, vw, vh, cw, ch);
    return 0;
}

/* ---- setup ------------------------------------------------------------------------------------------- */

static psp_gpu_backend GL_BACKEND = { "gl", gl_draw, gl_sync_vram, gl_vram_written, 1, NULL };

static void prog_uniforms(draw_prog *p) {
    p->vp = glGetUniformLocation(p->prog, "vp");
    p->env = glGetUniformLocation(p->prog, "env");
    p->tf = glGetUniformLocation(p->prog, "tf");
    p->at = glGetUniformLocation(p->prog, "at");
    p->misc = glGetUniformLocation(p->prog, "misc");
    glUseProgram(p->prog);
    glUniform1i(glGetUniformLocation(p->prog, "T"), 0);
}

/* The vertex work on the GPU (gl_draw_tl), when the context has what it
 * needs: buffer textures and the optional entry points (gl_load.h). */
static void tl_init(void) {
#ifndef PSP2I_GLES
    if (!p_glTexBuffer || !p_glUniform4fv || !p_glUniform4iv || !p_glDrawElements || !p_glCullFace || !p_glFrontFace) {
        fprintf(stderr, "gl: no buffer textures; the vertex work stays on the CPU\n");
        return;
    }
    g_tlp.prog = gl_program("ge vertex work", TLVS_SRC, FS_SRC);
    if (!g_tlp.prog) { fprintf(stderr, "gl: the vertex work stays on the CPU\n"); return; }
    prog_uniforms(&g_tlp);
    u_tl_u = glGetUniformLocation(g_tlp.prog, "U");
    for (int k = 0; k < 4; k++) {
        char name[16];
        snprintf(name, sizeof name, "U[%d]", TL_GROUPS[k][0]);
        u_tl_group[k] = glGetUniformLocation(g_tlp.prog, name);
    }
    u_tl_li = glGetUniformLocation(g_tlp.prog, "LI");
    glUniform1i(glGetUniformLocation(g_tlp.prog, "VB"), 2);
    glUseProgram(0);
    {   /* GL guarantees only 65536 texels in a buffer texture */
        GLint texels = 0;
        glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &texels);
        if ((uint64_t)texels * 4u < g_tlvb_bytes) g_tlvb_bytes = (uint32_t)texels * 4u;
        if (g_tlvb_bytes < (1u << 20)) {
            fprintf(stderr, "gl: buffer textures of %d texels only; the vertex work stays on the CPU\n", (int)texels);
            g_tlp.prog = 0;
            return;
        }
    }
    glGenBuffers(1, &g_tlvb);
    glBindBuffer(GL_TEXTURE_BUFFER, g_tlvb);
    glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)g_tlvb_bytes, NULL, GL_STREAM_DRAW);
    glGenTextures(1, &g_tltex);
    glBindTexture(GL_TEXTURE_BUFFER, g_tltex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, g_tlvb);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
    glGenVertexArrays(1, &g_tlvao);
    glBindVertexArray(g_tlvao);
    glGenBuffers(1, &g_tlib);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_tlib);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)TLIB_BYTES, NULL, GL_STREAM_DRAW);
    glBindVertexArray(0);
    if (glGetError() != GL_NO_ERROR) { fprintf(stderr, "gl: vertex work setup failed; it stays on the CPU\n"); g_tlp.prog = 0; return; }
    GL_BACKEND.draw_tl = gl_draw_tl;
    fprintf(stderr, "gl: the vertex work of 3D draws runs on the GPU\n");
#endif
}

int gl_ge_init(void) {
    g_prog = gl_program("ge", VS_SRC, FS_SRC);
    if (!g_prog) return -1;
    g_dp.prog = g_prog;
    prog_uniforms(&g_dp);
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
#ifndef PSP2I_GLES
    glEnable(GL_DEPTH_CLAMP);                             /* as DepthClipEnable = FALSE */
#endif
    glDepthRange(0.0, 1.0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (glGetError() != GL_NO_ERROR) { fprintf(stderr, "gl: setup failed\n"); return -1; }
    tl_init();
    g_ok = 1;
    psp_gpu_set_backend(&GL_BACKEND);
    psp_mem_set_vram_hook(gl_vram_hook);
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
    fprintf(out, "  gl vertex work      %llu draws transformed and lit on the GPU%s\n", (unsigned long long)n_tl_draws,
            g_tlp.prog ? "" : " (unavailable: on the CPU)");
    fprintf(out, "  gl merged draws     %llu merged into the draw before; %llu GL draws for them\n",
            (unsigned long long)n_tl_merged, (unsigned long long)n_tl_gl_draws);
    fprintf(out, "  gl readbacks by cause: relayout %llu, evict %llu, grow %llu, texture %llu, overlap %llu, sync %llu, cpu access %llu\n",
            (unsigned long long)n_rb_reason[0], (unsigned long long)n_rb_reason[1], (unsigned long long)n_rb_reason[2],
            (unsigned long long)n_rb_reason[3], (unsigned long long)n_rb_reason[4], (unsigned long long)n_rb_reason[5],
            (unsigned long long)n_rb_reason[6]);
    if (psp_ge_threaded())
        fprintf(out, "  gl on the GE thread %llu game accesses to VRAM handed to it\n", (unsigned long long)n_cpu_access_calls);
    fprintf(out, "  gl state calls      %llu made, %llu skipped as unchanged\n", (unsigned long long)n_gl_calls, (unsigned long long)n_gl_skipped);
    fprintf(out, "  gl transfers        %llu GPU-side target-to-target transfers\n", (unsigned long long)n_xfer);
    fprintf(out, "  gl present          %llu frames from asynchronous copies, %llu from VRAM, %llu vblanks kept the last picture\n",
            (unsigned long long)n_present_async, (unsigned long long)n_present_vram, (unsigned long long)n_present_kept);
    fprintf(out, "  gl present direct   %llu vblanks drawn straight from the render target\n",
            (unsigned long long)n_present_direct);
}
