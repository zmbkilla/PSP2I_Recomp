/* Model extraction (engine menu EXTRACT MODELS, PC builds): every model the
 * game has loaded, written as Data/<group>/<name>.glb next to the exe, with
 * its skeleton, skin weights, materials and textures. A file that exists is
 * left alone.
 *
 * Where things are (found in a running game; port/testwork has the notes):
 *
 *   - Names. The game loads files from packages inside file.fpb; its package
 *     loader creates one resource per file at 0x08D57B48(package, i). The
 *     package holds a directory at +5C (20 bytes a file: resource, offset,
 *     type tag "UNJ\0" / "UNT\0" / "UVR\0" ..., size, name offset in the low
 *     16 bits of +10) and the names at +60. The hook below keeps name, type
 *     and resource for every file; a resource's +0C is its data, a model's
 *     +10 its object.
 *
 *   - Models are Sega's Ninja format for the PSP (.unj): the object (centre,
 *     radius, materials, vertex lists, nodes, sub-objects with mesh sets),
 *     with pointers already absolute in memory. Nodes are 0x90 bytes and hold
 *     their inverse bind matrix (column-major, as glTF's). A vertex list holds
 *     the GE vertex type, its strip lengths, the vertices, a bone table (for
 *     skinned lists: matrix-palette indices) and the position scale. A mesh
 *     set names its node, material and vertex list. A material is a block of
 *     GE commands (UV scale and offset at 0x48-0x4B) ending in RET, followed
 *     by its texture index.
 *
 *   - Textures are .uvr files: GBIX + UVRT (colour format -- 0 ARGB1555,
 *     1 RGB565, 2 ARGB4444, 3 ARGB8888, as Dreamcast PVR -- data format (4 / 8
 *     bit palette or direct), size, palette, pixels in the PSP's swizzled
 *     layout). The model's texture list
 *     (NUTL, in its .unt file) names them.
 *
 *   - Groups come from the name's prefix: map (sn_ sa_ st_ ...), object
 *     (ob_), creature (en_), npc (np_), player (pl_), weapon (wp_), effect
 *     (eff_), else other. */

#include "modelx.h"

#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/hle.h>
#include <psprecomp/mem.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#define mkdir_one(p) _mkdir(p)
#define file_exists(p) (_access(p, 0) == 0)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_one(p) mkdir(p, 0777)
#define file_exists(p) (access(p, 0) == 0)
#endif

/* A file's size and modification time as one number (0 = missing): a GLB read
 * earlier is used again while it is unchanged. */
static long long file_stamp(const char *path) {
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
#endif
    return (long long)st.st_mtime * 1000003LL + (long long)st.st_size + 1;
}

#define TAG_UNJ 0x004A4E55u
#define TAG_UNT 0x00544E55u

static double now_ms(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

/* ---- names, as packages load ------------------------------------------------ */

typedef struct { uint32_t res, tag, pkg; char name[48]; } rec;
static rec *g_rec;
static int g_nrec, g_caprec;

static void read_str(uint32_t a, char *out, int cap) {
    int n = 0;
    for (; n < cap - 1; n++) {
        const uint8_t c = psp_read8(a + (uint32_t)n);
        if (!c || c < 32 || c > 126) break;
        out[n] = (char)c;
    }
    out[n] = 0;
}

static int g_scan_due;                     /* files loaded: replaced textures may have moved */

static void hook_package_file(void (*original)(void)) {
    const uint32_t pkg = psp_cpu.r[4], idx = psp_cpu.r[5];
    original();
    const uint32_t e = psp_read32(pkg + 0x5C) + idx * 20u;
    const uint32_t res = psp_read32(e), tag = psp_read32(e + 8);
    if (!res) return;
    rec r;
    r.res = res; r.tag = tag; r.pkg = pkg;
    read_str(psp_read32(pkg + 0x60) + (psp_read32(e + 16) & 0xFFFFu), r.name, sizeof r.name);
    if (!r.name[0]) return;
    g_scan_due = 1;
    for (int i = 0; i < g_nrec; i++)            /* a resource address used again: the newer file */
        if (g_rec[i].res == res) { g_rec[i] = r; return; }
    if (g_nrec == g_caprec) {
        const int cap = g_caprec ? g_caprec * 2 : 4096;
        rec *p = (rec *)realloc(g_rec, (size_t)cap * sizeof *p);
        if (!p) return;
        g_rec = p; g_caprec = cap;
    }
    g_rec[g_nrec++] = r;
}

void modelx_init(void) { psp_hook_set(0x08D57B48u, hook_package_file); }

/* A recorded resource that is still the same file: its data, else 0. */
static uint32_t rec_data(const rec *r) {
    if (!psp_mem_ptr(r->res, 0x14)) return 0;
    const uint32_t t = psp_read32(r->res + 4);
    if (((t >> 8) & 0xFFFFu) != ((r->tag >> 8) & 0xFFFFu)) return 0;
    const uint32_t data = psp_read32(r->res + 0x0C);
    return psp_mem_ptr(data, 16) ? data : 0;
}

static const rec *find_rec(const char *name, uint32_t tag) {
    for (int i = g_nrec - 1; i >= 0; i--)
        if ((!tag || g_rec[i].tag == tag) && !strcmp(g_rec[i].name, name) && rec_data(&g_rec[i])) return &g_rec[i];
    return NULL;
}

/* ---- a growing byte buffer ------------------------------------------------- */

typedef struct { uint8_t *p; size_t n, cap; } buf;
static int bput(buf *b, const void *src, size_t n) {
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + n) cap *= 2;
        uint8_t *p = (uint8_t *)realloc(b->p, cap);
        if (!p) return -1;
        b->p = p; b->cap = cap;
    }
    if (src) memcpy(b->p + b->n, src, n); else memset(b->p + b->n, 0, n);
    b->n += n;
    return 0;
}
static void balign(buf *b, size_t a, uint8_t fill) { while (b->n % a) bput(b, &fill, 1); }
static int bprintf(buf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    return n > 0 ? bput(b, tmp, (size_t)(n < (int)sizeof tmp ? n : (int)sizeof tmp - 1)) : 0;
}

/* ---- PNG (stored deflate: no compression library) -------------------------- */

static uint32_t crc_table[256];
static uint32_t crc32_of(const uint8_t *p, size_t n, uint32_t c) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}
static void be32(buf *b, uint32_t v) { const uint8_t x[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; bput(b, x, 4); }
static void png_chunk(buf *b, const char *type, const uint8_t *data, size_t n) {
    be32(b, (uint32_t)n);
    const size_t at = b->n;
    bput(b, type, 4);
    bput(b, data, n);
    be32(b, crc32_of(b->p + at, n + 4, 0));
}
static void png_rgba(buf *out, const uint8_t *rgba, int w, int h) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    bput(out, sig, 8);
    uint8_t ihdr[13] = { 0 };
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16); ihdr[2] = (uint8_t)(w >> 8); ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16); ihdr[6] = (uint8_t)(h >> 8); ihdr[7] = (uint8_t)h;
    ihdr[8] = 8; ihdr[9] = 6;
    png_chunk(out, "IHDR", ihdr, 13);
    buf raw = { 0 }, z = { 0 };
    for (int y = 0; y < h; y++) { const uint8_t f = 0; bput(&raw, &f, 1); bput(&raw, rgba + (size_t)y * w * 4, (size_t)w * 4); }
    const uint8_t zh[2] = { 0x78, 0x01 };
    bput(&z, zh, 2);
    uint32_t a = 1, s = 0;
    for (size_t i = 0; i < raw.n; i++) { a = (a + raw.p[i]) % 65521; s = (s + a) % 65521; }
    for (size_t at = 0; at < raw.n || at == 0; ) {
        const size_t n = raw.n - at > 65535 ? 65535 : raw.n - at;
        const uint8_t hd[5] = { (uint8_t)(at + n >= raw.n), (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)~n, (uint8_t)(~n >> 8) };
        bput(&z, hd, 5);
        bput(&z, raw.p + at, n);
        at += n;
        if (!n) break;
    }
    be32(&z, (s << 16) | a);
    png_chunk(out, "IDAT", z.p, z.n);
    png_chunk(out, "IEND", NULL, 0);
    free(raw.p); free(z.p);
}

/* ---- .uvr textures ---------------------------------------------------------- */

static void colour16(uint16_t v, int f, uint8_t *o) {
    switch (f) {
    case 0:  o[0] = (uint8_t)((v & 31) * 255 / 31); o[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63); o[2] = (uint8_t)(((v >> 11) & 31) * 255 / 31); o[3] = 255; break;
    case 1:  o[0] = (uint8_t)((v & 31) * 255 / 31); o[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31); o[2] = (uint8_t)(((v >> 10) & 31) * 255 / 31); o[3] = v & 0x8000 ? 255 : 0; break;
    default: o[0] = (uint8_t)((v & 15) * 17); o[1] = (uint8_t)(((v >> 4) & 15) * 17); o[2] = (uint8_t)(((v >> 8) & 15) * 17); o[3] = (uint8_t)((v >> 12) * 17); break;
    }
}

/* The UVRT chunk at `a` as RGBA (malloc'd), or NULL. */
static uint8_t *decode_uvr(uint32_t a, int *pw, int *ph) {
    for (int k = 0; k < 64 && psp_read32(a) != 0x54525655u; k += 4) a += 4;   /* past GBIX */
    if (psp_read32(a) != 0x54525655u) return NULL;
    const int f0 = psp_read8(a + 8), f1 = psp_read8(a + 9);
    const int w = psp_read16(a + 12), h = psp_read16(a + 14);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || (w & 3) || (h & 7)) return NULL;
    const int lo = f1 & 0x7F;
    const int bpp = lo == 6 || lo == 8 ? 4 : lo == 10 || lo == 12 ? 8 : f0 == 3 ? 32 : 16;
    const int psize = f0 == 3 ? 4 : 2;
    const int cf = f0 == 0 ? 1 : f0 == 1 ? 0 : 2;            /* UVR (as Dreamcast PVR): 0 ARGB1555, 1 RGB565, 2 ARGB4444 */
    uint32_t p = a + 16;
    uint8_t pal[256][4];
    if (bpp <= 8) {
        const int n = bpp == 4 ? 16 : 256;
        for (int i = 0; i < n; i++) {
            if (psize == 4) { for (int c = 0; c < 4; c++) pal[i][c] = psp_read8(p + (uint32_t)(i * 4 + c)); }
            else colour16(psp_read16(p + (uint32_t)i * 2), cf, pal[i]);
        }
        p += (uint32_t)(n * psize);
    }
    const int rowb = w * bpp / 8;
    if (rowb % 16) return NULL;
    const uint8_t *src = (const uint8_t *)psp_mem_ptr(p, (uint32_t)(rowb * h));
    if (!src) return NULL;
    uint8_t *lin = (uint8_t *)malloc((size_t)rowb * h), *rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!lin || !rgba) { free(lin); free(rgba); return NULL; }
    for (int by = 0, i = 0; by < h / 8; by++)            /* unswizzle: 16-byte x 8-row blocks */
        for (int bx = 0; bx < rowb / 16; bx++)
            for (int r = 0; r < 8; r++, i += 16) memcpy(lin + (size_t)(by * 8 + r) * rowb + bx * 16, src + i, 16);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *o = rgba + ((size_t)y * w + x) * 4;
            const uint8_t *row = lin + (size_t)y * rowb;
            if (bpp == 4) memcpy(o, pal[(row[x / 2] >> (4 * (x & 1))) & 15], 4);
            else if (bpp == 8) memcpy(o, pal[row[x]], 4);
            else if (bpp == 16) colour16((uint16_t)(row[2 * x] | row[2 * x + 1] << 8), cf, o);
            else memcpy(o, row + 4 * x, 4);
        }
    free(lin);
    *pw = w; *ph = h;
    return rgba;
}

/* ---- the texture registry ----------------------------------------------------
 * Textures do not come through the package loader: the game's texture
 * registry keeps, per texture, its name (32 bytes) followed by a pointer to
 * the texture pack it came in and the file's offset in that pack. One pass
 * over RAM per extraction finds those entries: a "*.uvr" name whose pack +
 * offset is a GBIX / UVRT header. */
typedef struct { char name[32]; uint32_t data, at; } texent;
static texent *g_tex;
static int g_ntex;

static int texent_cmp(const void *a, const void *b) { return strcmp(((const texent *)a)->name, ((const texent *)b)->name); }

/* Every registry entry in RAM, sorted by name; the count. Only reads guest RAM:
 * the decode worker runs it while the game goes on (what it finds is checked). */
static int scan_textures(texent **out) {
    texent *g_tex = NULL;                                             /* (shadows the index) */
    int g_ntex = 0, cap = 0;
    *out = NULL;
    const uint8_t *ram = (const uint8_t *)psp_mem_ptr(PSP_RAM_BASE, PSP_RAM_SIZE);
    if (!ram) return 0;
    const uint8_t *end = ram + PSP_RAM_SIZE - 0x28, *q = ram + 32;
    while (q < end && (q = (const uint8_t *)memchr(q, '.', (size_t)(end - q))) != NULL) {
        const uint8_t *dot = q++;
        if (dot[1] != 'u' || dot[2] != 'v' || dot[3] != 'r' || dot[4] != 0) continue;
        /* the entry's name starts on a 4-byte boundary at most 31 bytes before its end */
        uint32_t o = (uint32_t)(dot + 4 - ram);
        o = o >= 31 ? (o - 31 + 3) & ~3u : 0;
        for (; o + 5 <= (uint32_t)(dot + 4 - ram); o += 4) {
            int printable = 1;
            for (const uint8_t *c = ram + o; c < dot && printable; c++) printable = *c >= 32 && *c < 127;
            if (!printable || (dot + 4 - ram) - (int64_t)o < 5) continue;
            uint32_t data, off;                                          /* the texture pack, the file's offset in it */
            memcpy(&data, ram + o + 32, 4);
            memcpy(&off, ram + o + 36, 4);
            if (data < PSP_RAM_BASE || off >= 0x400000u) continue;
            data += off;
            if (data >= PSP_RAM_BASE + PSP_RAM_SIZE - 32) continue;
            const uint32_t t = psp_read32(data);
            if (t != 0x58494247u && t != 0x54525655u) continue;          /* GBIX, UVRT */
            if (g_ntex == cap) {
                cap = cap ? cap * 2 : 512;
                texent *p = (texent *)realloc(g_tex, (size_t)cap * sizeof *p);
                if (!p) { *out = g_tex; return g_ntex; }
                g_tex = p;
            }
            memcpy(g_tex[g_ntex].name, ram + o, 32);
            g_tex[g_ntex].name[31] = 0;
            g_tex[g_ntex].data = data;
            g_tex[g_ntex].at = PSP_RAM_BASE + o;
            g_ntex++;
            break;
        }
    }
    if (g_ntex) qsort(g_tex, (size_t)g_ntex, sizeof *g_tex, texent_cmp);
    *out = g_tex;
    return g_ntex;
}

static void index_textures(void) {
    free(g_tex);
    g_ntex = scan_textures(&g_tex);
}

static const texent *texture_entry(const char *name) {
    texent k;
    snprintf(k.name, sizeof k.name, "%s", name);
    return g_ntex ? (const texent *)bsearch(&k, g_tex, (size_t)g_ntex, sizeof *g_tex, texent_cmp) : NULL;
}

static uint32_t texture_data(const char *name) {
    const texent *e = texture_entry(name);
    return e ? e->data : 0;
}

/* ---- the Ninja object -------------------------------------------------------- */

#define MAXN 256
#define MAXTEX 512                 /* textures in one model's list (the lobby has 157) */
typedef struct {
    int parent, imatrix;
    float inv[16], world[16], local[16];
} node;

static void mat_mul(const float *a, const float *b, float *o) {   /* column-major: o = a * b */
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + rr] * b[c * 4 + k];
            r[c * 4 + rr] = s;
        }
    memcpy(o, r, sizeof r);
}
static int mat_inv(const float *m, float *o) {
    float inv[16];
    inv[0] = m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
    const float det = m[0]*inv[0]+m[1]*inv[4]+m[2]*inv[8]+m[3]*inv[12];
    if (fabsf(det) < 1e-12f) return -1;
    for (int i = 0; i < 16; i++) o[i] = inv[i] / det;
    return 0;
}
static const float IDENT[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

static float rf(uint32_t a) { return psp_read_f32(a); }
static int ok_ptr(uint32_t a, uint32_t n) { return a && psp_mem_ptr(a, n) != NULL; }

/* GE vertex type -> component layout (offsets, sizes). */
typedef struct { int wt, nw, tc, col, nrm, pos, ow, otc, ocol, onrm, opos, size; } vlayout;
static int csize(int f) { return f == 1 ? 1 : f == 2 ? 2 : f == 3 ? 4 : 0; }
static void layout_of(uint32_t vt, vlayout *L) {
    memset(L, 0, sizeof *L);
    L->tc = vt & 3; L->col = (vt >> 2) & 7; L->nrm = (vt >> 5) & 3; L->pos = (vt >> 7) & 3; L->wt = (vt >> 9) & 3;
    L->nw = L->wt ? (int)((vt >> 14) & 7) + 1 : 0;
    int off = 0, al = 1, s;
#define ALIGN(n) do { off = (off + (n) - 1) / (n) * (n); if ((n) > al) al = (n); } while (0)
    if (L->wt) { s = csize(L->wt); ALIGN(s); L->ow = off; off += s * L->nw; }
    if (L->tc) { s = csize(L->tc); ALIGN(s); L->otc = off; off += s * 2; }
    if (L->col) { s = L->col == 7 ? 4 : 2; ALIGN(s); L->ocol = off; off += s; }
    if (L->nrm) { s = csize(L->nrm); ALIGN(s); L->onrm = off; off += s * 3; }
    if (L->pos) { s = csize(L->pos); ALIGN(s); L->opos = off; off += s * 3; }
    ALIGN(al);
#undef ALIGN
    L->size = off;
}
static float comp(uint32_t a, int f, int sgn) {
    if (f == 1) return sgn ? (float)(int8_t)psp_read8(a) / 128.0f : (float)psp_read8(a) / 128.0f;
    if (f == 2) return sgn ? (float)(int16_t)psp_read16(a) / 32768.0f : (float)psp_read16(a) / 32768.0f;
    return psp_read_f32(a);
}

/* ---- GLB writing ------------------------------------------------------------- */

typedef struct {
    buf json, bin;
    int nacc, nview;
} gltf;

/* `data` as a buffer view (target 1: vertices, 2: indices, 0: none); its index. */
static int add_view(gltf *g, const void *data, size_t bytes, int target) {
    balign(&g->bin, 4, 0);
    const size_t at = g->bin.n;
    bput(&g->bin, data, bytes);
    bprintf(&g->json, "%s{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu%s}", g->nview ? "," : "", at, bytes,
            target ? (target == 1 ? ",\"target\":34962" : ",\"target\":34963") : "");
    return g->nview++;
}

/* ---- one model --------------------------------------------------------------- */

static const char *group_of(const char *n) {
    static const struct { const char *pre, *grp; } G[] = {
        { "sn_", "map" }, { "sa_", "map" }, { "st_", "map" }, { "mp_", "map" }, { "map", "map" }, { "fl_", "map" },
        { "ob_", "object" }, { "en_", "creature" }, { "np_", "npc" }, { "npc", "npc" }, { "pl_", "player" },
        { "wp_", "weapon" }, { "eff_", "effect" }, { "ef_", "effect" },
    };
    for (size_t i = 0; i < sizeof G / sizeof G[0]; i++)
        if (!strncmp(n, G[i].pre, strlen(G[i].pre))) return G[i].grp;
    return "other";
}

typedef struct {
    float *pos, *nrm, *uv, *col;
    uint16_t *joints;
    float *wts;
    uint32_t *idx;
    int nv, ni, has_nrm, has_uv, has_col, skinned, node, tex;
} prim;

static void prim_free(prim *p) { free(p->pos); free(p->nrm); free(p->uv); free(p->col); free(p->joints); free(p->wts); free(p->idx); }

/* GE float24 command argument -> float. */
static float f24(uint32_t arg) { uint32_t b = arg << 8; float f; memcpy(&f, &b, 4); return f; }

/* A material: its UV transform and texture index (-1 = none). */
/* A material's GE block: UV transform, texture index (-1 = none) and how the
 * texture's alpha is used. alpha: 0 = ignored (TCC off: opaque), 1 = alpha
 * test with `cutoff` (0..1), 2 = blended. */
typedef struct { float uvs[4]; int tex, alpha; float cutoff; } matinfo;

static void material_of(uint32_t m, matinfo *mi) {
    mi->uvs[0] = mi->uvs[1] = 1.0f; mi->uvs[2] = mi->uvs[3] = 0.0f;
    mi->tex = -1; mi->alpha = 0; mi->cutoff = 0.5f;
    if (!ok_ptr(m, 0x200)) return;
    int first = 1, tcc = 0, atest = 0, blend = 0;   /* a two-layer material has two blocks: the first layer's */
    uint32_t aref = 0;
    for (uint32_t k = 0x0C; k < 0x200; k += 4) {
        const uint32_t w = psp_read32(m + k), cmd = w >> 24, arg = w & 0xFFFFFF;
        if (first) {
            if (cmd >= 0x48 && cmd <= 0x4B) {
                float v = f24(arg);
                if (cmd <= 0x49 && v == 0.0f) v = 1.0f;          /* a scale of 0: not set (the game sets it) */
                mi->uvs[cmd - 0x48] = v;
            } else if (cmd == 0x21) blend = arg & 1;
            else if (cmd == 0x22) atest = arg & 1;
            else if (cmd == 0xDB) aref = (arg >> 8) & 0xFF;
            else if (cmd == 0xC9) tcc = (arg >> 8) & 1;          /* texture function: RGBA (1) or RGB */
        }
        if (w == 0x0B000000u) {                                  /* RET: the block's end */
            const uint32_t t = psp_read32(m + k + 4);
            if (t < MAXTEX || t == 0xFFFFFFFFu) {
                mi->tex = t < MAXTEX ? (int)t : -1;
                break;
            }
            first = 0;                                           /* another block follows */
        }
    }
    if (tcc) {
        mi->alpha = atest ? 1 : blend ? 2 : 0;
        mi->cutoff = atest ? ((float)aref + 0.5f) / 255.0f : 0.5f;
    }
}

/* Decode vertex list `vl` (strip) into p, in the space of node `nodeidx` (or skinned). */
static int decode_vtxlist(uint32_t vl, const node *nodes, int nnode, float uvs[4], prim *p) {
    if (!ok_ptr(vl, 0x30)) return -1;
    const uint32_t vt = psp_read32(vl + 4), counts = psp_read32(vl + 0x10), data = psp_read32(vl + 0x14);
    const uint32_t bones = psp_read32(vl + 0x18), nbones = psp_read32(vl + 0x1C), nv = psp_read32(vl + 0x20);
    const float scale = rf(vl + 0x2C);
    if (vt & (7u << 18) || (vt >> 23) & 1 || ((vt >> 11) & 3)) return -1;   /* morphs, through mode, indexed: not here */
    vlayout L;
    layout_of(vt, &L);
    if (!L.pos || !nv || nv > 65536 || !ok_ptr(data, nv * (uint32_t)L.size)) return -1;
    p->nv = (int)nv;
    p->pos = (float *)calloc(nv * 3, sizeof(float));
    p->has_nrm = L.nrm != 0; p->has_uv = L.tc != 0; p->has_col = L.col != 0;
    p->skinned = L.wt != 0 && nbones > 0 && nbones <= 8 && ok_ptr(bones, nbones * 4);
    if (p->has_nrm) p->nrm = (float *)calloc(nv * 3, sizeof(float));
    if (p->has_uv) p->uv = (float *)calloc(nv * 2, sizeof(float));
    if (p->has_col) p->col = (float *)calloc(nv * 4, sizeof(float));
    if (p->skinned) { p->joints = (uint16_t *)calloc(nv * 4, 2); p->wts = (float *)calloc(nv * 4, sizeof(float)); }
    int palette_node[8];
    for (uint32_t b = 0; p->skinned && b < nbones; b++) {
        const int m = (int)psp_read32(bones + b * 4);
        palette_node[b] = 0;
        for (int n = 0; n < nnode; n++) if (nodes[n].imatrix == m) { palette_node[b] = n; break; }
    }
    const float ps = (L.pos == 3 || scale == 0.0f) ? 1.0f : scale;
    for (uint32_t i = 0; i < nv; i++) {
        const uint32_t v = data + i * (uint32_t)L.size;
        for (int k = 0; k < 3; k++) p->pos[i * 3 + k] = comp(v + L.opos + (uint32_t)(k * csize(L.pos)), L.pos, 1) * ps;
        if (p->has_nrm) {
            float n[3], l = 0;
            for (int k = 0; k < 3; k++) { n[k] = comp(v + L.onrm + (uint32_t)(k * csize(L.nrm)), L.nrm, 1); l += n[k] * n[k]; }
            l = l > 0 ? 1.0f / sqrtf(l) : 0;
            for (int k = 0; k < 3; k++) p->nrm[i * 3 + k] = n[k] * l;
            if (l == 0) p->nrm[i * 3 + 1] = 1.0f;
        }
        if (p->has_uv)
            for (int k = 0; k < 2; k++) p->uv[i * 2 + k] = comp(v + L.otc + (uint32_t)(k * csize(L.tc)), L.tc, 0) * uvs[k] + uvs[2 + k];
        if (p->has_col) {
            float *c = p->col + i * 4;
            if (L.col == 7) { for (int k = 0; k < 4; k++) c[k] = psp_read8(v + L.ocol + (uint32_t)k) / 255.0f; }
            else { uint8_t o[4]; colour16(psp_read16(v + L.ocol), L.col == 4 ? 0 : L.col == 5 ? 1 : 2, o); for (int k = 0; k < 4; k++) c[k] = o[k] / 255.0f; }
        }
        if (p->skinned) {
            float s = 0;
            for (int k = 0; k < 4; k++) {
                const float wv = k < L.nw && k < (int)nbones ? comp(v + L.ow + (uint32_t)(k * csize(L.wt)), L.wt, 0) : 0.0f;
                p->wts[i * 4 + k] = wv;
                p->joints[i * 4 + k] = (uint16_t)(k < (int)nbones ? palette_node[k] : 0);
                s += wv;
            }
            if (s > 0) for (int k = 0; k < 4; k++) p->wts[i * 4 + k] /= s;
            else p->wts[i * 4] = 1.0f;
        }
    }
    /* the strips -> triangles */
    uint32_t total = 0;
    p->idx = (uint32_t *)malloc(sizeof(uint32_t) * nv * 3);
    for (int s = 0; s < 64 && total < nv && ok_ptr(counts + (uint32_t)s * 4, 4); s++) {
        const uint32_t n = psp_read32(counts + (uint32_t)s * 4);
        if (!n || total + n > nv) break;
        for (uint32_t k = 2; k < n; k++) {
            const uint32_t a = total + k - 2, b = total + k - 1, c = total + k;
            if (a == b || b == c || a == c) continue;
            const float *A = p->pos + a * 3, *B = p->pos + b * 3, *C = p->pos + c * 3;
            if (!memcmp(A, B, 12) || !memcmp(B, C, 12) || !memcmp(A, C, 12)) continue;   /* joins */
            if (k & 1) { p->idx[p->ni++] = b; p->idx[p->ni++] = a; p->idx[p->ni++] = c; }
            else { p->idx[p->ni++] = a; p->idx[p->ni++] = b; p->idx[p->ni++] = c; }
        }
        total += n;
    }
    return p->ni ? 0 : -1;
}

static void minmax3(const float *v, int n, float *mn, float *mx) {
    for (int k = 0; k < 3; k++) { mn[k] = 1e30f; mx[k] = -1e30f; }
    for (int i = 0; i < n; i++) for (int k = 0; k < 3; k++) { if (v[i * 3 + k] < mn[k]) mn[k] = v[i * 3 + k]; if (v[i * 3 + k] > mx[k]) mx[k] = v[i * 3 + k]; }
}

/* Texture names of the model's list (NUTL). */
static int texture_names(uint32_t unt, char names[][48], int max) {
    for (uint32_t k = 0; k < 0x100 && psp_read32(unt) != 0x4C54554Eu; k += 4) unt += 4;   /* "NUTL" */
    if (psp_read32(unt) != 0x4C54554Eu) return 0;
    const uint32_t list = unt + psp_read32(unt + 8);
    const uint32_t n = psp_read32(list), e = psp_read32(list + 4);
    if (!ok_ptr(e, 4)) return 0;
    int c = 0;
    for (uint32_t i = 0; i < n && c < max; i++, c++) read_str(psp_read32(e + i * 0x14 + 4), names[c], 48);
    return c;
}

/* The names in model `r`'s texture list (its .unt, else its package's). */
static int model_textures(const rec *r, char names[][48], int max) {
    char stem[48];
    snprintf(stem, sizeof stem, "%s", r->name);
    char *dot = strrchr(stem, '.');
    if (dot) strcpy(dot, ".unt");
    const rec *t = find_rec(stem, TAG_UNT);
    if (!t)
        for (int i = g_nrec - 1; i >= 0 && !t; i--)
            if (g_rec[i].pkg == r->pkg && g_rec[i].tag == TAG_UNT && rec_data(&g_rec[i])) t = &g_rec[i];
    return t ? texture_names(rec_data(t), names, max) : 0;
}

/* The whole model -> GLB. 1 written, 0 nothing to write, -1 failed. */
static int export_model(const rec *r, uint32_t obj, const char *path) {
    if (!ok_ptr(obj, 0x48)) return 0;
    const int nmat = (int)psp_read32(obj + 0x10), nvl = (int)psp_read32(obj + 0x18), nnode = (int)psp_read32(obj + 0x28);
    const uint32_t mats = psp_read32(obj + 0x14), vls = psp_read32(obj + 0x1C), nodesp = psp_read32(obj + 0x30);
    const int nsub = (int)psp_read32(obj + 0x38);
    const uint32_t subs = psp_read32(obj + 0x3C);
    if (nmat < 0 || nmat > 4096 || nvl <= 0 || nvl > 8192 || nnode <= 0 || nnode > MAXN || nsub <= 0 || nsub > 16) return 0;
    if (!ok_ptr(nodesp, (uint32_t)nnode * 0x90) || !ok_ptr(vls, (uint32_t)nvl * 8) || !ok_ptr(subs, (uint32_t)nsub * 0x14)) return 0;

    /* nodes: bind matrices from the inverse ones */
    node *nodes = (node *)calloc((size_t)nnode, sizeof *nodes);
    for (int i = 0; i < nnode; i++) {
        const uint32_t a = nodesp + (uint32_t)i * 0x90;
        nodes[i].imatrix = (int16_t)psp_read16(a + 4);
        nodes[i].parent = (int16_t)psp_read16(a + 6);
        for (int k = 0; k < 16; k++) nodes[i].inv[k] = rf(a + 0x30 + (uint32_t)k * 4);
        if (mat_inv(nodes[i].inv, nodes[i].world) != 0) { memcpy(nodes[i].world, IDENT, sizeof IDENT); memcpy(nodes[i].inv, IDENT, sizeof IDENT); }
    }
    for (int i = 0; i < nnode; i++) {
        const int pa = nodes[i].parent;
        if (pa >= 0 && pa < nnode) mat_mul(nodes[pa].inv, nodes[i].world, nodes[i].local);
        else memcpy(nodes[i].local, nodes[i].world, sizeof IDENT);
    }

    /* textures */
    static char tnames[MAXTEX][48];
    const int ntex = model_textures(r, tnames, MAXTEX);

    gltf g = { 0 };
    buf meshes = { 0 }, mats_json = { 0 }, accs = { 0 }, images = { 0 }, texs = { 0 };
    int nmesh = 0, nmatj = 0, nimg = 0, skinned_any = 0;
    static int tex_image[MAXTEX];
    for (int i = 0; i < MAXTEX; i++) tex_image[i] = -2;
    static int matkey[1024];
    int nmatkey = 0;
    static float matcut[1024];
    int *mesh_node = (int *)malloc(sizeof(int) * 4096), *mesh_skin = (int *)malloc(sizeof(int) * 4096);

    for (int s = 0; s < nsub; s++) {
        const uint32_t so = subs + (uint32_t)s * 0x14;
        const int nms = (int)psp_read32(so + 4);
        const uint32_t msl = psp_read32(so + 8);
        if (nms <= 0 || nms > 4096 || !ok_ptr(msl, (uint32_t)nms * 0x24)) continue;
        for (int m = 0; m < nms && nmesh < 4096; m++) {
            const uint32_t ms = msl + (uint32_t)m * 0x24;
            const int inode = (int)psp_read32(ms + 16), imat = (int)psp_read32(ms + 24), ivl = (int)psp_read32(ms + 28);
            if (ivl < 0 || ivl >= nvl) continue;
            matinfo mi;
            material_of(imat >= 0 && imat < nmat ? psp_read32(mats + (uint32_t)imat * 8 + 4) : 0, &mi);
            int ti = mi.tex;
            /* the material's index is into the model's texture list (NUTL) itself; the
             * sub-object's list only names the textures it uses (NPC 038: [0 2 3 4]) */
            if (ti < 0 && ntex == 1) ti = 0;                         /* no index, one texture: that one */
            prim p;
            memset(&p, 0, sizeof p);
            if (decode_vtxlist(psp_read32(vls + (uint32_t)ivl * 8 + 4), nodes, nnode, mi.uvs, &p) != 0) { prim_free(&p); continue; }
            /* the texture as an image, once */
            int img = -1;
            if (ti >= 0 && ti < ntex) {
                if (tex_image[ti] == -2) {
                    tex_image[ti] = -1;
                    const uint32_t td = texture_data(tnames[ti]);
                    int w, h;
                    uint8_t *rgba = td ? decode_uvr(td, &w, &h) : NULL;
                    if (rgba) {
                        buf png = { 0 };
                        png_rgba(&png, rgba, w, h);
                        free(rgba);
                        balign(&g.bin, 4, 0);
                        const size_t at = g.bin.n;
                        bput(&g.bin, png.p, png.n);
                        bprintf(&g.json, "%s{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}", g.nview ? "," : "", at, png.n);
                        bprintf(&images, "%s{\"bufferView\":%d,\"mimeType\":\"image/png\",\"name\":\"%s\"}", nimg ? "," : "", g.nview++, tnames[ti]);
                        bprintf(&texs, "%s{\"source\":%d,\"sampler\":0}", nimg ? "," : "", nimg);
                        tex_image[ti] = nimg++;
                        free(png.p);
                    }
                }
                img = tex_image[ti];
            }
            /* one glTF material per game material (its index in extras: LOAD MODELS
             * maps a primitive back to it), named after its texture */
            const int key = imat;
            int matj = -1;
            for (int q = 0; q < nmatkey; q++) if (matkey[q] == key) matj = q;
            if (matj < 0 && nmatkey < 1024) {
                static const char *const MODE[3] = { "OPAQUE", "MASK", "BLEND" };
                if (img >= 0) {
                    bprintf(&mats_json, "%s{\"name\":\"%s\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":%d},\"metallicFactor\":0,\"roughnessFactor\":1},\"alphaMode\":\"%s\"",
                            nmatj ? "," : "", tnames[ti], img, MODE[mi.alpha]);
                    if (mi.alpha == 1) bprintf(&mats_json, ",\"alphaCutoff\":%.3f", (double)mi.cutoff);
                } else
                    bprintf(&mats_json, "%s{\"name\":\"untextured\",\"pbrMetallicRoughness\":{\"metallicFactor\":0,\"roughnessFactor\":1}", nmatj ? "," : "");
                bprintf(&mats_json, ",\"doubleSided\":true,\"extras\":{\"psp_material\":%d}}", imat);
                matkey[nmatkey] = key; matcut[nmatkey] = mi.cutoff;
                matj = nmatkey++;
                nmatj++;
            }
            if (matj < 0) matj = 0;

            /* the primitive's data */
            float mn[3], mx[3];
            minmax3(p.pos, p.nv, mn, mx);
            const int vpos = add_view(&g, p.pos, (size_t)p.nv * 12, 1);
            const int vnrm = p.has_nrm ? add_view(&g, p.nrm, (size_t)p.nv * 12, 1) : -1;
            const int vuv = p.has_uv ? add_view(&g, p.uv, (size_t)p.nv * 8, 1) : -1;
            const int vcol = p.has_col ? add_view(&g, p.col, (size_t)p.nv * 16, 1) : -1;
            const int vjn = p.skinned ? add_view(&g, p.joints, (size_t)p.nv * 8, 1) : -1;
            const int vwt = p.skinned ? add_view(&g, p.wts, (size_t)p.nv * 16, 1) : -1;
            const int vix = add_view(&g, p.idx, (size_t)p.ni * 4, 2);
            /* accessors: written into a separate list, numbered as the views (one each) */
            bprintf(&meshes, "%s{\"primitives\":[{\"attributes\":{\"POSITION\":%d", nmesh ? "," : "", vpos);
            if (vnrm >= 0) bprintf(&meshes, ",\"NORMAL\":%d", vnrm);
            if (vuv >= 0) bprintf(&meshes, ",\"TEXCOORD_0\":%d", vuv);
            if (vcol >= 0) bprintf(&meshes, ",\"COLOR_0\":%d", vcol);
            if (vjn >= 0) bprintf(&meshes, ",\"JOINTS_0\":%d,\"WEIGHTS_0\":%d", vjn, vwt);
            bprintf(&meshes, "},\"indices\":%d,\"material\":%d}]}", vix, matj);
            mesh_node[nmesh] = p.skinned ? -1 : (inode >= 0 && inode < nnode ? inode : 0);
            mesh_skin[nmesh] = p.skinned;
            skinned_any |= p.skinned;
            /* the accessors (in `accs`), one per view; the mesh above names views, mapped below */
            {
                char tmp[512];
                int n = 0;
                n += snprintf(tmp + n, sizeof tmp - (size_t)n, "{\"bufferView\":%d,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\",\"min\":[%g,%g,%g],\"max\":[%g,%g,%g]}",
                              vpos, p.nv, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
                bprintf(&accs, "%s%s", g.nacc ? "," : "", tmp);
                g.nacc++;
                if (vnrm >= 0) { bprintf(&accs, ",{\"bufferView\":%d,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\"}", vnrm, p.nv); g.nacc++; }
                if (vuv >= 0) { bprintf(&accs, ",{\"bufferView\":%d,\"componentType\":5126,\"count\":%d,\"type\":\"VEC2\"}", vuv, p.nv); g.nacc++; }
                if (vcol >= 0) { bprintf(&accs, ",{\"bufferView\":%d,\"componentType\":5126,\"count\":%d,\"type\":\"VEC4\"}", vcol, p.nv); g.nacc++; }
                if (vjn >= 0) {
                    bprintf(&accs, ",{\"bufferView\":%d,\"componentType\":5123,\"count\":%d,\"type\":\"VEC4\"}", vjn, p.nv);
                    bprintf(&accs, ",{\"bufferView\":%d,\"componentType\":5126,\"count\":%d,\"type\":\"VEC4\"}", vwt, p.nv);
                    g.nacc += 2;
                }
                bprintf(&accs, ",{\"bufferView\":%d,\"componentType\":5125,\"count\":%d,\"type\":\"SCALAR\"}", vix, p.ni);
                g.nacc++;
            }
            nmesh++;
            prim_free(&p);
        }
    }
    (void)mesh_skin;

    int rc = 0;
    if (!nmesh) { rc = 0; goto done; }
    {
        /* Accessor indices: the mesh JSON used view numbers; accessors were appended in the same
         * order as their views, skipping image views. Map view -> accessor. */
        int *v2a = (int *)malloc(sizeof(int) * (size_t)(g.nview + 1));
        for (int i = 0; i <= g.nview; i++) v2a[i] = -1;
        {   /* parse "bufferView":N from the accessor list, in order */
            int a = 0;
            const char *s = (const char *)accs.p;
            const char *end = s + accs.n;
            while (s && s < end) {
                const char *q = strstr(s, "\"bufferView\":");
                if (!q || q >= end) break;
                const int v = atoi(q + 13);
                if (v >= 0 && v <= g.nview) v2a[v] = a++;
                s = q + 13;
            }
        }
        /* rewrite the mesh JSON's numbers through v2a */
        buf m2 = { 0 };
        {
            const char *s = (const char *)meshes.p, *end = s + meshes.n;
            while (s < end) {
                const char *keys[] = { "\"POSITION\":", "\"NORMAL\":", "\"TEXCOORD_0\":", "\"COLOR_0\":", "\"JOINTS_0\":", "\"WEIGHTS_0\":", "\"indices\":" };
                int hit = -1;
                for (int k = 0; k < 7; k++) if (!strncmp(s, keys[k], strlen(keys[k]))) { hit = k; break; }
                if (hit >= 0) {
                    const size_t kl = strlen(keys[hit]);
                    bput(&m2, s, kl);
                    s += kl;
                    const int v = atoi(s);
                    while (s < end && *s >= '0' && *s <= '9') s++;
                    bprintf(&m2, "%d", v >= 0 && v <= g.nview ? v2a[v] : v);
                } else { bput(&m2, s, 1); s++; }
            }
        }
        /* inverse bind matrices (all nodes are joints) */
        int ibm_acc = -1;
        if (skinned_any) {
            float *ibm = (float *)malloc(sizeof(float) * 16 * (size_t)nnode);
            for (int i = 0; i < nnode; i++) memcpy(ibm + i * 16, nodes[i].inv, 64);
            const int v = add_view(&g, ibm, (size_t)nnode * 64, 0);
            bprintf(&accs, ",{\"bufferView\":%d,\"componentType\":5126,\"count\":%d,\"type\":\"MAT4\"}", v, nnode);
            ibm_acc = g.nacc++;
            free(ibm);
        }
        /* the document */
        buf j = { 0 };
        bprintf(&j, "{\"asset\":{\"version\":\"2.0\",\"generator\":\"psp2i recomp model extraction\"},\"scene\":0,");
        /* nodes: the skeleton, then one node per mesh */
        bprintf(&j, "\"nodes\":[");
        for (int i = 0; i < nnode; i++) {
            bprintf(&j, "%s{\"name\":\"node%d\",\"matrix\":[", i ? "," : "", i);
            for (int k = 0; k < 16; k++) bprintf(&j, "%s%g", k ? "," : "", (double)nodes[i].local[k]);
            bprintf(&j, "]");
            int nch = 0;
            for (int c = 0; c < nnode; c++)
                if (nodes[c].parent == i) bprintf(&j, "%s%d", nch++ ? "," : ",\"children\":[", c);
            for (int m = 0; m < nmesh; m++)
                if (mesh_node[m] == i) bprintf(&j, "%s%d", nch++ ? "," : ",\"children\":[", nnode + m);
            if (nch) bprintf(&j, "]");
            bprintf(&j, "}");
        }
        for (int m = 0; m < nmesh; m++)
            bprintf(&j, ",{\"name\":\"mesh%d\",\"mesh\":%d%s}", m, m, mesh_node[m] < 0 ? ",\"skin\":0" : "");
        bprintf(&j, "],\"scenes\":[{\"nodes\":[");
        int first = 1;
        for (int i = 0; i < nnode; i++) if (nodes[i].parent < 0 || nodes[i].parent >= nnode) { bprintf(&j, "%s%d", first ? "" : ",", i); first = 0; }
        for (int m = 0; m < nmesh; m++) if (mesh_node[m] < 0) { bprintf(&j, "%s%d", first ? "" : ",", nnode + m); first = 0; }
        bprintf(&j, "]}],\"meshes\":[");
        bput(&j, m2.p, m2.n);
        bprintf(&j, "],\"materials\":[");
        bput(&j, mats_json.p, mats_json.n);
        bprintf(&j, "]");
        if (nimg) {
            bprintf(&j, ",\"images\":[");
            bput(&j, images.p, images.n);
            bprintf(&j, "],\"textures\":[");
            bput(&j, texs.p, texs.n);
            bprintf(&j, "],\"samplers\":[{\"magFilter\":9729,\"minFilter\":9729}]");
        }
        if (skinned_any) {
            bprintf(&j, ",\"skins\":[{\"inverseBindMatrices\":%d,\"joints\":[", ibm_acc);
            for (int i = 0; i < nnode; i++) bprintf(&j, "%s%d", i ? "," : "", i);
            bprintf(&j, "]}]");
        }
        balign(&g.bin, 4, 0);
        bprintf(&j, ",\"accessors\":[");
        bput(&j, accs.p, accs.n);
        bprintf(&j, "],\"bufferViews\":[");
        bput(&j, g.json.p, g.json.n);
        bprintf(&j, "],\"buffers\":[{\"byteLength\":%zu}]}", g.bin.n);
        balign(&j, 4, ' ');
        free(m2.p);
        free(v2a);
        /* the file */
        FILE *f = fopen(path, "wb");
        if (!f) { free(j.p); rc = -1; goto done; }
        const uint32_t total = (uint32_t)(12 + 8 + j.n + 8 + g.bin.n);
        const uint32_t hdr[3] = { 0x46546C67u, 2, total }, jh[2] = { (uint32_t)j.n, 0x4E4F534Au }, bh[2] = { (uint32_t)g.bin.n, 0x004E4942u };
        fwrite(hdr, 4, 3, f); fwrite(jh, 4, 2, f); fwrite(j.p, 1, j.n, f); fwrite(bh, 4, 2, f); fwrite(g.bin.p, 1, g.bin.n, f);
        rc = ferror(f) ? -1 : 1;
        fclose(f);
        free(j.p);
    }
done:
    free(g.json.p); free(g.bin.p); free(meshes.p); free(mats_json.p); free(accs.p); free(images.p); free(texs.p);
    free(mesh_node); free(mesh_skin); free(nodes);
    return rc;
}

/* ---- the menu action ------------------------------------------------------------ */

int modelx_extract(const char *base_dir, char *summary, size_t cap) {
    char dir[700];
    snprintf(dir, sizeof dir, "%s/Data", base_dir);
    mkdir_one(dir);
    int written = 0, skipped = 0, failed = 0, seen = 0;
    index_textures();
    for (int i = 0; i < g_nrec; i++) {
        const rec *r = &g_rec[i];
        if (r->tag != TAG_UNJ || !rec_data(r)) continue;
        int dup = 0;                                  /* the same name loaded twice: once */
        for (int k = i + 1; k < g_nrec && !dup; k++) dup = g_rec[k].tag == TAG_UNJ && !strcmp(g_rec[k].name, r->name) && rec_data(&g_rec[k]);
        if (dup) continue;
        seen++;
        char stem[48];
        snprintf(stem, sizeof stem, "%s", r->name);
        char *dot = strrchr(stem, '.');
        if (dot) *dot = 0;
        const char *grp = group_of(stem);
        char path[800];
        snprintf(path, sizeof path, "%s/%s", dir, grp);
        mkdir_one(path);
        snprintf(path, sizeof path, "%s/%s/%s.glb", dir, grp, stem);
        if (file_exists(path)) { skipped++; continue; }
        const int rc = export_model(r, psp_read32(r->res + 0x10), path);
        if (rc > 0) written++;
        else if (rc < 0) failed++;
        else fprintf(stderr, "models: %s: no geometry read\n", r->name);
    }
    snprintf(summary, cap, "%d NEW, %d KEPT", written, skipped);
    fprintf(stderr, "models: %d loaded, %d written, %d already there, %d failed -> %s\n", seen, written, skipped, failed, dir);
    return written;
}

/* ---- LOAD MODELS: a model from Data/ in place of the game's ------------------------------
 *
 * When a model finishes loading (the model resource's setup, 0x08D59624, has
 * set resource+0x10 to its object) and Data/<group>/<name>.glb exists, a new
 * object is built from it in the host's RAM (above 0x0A000000: the game's
 * heap is untouched) and put in the resource's place. It keeps the game's
 * nodes (the skeleton the motions drive) and material blocks (the draw state);
 * the GLB gives the geometry:
 *
 *   - a glTF node named "nodeN" is the game's node N (as extraction names them);
 *     a rigid mesh belongs to the node it hangs under, a skinned one is in the
 *     bind space of the skin, whose joints name nodes;
 *   - a primitive's material names its game material in extras.psp_material
 *     (else material 0);
 *   - skinned triangles are split into vertex lists of at most 4 bones, as the
 *     game's own (the bone table holds matrix-palette indices);
 *   - vertices are written as floats (weights, UV, colour 8888, normal,
 *     position) in one triangle strip per list, UVs undoing the material's
 *     scale and offset (the GE applies them again).
 *
 * Images named after one of the model's textures replace that texture where
 * the renderer samples it (modelx_texture), at any size. */

#include "glbread.h"

static int g_load_on;
static char g_data_dir[700];
static struct { uint32_t mine, orig; } g_swap[512];  /* loaded models: ours, the game's (dev A/B) */
static int g_nswap;
static uint32_t g_pool = PSP_HOST_RAM_BASE;
static uint64_t g_loaded, g_load_failed;

static void read_ahead(void);
void modelx_set_load(int on) {
    if (on && !g_load_on) read_ahead();
    g_load_on = on;
}

static uint32_t pool_alloc(uint32_t n) {
    n = (n + 15u) & ~15u;
    if (g_pool + n > PSP_RAM_BASE + PSP_RAM_SIZE) return 0;
    const uint32_t a = g_pool;
    g_pool += n;
    memset(psp_mem_ptr(a, n), 0, n);
    return a;
}

/* Texture replacements: original pixel address -> RGBA. `data` is the file it
 * was found at, `sig` a hash of its header and first pixels: checked every
 * vblank, as the game loads other textures where these were. `key` = the PNG's hash. */
typedef struct { char name[32]; uint32_t addr, at, data; unsigned key, sig; uint8_t *rgba; int w, h; } texrep;
static texrep *g_rep;
static int g_nrep, g_rep_pending;
static unsigned g_rep_gen;

/* The pixel data of a .uvr file: past GBIX, UVRT's header and its palette. */
static uint32_t uvr_pixels(uint32_t a) {
    for (int k = 0; k < 64 && psp_read32(a) != 0x54525655u; k += 4) a += 4;
    if (psp_read32(a) != 0x54525655u) return 0;
    const int f0 = psp_read8(a + 8), lo = psp_read8(a + 9) & 0x7F;
    const int ps = f0 == 3 ? 4 : 2, n = lo == 6 || lo == 8 ? 16 : lo == 10 || lo == 12 ? 256 : 0;
    return a + 16 + (uint32_t)(n * ps);
}

static unsigned fnv(const uint8_t *p, size_t n);
static unsigned tex_sig(uint32_t data, uint32_t px) {
    const uint8_t *h = (const uint8_t *)psp_mem_ptr(data, 32), *q = (const uint8_t *)psp_mem_ptr(px, 256);
    return h && q ? fnv(h, 32) * 31u + fnv(q, 256) : 0;
}

static void rep_add(const char *name, unsigned key, uint8_t *rgba, int w, int h) {
    for (int i = 0; i < g_nrep; i++)
        if (!strcmp(g_rep[i].name, name)) {
            free(g_rep[i].rgba);
            g_rep[i].rgba = rgba; g_rep[i].w = w; g_rep[i].h = h; g_rep[i].key = key;
            g_rep[i].addr = g_rep[i].at = g_rep[i].data = 0;
            g_rep_pending = g_scan_due = 1; g_rep_gen++;
            return;
        }
    texrep *p = (texrep *)realloc(g_rep, sizeof *p * (size_t)(g_nrep + 1));
    if (!p) { free(rgba); return; }
    g_rep = p;
    memset(&g_rep[g_nrep], 0, sizeof g_rep[g_nrep]);
    snprintf(g_rep[g_nrep].name, sizeof g_rep[g_nrep].name, "%s", name);
    g_rep[g_nrep].rgba = rgba; g_rep[g_nrep].w = w; g_rep[g_nrep].h = h; g_rep[g_nrep].key = key;
    g_nrep++;
    g_rep_pending = g_scan_due = 1;
}

/* Where the replaced textures are now (the texture registry). */
static void rep_resolve(void) {
    const double t0 = now_ms();
    int left = 0;
    for (int i = 0; i < g_nrep; i++) {
        const texent *e = texture_entry(g_rep[i].name);
        const uint32_t px = e ? uvr_pixels(e->data) : 0;
        if (px != g_rep[i].addr) {
            static int verbose = -1;
            if (verbose < 0) verbose = getenv("PSP2I_MODELX_VERBOSE") != NULL;
            if (verbose) fprintf(stderr, "models: texture %s now at %08X\n", g_rep[i].name, px);
            g_rep[i].addr = px; g_rep_gen++;
        }
        g_rep[i].at = e ? e->at : 0;
        g_rep[i].data = e ? e->data : 0;
        g_rep[i].sig = px ? tex_sig(e->data, px) : 0;
        if (!px) left++;
    }
    g_rep_pending = left > 0;
    (void)t0;
}

/* A replacement whose texture is no longer where it was: unknown again. */
static void rep_check(void) {
    for (int i = 0; i < g_nrep; i++) {
        if (!g_rep[i].addr) continue;
        if (tex_sig(g_rep[i].data, g_rep[i].addr) == g_rep[i].sig) continue;   /* the same texture still there */
        g_rep[i].addr = 0;
        g_rep_gen++;
        g_rep_pending = g_scan_due = 1;
    }
}

/* ---- GLB images, decoded off the game thread -------------------------------- */

typedef struct { char name[32]; unsigned key; uint8_t *png; size_t n; uint8_t *rgba; int w, h, done; } imgjob;
static imgjob *g_job;
static int g_njob, g_capjob, g_worker;
static int g_scan_req, g_scan_ready, g_scan_n;       /* a texture scan asked for / finished, under the lock */
static texent *g_scan_out;

/* GLBs read and parsed ahead by the worker (all of Data/ when LOAD MODELS is on),
 * so a model loads without waiting for its file. Kept while unchanged. */
typedef struct { char path[600]; long long stamp; glb g; } glbent;
static glbent *g_glbs;
static int g_nglb, g_capglb;
static char (*g_ahead)[600];                         /* paths still to read ahead */
static int g_nahead;
#ifdef _WIN32
static CRITICAL_SECTION g_jl;
static CONDITION_VARIABLE g_jc;
#define JLOCK()   EnterCriticalSection(&g_jl)
#define JUNLOCK() LeaveCriticalSection(&g_jl)
#define JWAIT()   SleepConditionVariableCS(&g_jc, &g_jl, INFINITE)
#define JWAKE()   WakeConditionVariable(&g_jc)
#else
static pthread_mutex_t g_jl = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_jc = PTHREAD_COND_INITIALIZER;
#define JLOCK()   pthread_mutex_lock(&g_jl)
#define JUNLOCK() pthread_mutex_unlock(&g_jl)
#define JWAIT()   pthread_cond_wait(&g_jc, &g_jl)
#define JWAKE()   pthread_cond_signal(&g_jc)
#endif

/* Under the lock: `g` kept for `path`; 0 when there's no room (then not kept). */
static int glb_keep(const char *path, long long stamp, const glb *g) {
    if (g_nglb == g_capglb) {
        const int cap = g_capglb ? g_capglb * 2 : 64;
        glbent *p = (glbent *)realloc(g_glbs, sizeof *p * (size_t)cap);
        if (!p) return 0;
        g_glbs = p; g_capglb = cap;
    }
    glbent *e = &g_glbs[g_nglb++];
    snprintf(e->path, sizeof e->path, "%s", path);
    e->stamp = stamp;
    e->g = *g;
    return 1;
}

static void decode_jobs(void) {
    JLOCK();
    for (;;) {
        if (g_scan_req) {
            g_scan_req = 0;
            JUNLOCK();
            texent *t;
            const int n = scan_textures(&t);
            JLOCK();
            free(g_scan_out);
            g_scan_out = t; g_scan_n = n; g_scan_ready = 1;
            continue;
        }
        int i = 0;
        while (i < g_njob && (g_job[i].done || !g_job[i].png)) i++;
        if (i == g_njob && g_nahead) {                                /* idle: read a GLB ahead */
            char path[600];
            memcpy(path, g_ahead[--g_nahead], sizeof path);
            int have = 0;
            for (int k = 0; k < g_nglb; k++) have |= !strcmp(g_glbs[k].path, path);
            if (have) continue;
            JUNLOCK();
            const long long st = file_stamp(path);
            glb g;
            const int ok = st && glb_load(path, &g) == 0;
            JLOCK();
            have = 0;
            for (int k = 0; k < g_nglb; k++) have |= !strcmp(g_glbs[k].path, path);
            if (ok && !have && glb_keep(path, st, &g)) continue;
            if (ok) glb_free(&g);
            continue;
        }
        if (i == g_njob) { JWAIT(); continue; }
        uint8_t *png = g_job[i].png;
        const size_t n = g_job[i].n;
        const unsigned key = g_job[i].key;
        g_job[i].png = NULL;                                          /* taken */
        JUNLOCK();
        int w = 0, h = 0;
        uint8_t *rgba = png_decode(png, n, &w, &h);
        free(png);
        JLOCK();
        for (int k = 0; k < g_njob; k++)                              /* the array may have moved */
            if (!g_job[k].done && !g_job[k].png && g_job[k].key == key && !g_job[k].rgba) {
                g_job[k].rgba = rgba; g_job[k].w = w; g_job[k].h = h; g_job[k].done = 1;
                rgba = NULL;
                break;
            }
        free(rgba);
    }
}
#ifdef _WIN32
static DWORD WINAPI worker_main(LPVOID a) { (void)a; decode_jobs(); return 0; }
#else
static void *worker_main(void *a) { (void)a; decode_jobs(); return NULL; }
#endif

static unsigned fnv(const uint8_t *p, size_t n) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h ? h : 1;
}

static void start_worker(void) {
    if (g_worker) return;
#ifdef _WIN32
    InitializeCriticalSection(&g_jl);
    InitializeConditionVariable(&g_jc);
    HANDLE t = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
    if (t) CloseHandle(t);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, worker_main, NULL) == 0) pthread_detach(t);
#endif
    g_worker = 1;
}

/* The GLB at `path`, read ahead or now (and kept); 0 ok. Not to be freed. */
static int glb_get(const char *path, glb *out) {
    const long long st = file_stamp(path);
    if (!st) return -1;
    start_worker();
    JLOCK();
    for (int k = 0; k < g_nglb; k++)
        if (!strcmp(g_glbs[k].path, path)) {
            if (g_glbs[k].stamp == st) { *out = g_glbs[k].g; JUNLOCK(); return 0; }
            glb_free(&g_glbs[k].g);                                   /* changed: read again */
            g_glbs[k] = g_glbs[--g_nglb];
            break;
        }
    JUNLOCK();
    glb g;
    if (glb_load(path, &g) != 0) return -1;
    JLOCK();
    int have = 0;
    for (int k = 0; k < g_nglb && !have; k++)
        if (!strcmp(g_glbs[k].path, path)) {                          /* the worker read it meanwhile */
            glb_free(&g_glbs[k].g);
            g_glbs[k].g = g; g_glbs[k].stamp = st;
            have = 1;
        }
    if (!have && !glb_keep(path, st, &g)) { JUNLOCK(); glb_free(&g); return -1; }
    *out = g;
    JUNLOCK();
    return 0;
}

/* Every Data/<group>/*.glb to be read ahead. */
static void read_ahead(void) {
    if (!g_data_dir[0]) return;
    char dir[700];
    snprintf(dir, sizeof dir, "%s/Data", g_data_dir);
    char (*list)[600] = NULL;
    int n = 0, cap = 0;
#ifdef _WIN32
    char pat[760];
    snprintf(pat, sizeof pat, "%s/*", dir);
    WIN32_FIND_DATAA gd;
    HANDLE hg = FindFirstFileA(pat, &gd);
    if (hg == INVALID_HANDLE_VALUE) return;
    do {
        if (!(gd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || gd.cFileName[0] == '.') continue;
        snprintf(pat, sizeof pat, "%s/%s/*.glb", dir, gd.cFileName);
        WIN32_FIND_DATAA fd;
        HANDLE hf = FindFirstFileA(pat, &fd);
        if (hf == INVALID_HANDLE_VALUE) continue;
        do {
            if (n == cap) { cap = cap ? cap * 2 : 128; void *q = realloc(list, sizeof *list * (size_t)cap); if (!q) break; list = (char (*)[600])q; }
            snprintf(list[n++], 600, "%s/%s/%s", dir, gd.cFileName, fd.cFileName);
        } while (FindNextFileA(hf, &fd));
        FindClose(hf);
    } while (FindNextFileA(hg, &gd));
    FindClose(hg);
#else
    DIR *dg = opendir(dir);
    if (!dg) return;
    struct dirent *ge;
    while ((ge = readdir(dg)) != NULL) {
        if (ge->d_name[0] == '.') continue;
        char sub[760];
        snprintf(sub, sizeof sub, "%s/%s", dir, ge->d_name);
        DIR *df = opendir(sub);
        if (!df) continue;
        struct dirent *fe;
        while ((fe = readdir(df)) != NULL) {
            const size_t l = strlen(fe->d_name);
            if (l < 5 || strcmp(fe->d_name + l - 4, ".glb")) continue;
            if (n == cap) { cap = cap ? cap * 2 : 128; void *q = realloc(list, sizeof *list * (size_t)cap); if (!q) break; list = (char (*)[600])q; }
            snprintf(list[n++], 600, "%s/%s", sub, fe->d_name);
        }
        closedir(df);
    }
    closedir(dg);
#endif
    if (!n) { free(list); return; }
    start_worker();
    JLOCK();
    free(g_ahead);
    g_ahead = list; g_nahead = n;
    JWAKE();
    JUNLOCK();
}

/* Texture `name` replaced by PNG `png`: decoded on the worker, unless it already is. */
static void queue_image(const char *name, const uint8_t *png, size_t n) {
    if (!png || !n) return;
    const unsigned key = fnv(png, n);
    for (int i = 0; i < g_nrep; i++)
        if (!strcmp(g_rep[i].name, name) && g_rep[i].key == key) return;
    start_worker();
    JLOCK();
    int dup = 0;
    for (int i = 0; i < g_njob; i++) dup |= !strcmp(g_job[i].name, name) && g_job[i].key == key;
    if (!dup && g_njob == g_capjob) {
        const int cap = g_capjob ? g_capjob * 2 : 64;
        imgjob *p = (imgjob *)realloc(g_job, sizeof *p * (size_t)cap);
        if (p) { g_job = p; g_capjob = cap; } else dup = 1;
    }
    uint8_t *copy = dup ? NULL : (uint8_t *)malloc(n);
    if (copy) {
        memcpy(copy, png, n);
        imgjob *j = &g_job[g_njob++];
        memset(j, 0, sizeof *j);
        snprintf(j->name, sizeof j->name, "%s", name);
        j->key = key; j->png = copy; j->n = n;
        JWAKE();
    }
    JUNLOCK();
}

/* Decoded images become replacements, in the order they were queued. */
static void take_images(void) {
    if (!g_worker) return;
    JLOCK();
    int k = 0;
    while (k < g_njob && g_job[k].done) {
        if (g_job[k].rgba) rep_add(g_job[k].name, g_job[k].key, g_job[k].rgba, g_job[k].w, g_job[k].h);
        k++;
    }
    if (k) { memmove(g_job, g_job + k, sizeof *g_job * (size_t)(g_njob - k)); g_njob -= k; }
    JUNLOCK();
}

const uint8_t *modelx_texture(uint32_t addr, int *w, int *h) {
    addr &= 0x0FFFFFFFu;
    for (int i = 0; i < g_nrep; i++)
        if (g_rep[i].addr && g_rep[i].addr == addr) { *w = g_rep[i].w; *h = g_rep[i].h; return g_rep[i].rgba; }
    return NULL;
}
unsigned modelx_texture_gen(void) { return g_rep_gen; }

/* Dev A/B (PSP2I_MODELX_FLIP_AT=v1,v2,...): at those vblanks the loaded models
 * switch between the rebuilt geometry and the game's own, in the object and in
 * every copy of its header the game made (instances), so one run can show the
 * same view both ways. */
static void flip_models(void) {
    static int showing_orig;
    const uint8_t *ram = (const uint8_t *)psp_mem_ptr(PSP_RAM_BASE, PSP_RAM_SIZE);
    int n = 0;
    for (int i = 0; i < g_nswap && ram; i++) {
        const uint32_t from = showing_orig ? g_swap[i].orig : g_swap[i].mine, to = showing_orig ? g_swap[i].mine : g_swap[i].orig;
        const uint32_t fv = psp_read32(from + 0x1C), fs = psp_read32(from + 0x3C);
        static const uint32_t F[] = { 0x18, 0x1C, 0x20, 0x24, 0x38, 0x3C };
        uint32_t want[6];
        for (int k = 0; k < 6; k++) want[k] = psp_read32(to + F[k]);
        for (uint32_t o = 0; o + 0x60 < PSP_RAM_SIZE; o += 4) {   /* headers showing `from`: the object and its instances */
            uint32_t a, b;
            memcpy(&a, ram + o + 0x1C, 4);
            if (a != fv) continue;
            memcpy(&b, ram + o + 0x3C, 4);
            if (b != fs) continue;
            for (int k = 0; k < 6; k++) psp_write32(PSP_RAM_BASE + o + F[k], want[k]);
            n++;
        }
    }
    showing_orig = !showing_orig;
    fprintf(stderr, "models: flip -> showing the %s models (%d headers)\n", showing_orig ? "game's own" : "loaded", n);
}

void modelx_vblank(void) {
    {
        static const char *flips = (const char *)1;
        static unsigned vb;
        if (flips == (const char *)1) flips = getenv("PSP2I_MODELX_FLIP_AT");
        vb++;
        for (const char *c = flips; c && *c; ) {
            if ((unsigned)strtoul(c, NULL, 10) == vb) flip_models();
            c = strchr(c, ',');
            if (c) c++;
        }
    }
    static unsigned n, last;
    n++;
    take_images();
    rep_check();
    /* look for the textures (on the worker) when files have loaded, at most twice a second */
    if (!g_worker) return;
    JLOCK();
    if (g_scan_ready) {
        g_scan_ready = 0;
        free(g_tex);
        g_tex = g_scan_out; g_ntex = g_scan_n;
        g_scan_out = NULL;
        JUNLOCK();
        rep_resolve();
        JLOCK();
    }
    if (g_rep_pending && g_scan_due && n - last >= 30 && !g_scan_req) {
        g_scan_due = 0; last = n;
        g_scan_req = 1;
        JWAKE();
    }
    JUNLOCK();
}

/* glTF node -> its local matrix (column-major). */
static void node_matrix(const js *nd, float *m) {
    const js *mt = js_get(nd, "matrix");
    if (mt && mt->n == 16) { for (int k = 0; k < 16; k++) m[k] = (float)js_num(js_at(mt, k), 0); return; }
    const js *t = js_get(nd, "translation"), *r = js_get(nd, "rotation"), *s = js_get(nd, "scale");
    const double x = js_num(js_at(r, 0), 0), y = js_num(js_at(r, 1), 0), z = js_num(js_at(r, 2), 0), w = r ? js_num(js_at(r, 3), 1) : 1;
    const double sx = s ? js_num(js_at(s, 0), 1) : 1, sy = s ? js_num(js_at(s, 1), 1) : 1, sz = s ? js_num(js_at(s, 2), 1) : 1;
    const double R[9] = { 1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                          2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                          2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y) };
    for (int c = 0; c < 3; c++) {
        const double sc = c == 0 ? sx : c == 1 ? sy : sz;
        for (int rr = 0; rr < 3; rr++) m[c * 4 + rr] = (float)(R[c * 3 + rr] * sc);
        m[c * 4 + 3] = 0;
    }
    m[12] = (float)js_num(js_at(t, 0), 0); m[13] = (float)js_num(js_at(t, 1), 0); m[14] = (float)js_num(js_at(t, 2), 0); m[15] = 1;
}

/* One vertex list to write. */
typedef struct {
    float *v;                 /* per vertex: w[4] u v r g b a nx ny nz x y z = 16 floats */
    int nv, cap;
    uint32_t *strip;          /* vertex numbers in strip order */
    int ns, scap;
    int bones[4], nb;         /* palette indices */
    uint32_t *tri;            /* triangles (3 vertex numbers each), made into the strip at the end */
    int nt, tcap;
    int material, node, skinned, has_col, has_nrm;
    int mesh;                 /* the glTF mesh it came from */
} newlist;

static int nl_vertex(newlist *l, const float *f) {
    if (l->nv == l->cap) { l->cap = l->cap ? l->cap * 2 : 256; l->v = (float *)realloc(l->v, sizeof(float) * 16 * (size_t)l->cap); }
    memcpy(l->v + 16 * l->nv, f, 64);
    return l->nv++;
}
static void nl_strip(newlist *l, uint32_t a) {
    if (l->ns == l->scap) { l->scap = l->scap ? l->scap * 2 : 512; l->strip = (uint32_t *)realloc(l->strip, sizeof(uint32_t) * (size_t)l->scap); }
    l->strip[l->ns++] = a;
}
/* Triangle (a, b, c) onto the strip, joined to the last one by degenerate triangles. */
static void nl_triangle(newlist *l, uint32_t a, uint32_t b, uint32_t c) {
    if (l->nt == l->tcap) { l->tcap = l->tcap ? l->tcap * 2 : 256; l->tri = (uint32_t *)realloc(l->tri, sizeof(uint32_t) * 3 * (size_t)l->tcap); }
    l->tri[3 * l->nt] = a; l->tri[3 * l->nt + 1] = b; l->tri[3 * l->nt + 2] = c;
    l->nt++;
}

/* The vertex that adds triangle t to a strip ending (x, y) with n entries, or -1:
 * the triangle at n-2 is (x, y, new), reversed when n-2 is odd. */
static int strip_next(uint32_t x, uint32_t y, int n, const uint32_t *t) {
    const uint32_t p0 = (n & 1) ? y : x, p1 = (n & 1) ? x : y;
    for (int r = 0; r < 3; r++)
        if (t[r] == p0 && t[(r + 1) % 3] == p1) return (int)t[(r + 2) % 3];
    return -1;
}

/* The list's triangles as one strip: each continues it when it can (one vertex,
 * or two to turn the winding), else a new run joined by degenerate triangles,
 * started on the turn that lets the next triangle continue. */
static void nl_build(newlist *l) {
    for (int i = 0; i < l->nt; i++) {
        const uint32_t *t = l->tri + 3 * i;
        if (l->ns >= 2) {
            const uint32_t x = l->strip[l->ns - 2], y = l->strip[l->ns - 1];
            const int v = strip_next(x, y, l->ns, t);
            if (v >= 0) { nl_strip(l, (uint32_t)v); continue; }
            const int w = strip_next(y, x, l->ns + 1, t);             /* (x y x): the winding turned */
            if (w >= 0) { nl_strip(l, x); nl_strip(l, (uint32_t)w); continue; }
            int done = 0;                                             /* (x y z) with no area: z a copy of x or y */
            for (int k = 0; k < 3 && !done; k++) {
                const uint32_t z = t[k];
                if (z == y) continue;
                /* a copy in position and bone weights: otherwise the two part as the
                 * model animates and the triangle shows (a spike on NPC 914) */
                const float *Z = l->v + 16 * z;
                const int same_x = !memcmp(Z + 13, l->v + 16 * x + 13, 12) && !memcmp(Z, l->v + 16 * x, 16);
                const int same_y = !memcmp(Z + 13, l->v + 16 * y + 13, 12) && !memcmp(Z, l->v + 16 * y, 16);
                if (!same_x && !same_y) continue;
                const int u = strip_next(y, z, l->ns + 1, t);
                if (u >= 0) { nl_strip(l, z); nl_strip(l, (uint32_t)u); done = 1; }
            }
            if (done) continue;
        }
        int rot = 0;                                                  /* the turn the next one follows */
        if (i + 1 < l->nt)
            for (int r = 0; r < 3; r++) {
                const uint32_t q0 = t[r], q1 = t[(r + 1) % 3], q2 = t[(r + 2) % 3];
                if (strip_next(q1, q2, 3, l->tri + 3 * (i + 1)) >= 0) { rot = r; break; }
                (void)q0;
            }
        const uint32_t a = t[rot], b = t[(rot + 1) % 3], c = t[(rot + 2) % 3];
        if (l->ns) {
            nl_strip(l, l->strip[l->ns - 1]);
            nl_strip(l, a);
            if (l->ns & 1) nl_strip(l, a);     /* keep the winding: triangles start on even positions */
        }
        nl_strip(l, a); nl_strip(l, b); nl_strip(l, c);
    }
}

/* Write a list as a GE vertex list; its address, or 0. */
static uint32_t write_vtxlist(const newlist *l, uint32_t tmpl, float scale) {
    /* UVs as the template list has them (8 or 16-bit: x/128, x/32768) when they
     * fit there, so the list is laid out as the game's own; else float */
    const uint32_t ttc = tmpl ? psp_read32(tmpl + 4) & 3u : 2u;
    int uvf = ttc == 1 || ttc == 2 ? (int)ttc : 2;
    for (int pass = 0; pass < 2 && uvf != 3; pass++) {
        const float top = uvf == 1 ? 255.0f / 128.0f : 65535.0f / 32768.0f;
        int fits = 1;
        for (int i = 0; i < l->nv && fits; i++)
            for (int k = 4; k < 6; k++) { const float u = l->v[16 * i + k]; if (!(u >= 0.0f && u <= top)) fits = 0; }
        if (fits) break;
        uvf = uvf == 1 ? 2 : 3;                                    /* wider */
    }
    uint32_t vt = (uint32_t)uvf | 2u << 7;                        /* UV, 16-bit position */
    if (l->has_nrm) vt |= 1u << 5;                                /* 8-bit normal */
    /* colours in the template list's format (the game's own map lists: 4444),
     * else 8888 */
    const uint32_t tcol = tmpl ? (psp_read32(tmpl + 4) >> 2) & 7u : 0;
    const uint32_t cfmt = tcol >= 4 && tcol <= 7 ? tcol : 7u;
    if (l->has_col) vt |= cfmt << 2;
    if (l->skinned) vt |= 1u << 9 | (uint32_t)(l->nb - 1) << 14;   /* 8-bit weights (128 = 1) */
    vlayout L;
    layout_of(vt, &L);
    const uint32_t data = pool_alloc((uint32_t)(L.size * l->ns));
    const uint32_t vl = pool_alloc(0x30), counts = pool_alloc(4), bones = l->skinned ? pool_alloc(16) : 0;
    if (!data || !vl || !counts || (l->skinned && !bones)) return 0;
    /* positions are s16/32768 * scale, as the game's -- one scale for the whole
     * model (the caller's): the game clips a model's lists near the camera with
     * one matrix built for its scale (lists of another scale lost walls there) */
    /* Bones in the template list's order when they are its bones: the game sets
     * a list's bone matrices up in its own list's order (in another order the
     * weights moved the wrong bones: a spike on NPC 914). slot[k]: where our
     * bone k goes. */
    int slot[4] = { 0, 1, 2, 3 }, order[4];
    for (int k = 0; k < l->nb; k++) order[k] = l->bones[k];
    if (l->skinned && tmpl && (int)psp_read32(tmpl + 0x1C) == l->nb && ok_ptr(psp_read32(tmpl + 0x18), (uint32_t)l->nb * 4)) {
        int tb[4], all = 1;
        for (int k = 0; k < l->nb; k++) tb[k] = (int)psp_read32(psp_read32(tmpl + 0x18) + (uint32_t)k * 4);
        for (int k = 0; k < l->nb && all; k++) {
            int at = -1;
            for (int q = 0; q < l->nb; q++) if (tb[q] == l->bones[k]) at = q;
            if (at < 0) all = 0; else slot[k] = at;
        }
        if (all) for (int k = 0; k < l->nb; k++) order[slot[k]] = l->bones[k];
        else for (int k = 0; k < 4; k++) slot[k] = k;
    }
    for (int i = 0; i < l->ns; i++) {                             /* the strip's vertices, in order */
        const float *f = l->v + 16 * l->strip[i];
        const uint32_t v = data + (uint32_t)(i * L.size);
        if (l->skinned) {           /* rounded to sum to 127, as the game's: 0x80 reads as -1 (spikes) */
            int sum = 0, big = 0;
            uint8_t w8[4];
            for (int k = 0; k < l->nb; k++) {
                const int q = (int)lrintf(f[k] * 127.0f);
                w8[k] = (uint8_t)(q < 0 ? 0 : q > 127 ? 127 : q);
                sum += w8[k];
                if (f[k] > f[big]) big = k;
            }
            w8[big] = (uint8_t)(w8[big] + 127 - sum);
            for (int k = 0; k < l->nb; k++) psp_write8(v + (uint32_t)L.ow + (uint32_t)slot[k], w8[k]);
        }
        if (uvf == 1) for (int k = 0; k < 2; k++) psp_write8(v + (uint32_t)L.otc + (uint32_t)k, (uint8_t)lrintf(f[4 + k] * 128.0f));
        else if (uvf == 2) for (int k = 0; k < 2; k++) psp_write16(v + (uint32_t)L.otc + (uint32_t)k * 2, (uint16_t)lrintf(f[4 + k] * 32768.0f));
        else { psp_write_f32(v + (uint32_t)L.otc, f[4]); psp_write_f32(v + (uint32_t)L.otc + 4, f[5]); }
        if (l->has_col) {
            int c8[4];
            for (int k = 0; k < 4; k++) c8[k] = f[6 + k] < 0 ? 0 : f[6 + k] > 1 ? 255 : (int)(f[6 + k] * 255.0f + 0.5f);
            if (cfmt == 7) for (int k = 0; k < 4; k++) psp_write8(v + (uint32_t)L.ocol + (uint32_t)k, (uint8_t)c8[k]);
            else {                                                    /* 4: 565, 5: 5551, 6: 4444 */
                uint16_t c;
                if (cfmt == 4) c = (uint16_t)((c8[0] * 31 + 127) / 255 | (c8[1] * 63 + 127) / 255 << 5 | (c8[2] * 31 + 127) / 255 << 11);
                else if (cfmt == 5) c = (uint16_t)((c8[0] * 31 + 127) / 255 | (c8[1] * 31 + 127) / 255 << 5 | (c8[2] * 31 + 127) / 255 << 10 | (c8[3] >= 128) << 15);
                else c = (uint16_t)((c8[0] * 15 + 127) / 255 | (c8[1] * 15 + 127) / 255 << 4 | (c8[2] * 15 + 127) / 255 << 8 | (c8[3] * 15 + 127) / 255 << 12);
                psp_write16(v + (uint32_t)L.ocol, c);
            }
        }
        for (int k = 0; k < 3 && l->has_nrm; k++) {
            const float q = f[10 + k] * 127.0f;
            psp_write8(v + (uint32_t)L.onrm + (uint32_t)k, (uint8_t)(int8_t)(q > 127 ? 127 : q < -127 ? -127 : lrintf(q)));
        }
        for (int k = 0; k < 3; k++) {
            const float q = f[13 + k] / scale * 32768.0f;
            psp_write16(v + (uint32_t)L.opos + (uint32_t)k * 2, (uint16_t)(int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : lrintf(q)));
        }
    }
    psp_write32(counts, (uint32_t)l->ns);
    for (int k = 0; k < l->nb; k++) psp_write32(bones + (uint32_t)k * 4, (uint32_t)order[k]);
    for (uint32_t k = 0; k < 0x30; k += 4) psp_write32(vl + k, tmpl ? psp_read32(tmpl + k) : 0);   /* the game's own fields */
    /* the layout as the game describes it: +0 0x10 = 16-bit positions (scaled by +2C), 0x100|2 = UVs;
     * +8 bytes = UV offset, then the UV, color and normal sizes with padding; +C low byte = position size */
    uint32_t kind = psp_read32(vl) & ~0x1FFu;
    kind |= (psp_read32(vl) & 0xEu) && !(psp_read32(vl) & 1u) ? psp_read32(vl) & 0xEu : 2u;
    psp_write32(vl, kind | 0x110u);
    psp_write32(vl + 4, vt);
    const int after_col = L.nrm ? L.onrm : L.opos, after_tc = L.col ? L.ocol : after_col;
    psp_write32(vl + 8, (uint32_t)L.otc | (uint32_t)(after_tc - L.otc) << 8
                        | (uint32_t)(L.col ? after_col - L.ocol : 0) << 16 | (uint32_t)(L.nrm ? L.opos - L.onrm : 0) << 24);
    psp_write32(vl + 0x0C, (psp_read32(vl + 0x0C) & 0xFFFF0000u) | (uint32_t)L.size << 8 | 6u);
    psp_write32(vl + 0x10, counts);
    psp_write32(vl + 0x14, data);
    psp_write32(vl + 0x18, bones);
    psp_write32(vl + 0x1C, l->skinned ? (uint32_t)l->nb : 0);
    psp_write32(vl + 0x20, (uint32_t)l->ns);
    psp_write_f32(vl + 0x2C, scale);
    return vl;
}

/* The palette index a glTF joint stands for. */
static int joint_palette(const js *joints, uint32_t j, const int *game_of, int ngn, const int *imatrix, const int *parent) {
    const int jnode = js_int(js_at(joints, (int)j), -1);
    int gnode = jnode >= 0 && jnode < ngn ? game_of[jnode] : -1;
    while (gnode >= 0 && imatrix[gnode] < 0) gnode = parent[gnode];
    return gnode >= 0 ? imatrix[gnode] : 0;
}

/* Whether image `img` is model `model`'s own by its name ("np_xxx_026_wh_body.uvr"). */
static int own_image(const char *img, const char *model) {
    const char *dot = strrchr(model, '.');
    const size_t n = dot ? (size_t)(dot - model) : strlen(model);
    return n && !strncmp(img, model, n);
}

/* The model at `obj` rebuilt from `path`; the new object, or 0. */
static uint32_t import_model(const char *path, uint32_t obj, const rec *r) {
    const char *name = r->name;
    glb g;
    if (glb_get(path, &g) != 0) { fprintf(stderr, "models: %s: not a GLB file I can read\n", path); return 0; }
    const int nmat = (int)psp_read32(obj + 0x10), nvl = (int)psp_read32(obj + 0x18), nnode = (int)psp_read32(obj + 0x28);
    const uint32_t mats = psp_read32(obj + 0x14), vls = psp_read32(obj + 0x1C), nodesp = psp_read32(obj + 0x30);
    const int nsub = (int)psp_read32(obj + 0x38);
    const uint32_t subs = psp_read32(obj + 0x3C);
    uint32_t newobj = 0;
    newlist *lists = NULL;
    int nlists = 0;
    static int imatrix[MAXN], parent[MAXN], mat_sub[1024], mat_node[1024];
    int *game_of = NULL, *gparent = NULL, *named = NULL, *gmat_of = NULL;
    char *moved = NULL;
    float (*gw)[16] = NULL;                   /* glTF nodes' rest worlds */
    static float bindw[MAXN][16];                /* the game's nodes' bind worlds */
    int nmoved = 0, nforeign = 0;
    if (nnode <= 0 || nnode > MAXN || nsub <= 0 || nsub > 16 || nvl <= 0 || nmat <= 0) goto out;

    /* the game's nodes: palette index, parent; templates for rigid / skinned lists */
    for (int i = 0; i < nnode; i++) {
        imatrix[i] = (int16_t)psp_read16(nodesp + (uint32_t)i * 0x90 + 4);
        parent[i] = (int16_t)psp_read16(nodesp + (uint32_t)i * 0x90 + 6);
    }
    uint32_t tmpl_rigid = 0, tmpl_skin = 0, tmpl_rigid_t = 0, tmpl_skin_t = 0;
    for (int i = 0; i < nvl; i++) {
        const uint32_t t = psp_read32(vls + (uint32_t)i * 8), v = psp_read32(vls + (uint32_t)i * 8 + 4);
        if (psp_read32(v + 0x1C)) { if (!tmpl_skin) { tmpl_skin = v; tmpl_skin_t = t; } }
        else if (!tmpl_rigid) { tmpl_rigid = v; tmpl_rigid_t = t; }
    }
    if (!tmpl_rigid) { tmpl_rigid = tmpl_skin; tmpl_rigid_t = tmpl_skin_t; }
    if (!tmpl_skin) { tmpl_skin = tmpl_rigid; tmpl_skin_t = tmpl_rigid_t; }
    /* where each material was used: its sub-object, the node its meshsets name */
    int skin_node = 0;
    for (int i = 0; i < 1024; i++) { mat_sub[i] = -1; mat_node[i] = -1; }
    for (int s = 0; s < nsub; s++) {
        const uint32_t so = subs + (uint32_t)s * 0x14, msl = psp_read32(so + 8);
        const int nms = (int)psp_read32(so + 4);
        for (int m = 0; m < nms && m < 4096; m++) {
            const uint32_t ms = msl + (uint32_t)m * 0x24;
            const int im = (int)psp_read32(ms + 24), in = (int)psp_read32(ms + 16), iv = (int)psp_read32(ms + 28);
            if (im >= 0 && im < 1024 && mat_sub[im] < 0) { mat_sub[im] = s; mat_node[im] = in; }
            if (iv >= 0 && iv < nvl && psp_read32(psp_read32(vls + (uint32_t)iv * 8 + 4) + 0x1C)) skin_node = in;
        }
    }

    /* The game material of each meshset, in the order EXTRACT MODELS writes them
     * as meshes: a GLB from before materials carried their number (extras) has
     * one mesh per meshset, and that order is how its materials are known. */
    static int ms_mat[8192], ms_num[8192], ms_vl[8192], ms_sub[8192];
    static uint32_t ms_addr[8192];
    int nms_all = 0;
    for (int s = 0; s < nsub; s++) {
        const uint32_t so = subs + (uint32_t)s * 0x14, msl = psp_read32(so + 8);
        const int nms = (int)psp_read32(so + 4);
        for (int m = 0; m < nms && m < 4096 && nms_all < 8192; m++) {
            const int iv = (int)psp_read32(msl + (uint32_t)m * 0x24 + 28);
            if (iv < 0 || iv >= nvl) continue;
            ms_num[nms_all] = (int)psp_read32(msl + (uint32_t)m * 0x24 + 32);
            ms_vl[nms_all] = iv;
            ms_sub[nms_all] = s;
            ms_addr[nms_all] = msl + (uint32_t)m * 0x24;
            ms_mat[nms_all++] = (int)psp_read32(msl + (uint32_t)m * 0x24 + 24);
        }
    }
    const js *gmeshes = js_get(g.root, "meshes");
    const int by_order = gmeshes && gmeshes->n == nms_all;

    /* glTF nodes -> game nodes; parents */
    const js *gn = js_get(g.root, "nodes");
    const int ngn = gn ? gn->n : 0;
    game_of = (int *)malloc(sizeof(int) * (size_t)(ngn + 1));
    gparent = (int *)malloc(sizeof(int) * (size_t)(ngn + 1));
    named = (int *)malloc(sizeof(int) * (size_t)(ngn + 1));
    moved = (char *)calloc((size_t)ngn + 1, 1);
    for (int i = 0; i < ngn; i++) {
        const char *nm = js_str(js_get(js_at(gn, i), "name"));
        int k = -1;
        named[i] = nm && sscanf(nm, "node%d", &k) == 1;
        if (!named[i] || k < 0 || k >= nnode) k = -1;
        game_of[i] = k; gparent[i] = -1;
    }
    for (int i = 0; i < ngn; i++) {
        const js *ch = js_get(js_at(gn, i), "children");
        for (int c = 0; ch && c < ch->n; c++) { const int k = js_int(js_at(ch, c), -1); if (k >= 0 && k < ngn) gparent[k] = i; }
    }
    const js *joints = js_get(js_at(js_get(g.root, "skins"), 0), "joints");
    const js *gmats = js_get(g.root, "materials");

    /* The skeleton. A named node should sit where the game's node of that number does;
     * when it doesn't (a GLB from another model), it is matched to the game's nearest
     * node (bones: the nearest with a matrix) and what hangs on it is re-posed onto it. */
    gw = (float (*)[16])malloc(sizeof *gw * (size_t)(ngn + 1));
    {
        for (int i = 0; i < nnode; i++) {
            float inv[16];
            for (int k = 0; k < 16; k++) inv[k] = rf(nodesp + (uint32_t)i * 0x90 + 0x30 + (uint32_t)k * 4);
            if (mat_inv(inv, bindw[i]) != 0) memcpy(bindw[i], IDENT, sizeof IDENT);
        }
        char *done = (char *)calloc((size_t)ngn + 1, 1);
        for (int pass = 0, left = ngn; left && pass <= ngn; pass++)        /* rest worlds, parents first */
            for (int i = 0; i < ngn; i++) {
                if (done[i] || (gparent[i] >= 0 && !done[gparent[i]])) continue;
                float l[16];
                node_matrix(js_at(gn, i), l);
                if (gparent[i] >= 0) mat_mul(gw[gparent[i]], l, gw[i]); else memcpy(gw[i], l, sizeof l);
                done[i] = 1; left--;
            }
        for (int i = 0; i < ngn; i++) if (!done[i]) memcpy(gw[i], IDENT, sizeof IDENT);
        free(done);
        float ext = 0;
        for (int i = 0; i < nnode; i++)
            for (int k = 0; k < 3; k++) if (fabsf(bindw[i][12 + k]) > ext) ext = fabsf(bindw[i][12 + k]);
        const float tol = ext * 0.002f + 0.001f;
        char *is_joint = (char *)calloc((size_t)ngn + 1, 1);
        for (int j = 0; joints && j < joints->n; j++) { const int k = js_int(js_at(joints, j), -1); if (k >= 0 && k < ngn) is_joint[k] = 1; }
        for (int i = 0; i < ngn; i++) {
            if (!named[i]) continue;
            const int k = game_of[i];
            if (k >= 0) {
                float d = 0;
                for (int c = 0; c < 3; c++) d += (gw[i][12 + c] - bindw[k][12 + c]) * (gw[i][12 + c] - bindw[k][12 + c]);
                if (d <= tol * tol) continue;
            }
            int best = -1;
            float bd = 0;
            for (int n = 0; n < nnode; n++) {
                if (is_joint[i] && imatrix[n] < 0) continue;
                float d = 0;
                for (int c = 0; c < 3; c++) d += (gw[i][12 + c] - bindw[n][12 + c]) * (gw[i][12 + c] - bindw[n][12 + c]);
                if (best < 0 || d < bd) { best = n; bd = d; }
            }
            game_of[i] = best >= 0 ? best : 0;
            moved[i] = 1; nmoved++;
        }
        free(is_joint);
        if (nmoved) fprintf(stderr, "models: %s: %d nodes are another model's; matched to this one's by position\n", name, nmoved);
    }

    /* Materials. A GLB material is the game material it names (extras) when that one
     * uses its image; else a game material using the texture of the image's name. An
     * image this model doesn't have (another model's) takes over one of its texture
     * slots, matched by name ending (_body, _face...), else the next free one. */
    static char tnames[MAXTEX][48];
    const int ntex = model_textures(r, tnames, MAXTEX);
    const int ngm = gmats ? gmats->n : 0;
    gmat_of = (int *)malloc(sizeof(int) * (size_t)(ngm + 1));
    {
        static int slot_img[MAXTEX];
        for (int t = 0; t < MAXTEX; t++) slot_img[t] = -1;
        const js *texs = js_get(g.root, "textures"), *imgs = js_get(g.root, "images");
        for (int m = 0; m < ngm; m++) {
            const js *gm = js_at(gmats, m);
            const int ti = js_int(js_get(js_get(js_get(gm, "pbrMetallicRoughness"), "baseColorTexture"), "index"), -1);
            const int src = ti >= 0 ? js_int(js_get(js_at(texs, ti), "source"), -1) : -1;
            const char *in = src >= 0 ? js_str(js_get(js_at(imgs, src), "name")) : NULL;
            const char *am = js_str(js_get(gm, "alphaMode"));
            const int want_alpha = am && !strcmp(am, "MASK") ? 1 : am && !strcmp(am, "BLEND") ? 2 : 0;
            int t = -1;
            if (src >= 0 && ntex > 0) {
                for (int k = 0; k < ntex && t < 0; k++) if (in && !strcmp(in, tnames[k])) t = k;
                for (int k = 0; k < ntex && t < 0; k++) if (slot_img[k] == src) t = k;
                if (t < 0 && in && own_image(in, name)) t = -2;          /* ours, the list not read yet */
                if (t == -1) {                                              /* another model's image: a free slot */
                    const char *e1 = in ? strrchr(in, '_') : NULL;
                    for (int k = 0; k < ntex && t < 0 && e1; k++) {
                        const char *e2 = strrchr(tnames[k], '_');
                        if (slot_img[k] < 0 && e2 && !strcmp(e1, e2)) t = k;
                    }
                    for (int k = 0; k < ntex && t < 0; k++) {
                        int mine = 0;                                     /* not one this GLB uses by name */
                        for (int q = 0; imgs && q < imgs->n; q++) {
                            const char *qn = js_str(js_get(js_at(imgs, q), "name"));
                            mine |= qn && !strcmp(qn, tnames[k]);
                        }
                        if (slot_img[k] < 0 && !mine) t = k;
                    }
                    if (t >= 0) {
                        size_t n = 0;
                        const uint8_t *png = glb_image_bytes(&g, src, &n);
                        if (png) { queue_image(tnames[t], png, n); nforeign++; }
                    } else t = 0;
                }
                if (t >= 0 && slot_img[t] < 0) slot_img[t] = src;
            }
            const int named_mat = js_int(js_get(js_get(gm, "extras"), "psp_material"), -1);
            int pick = named_mat >= 0 && named_mat < nmat ? named_mat : -1;
            matinfo mi;
            if (pick >= 0) {
                material_of(psp_read32(mats + (uint32_t)pick * 8 + 4), &mi);
                const int mt = mi.tex < 0 && ntex == 1 ? 0 : mi.tex;
                if (src >= 0 && t != -2 && mt != t) pick = -1;
            }
            for (int pass = 0; pass < 2 && pick < 0 && t != -2 && (src >= 0 || pass == 0); pass++)   /* same alpha use first */
                for (int k = 0; k < nmat && pick < 0; k++) {
                    material_of(psp_read32(mats + (uint32_t)k * 8 + 4), &mi);
                    const int mt = mi.tex < 0 && ntex == 1 ? 0 : mi.tex;
                    if (src >= 0 ? mt == t && (pass || mi.alpha == want_alpha) : mi.tex < 0) pick = k;
                }
            gmat_of[m] = pick >= 0 ? pick : named_mat >= 0 && named_mat < nmat ? named_mat : 0;
        }
        if (nforeign) fprintf(stderr, "models: %s: %d images are another model's; they replace this one's textures\n", name, nforeign);
    }

    for (int ni = 0; ni < ngn; ni++) {
        const js *nd = js_at(gn, ni);
        const js *mesh = js_at(js_get(g.root, "meshes"), js_int(js_get(nd, "mesh"), -1));
        if (!mesh) continue;
        const int skinned_node = js_get(nd, "skin") != NULL && joints;
        /* a rigid mesh: the game node it hangs under (through un-named nodes), and its transform to it */
        float M[16];
        memcpy(M, IDENT, sizeof M);
        int rnode = 0;
        for (int p = ni; p >= 0; p = gparent[p]) {
            if (game_of[p] >= 0) {
                rnode = game_of[p];
                if (moved[p]) {                                       /* game node <- model space <- its node */
                    float inv[16];
                    if (mat_inv(bindw[rnode], inv) == 0) { mat_mul(gw[p], M, M); mat_mul(inv, M, M); }
                }
                break;
            }
            float l[16];
            node_matrix(js_at(gn, p), l);
            mat_mul(l, M, M);
        }
        const js *prims = js_get(mesh, "primitives");
        for (int pi = 0; prims && pi < prims->n; pi++) {
            const js *pr = js_at(prims, pi), *at = js_get(pr, "attributes");
            if (js_int(js_get(pr, "mode"), 4) != 4) continue;                 /* triangles only */
            int np = 0, c = 0, ncn = 0, nuv = 0, ncl = 0, nj = 0, nw = 0, ni2 = 0, cc = 0, colc = 0, jc = 0, wc = 0;
            float *pos = glb_floats(&g, js_int(js_get(at, "POSITION"), -1), &np, &c);
            if (!pos || c != 3) { free(pos); continue; }
            float *nrm = js_get(at, "NORMAL") ? glb_floats(&g, js_int(js_get(at, "NORMAL"), -1), &ncn, &cc) : NULL;
            float *uv = js_get(at, "TEXCOORD_0") ? glb_floats(&g, js_int(js_get(at, "TEXCOORD_0"), -1), &nuv, &cc) : NULL;
            float *col = js_get(at, "COLOR_0") ? glb_floats(&g, js_int(js_get(at, "COLOR_0"), -1), &ncl, &colc) : NULL;
            uint32_t *jn = skinned_node && js_get(at, "JOINTS_0") ? glb_uints(&g, js_int(js_get(at, "JOINTS_0"), -1), &nj, &jc) : NULL;
            float *wt = jn && js_get(at, "WEIGHTS_0") ? glb_floats(&g, js_int(js_get(at, "WEIGHTS_0"), -1), &nw, &wc) : NULL;
            uint32_t *idx = js_get(pr, "indices") ? glb_uints(&g, js_int(js_get(pr, "indices"), -1), &ni2, &cc) : NULL;
            if (nrm && ncn != np) { free(nrm); nrm = NULL; }
            if (uv && nuv != np) { free(uv); uv = NULL; }
            if (col && (ncl != np || colc < 3)) { free(col); col = NULL; }
            if (!idx) { ni2 = np; idx = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)np); for (int i = 0; i < np; i++) idx[i] = (uint32_t)i; }
            const int skinned = jn && wt && nj == np && nw == np && jc == 4 && wc == 4;
            /* the game material */
            const int gmi = js_int(js_get(pr, "material"), -1), meshi = js_int(js_get(nd, "mesh"), -1);
            int imat = gmi >= 0 && gmi < ngm ? gmat_of[gmi] : 0;
            if (by_order && meshi >= 0 && meshi < nms_all && ms_mat[meshi] >= 0 && ms_mat[meshi] < nmat && !nmoved && !nforeign &&
                !js_get(js_get(js_at(gmats, gmi), "extras"), "psp_material"))
                imat = ms_mat[meshi];                                 /* an untagged GLB: its meshset's */
            matinfo mi;
            material_of(psp_read32(mats + (uint32_t)imat * 8 + 4), &mi);
            /* each triangle into a list with room for its bones */
            int cur = -1;
            int *seen_list = (int *)malloc(sizeof(int) * (size_t)(np + 1)), *seen_at = (int *)malloc(sizeof(int) * (size_t)(np + 1));
            for (int i = 0; i < np; i++) seen_list[i] = -1;
            for (int t = 0; t + 2 < ni2; t += 3) {
                const uint32_t tv[3] = { idx[t], idx[t + 1], idx[t + 2] };
                if (tv[0] >= (uint32_t)np || tv[1] >= (uint32_t)np || tv[2] >= (uint32_t)np) continue;
                int tb[12], ntb = 0;                                         /* the triangle's palette indices */
                for (int q = 0; skinned && q < 3; q++)
                    for (int k = 0; k < 4; k++) {
                        if (wt[tv[q] * 4 + (uint32_t)k] <= 0.0f) continue;
                        const int pal = joint_palette(joints, jn[tv[q] * 4 + (uint32_t)k], game_of, ngn, imatrix, parent);
                        int seen = 0;
                        for (int z = 0; z < ntb; z++) seen |= tb[z] == pal;
                        if (!seen && ntb < 12) tb[ntb++] = pal;
                    }
                int fits = cur >= 0;
                if (fits) {
                    int need = lists[cur].nb;
                    for (int z = 0; z < ntb; z++) { int in = 0; for (int y = 0; y < lists[cur].nb; y++) in |= lists[cur].bones[y] == tb[z]; need += !in; }
                    fits = need <= 4 && lists[cur].nt < 20000;
                }
                if (!fits) {
                    newlist *nl2 = (newlist *)realloc(lists, sizeof *lists * (size_t)(nlists + 1));
                    if (!nl2) break;
                    lists = nl2;
                    memset(&lists[nlists], 0, sizeof lists[nlists]);
                    lists[nlists].material = imat;
                    lists[nlists].skinned = skinned;
                    lists[nlists].node = skinned ? skin_node : rnode;
                    lists[nlists].has_col = col != NULL;
                    lists[nlists].has_nrm = nrm != NULL;
                    lists[nlists].mesh = js_int(js_get(nd, "mesh"), -1);
                    cur = nlists++;
                }
                newlist *l = &lists[cur];
                for (int z = 0; z < ntb; z++) {
                    int in = 0;
                    for (int y = 0; y < l->nb; y++) in |= l->bones[y] == tb[z];
                    if (!in && l->nb < 4) l->bones[l->nb++] = tb[z];
                }
                if (skinned && !l->nb) l->bones[l->nb++] = 0;
                uint32_t out[3];
                for (int q = 0; q < 3; q++) {
                    const uint32_t v = tv[q];
                    if (seen_list && seen_at && seen_list[v] == cur) { out[q] = (uint32_t)seen_at[v]; continue; }
                    float f[16] = { 0 };
                    if (skinned) {
                        float s = 0;
                        for (int k = 0; k < 4; k++) {
                            const float wv = wt[v * 4 + (uint32_t)k];
                            if (wv <= 0) continue;
                            const int pal = joint_palette(joints, jn[v * 4 + (uint32_t)k], game_of, ngn, imatrix, parent);
                            for (int y = 0; y < l->nb; y++) if (l->bones[y] == pal) { f[y] += wv; s += wv; }
                        }
                        if (s > 0) for (int y = 0; y < 4; y++) f[y] /= s;
                        else f[0] = 1;
                    }
                    const float u = uv ? uv[v * 2] : 0, vv = uv ? uv[v * 2 + 1] : 0;
                    f[4] = (u - mi.uvs[2]) / (mi.uvs[0] != 0 ? mi.uvs[0] : 1);
                    f[5] = (vv - mi.uvs[3]) / (mi.uvs[1] != 0 ? mi.uvs[1] : 1);
                    for (int k = 0; k < 4; k++) f[6 + k] = col ? (k < colc ? col[v * (uint32_t)colc + (uint32_t)k] : 1.0f) : 1.0f;
                    const float *P = pos + v * 3, *N = nrm ? nrm + v * 3 : NULL;
                    float RP[3], RN[3];
                    if (skinned && nmoved) {          /* from its bones' rest to the matched game bones' bind */
                        float sw = 0;
                        for (int k = 0; k < 3; k++) RP[k] = RN[k] = 0;
                        for (int k = 0; k < 4; k++) {
                            const float wv = wt[v * 4 + (uint32_t)k];
                            const int jnode = js_int(js_at(joints, (int)jn[v * 4 + (uint32_t)k]), -1);
                            if (wv <= 0 || jnode < 0 || jnode >= ngn) continue;
                            float R[16], inv[16];
                            if (!moved[jnode] || mat_inv(gw[jnode], inv) != 0) memcpy(R, IDENT, sizeof R);
                            else mat_mul(bindw[game_of[jnode]], inv, R);
                            for (int c = 0; c < 3; c++) {
                                RP[c] += wv * (R[c] * P[0] + R[4 + c] * P[1] + R[8 + c] * P[2] + R[12 + c]);
                                if (N) RN[c] += wv * (R[c] * N[0] + R[4 + c] * N[1] + R[8 + c] * N[2]);
                            }
                            sw += wv;
                        }
                        if (sw > 0) {
                            const float nl2 = sqrtf(RN[0] * RN[0] + RN[1] * RN[1] + RN[2] * RN[2]);
                            for (int c = 0; c < 3; c++) { RP[c] /= sw; if (nl2 > 0) RN[c] /= nl2; }
                            P = RP;
                            if (N && nl2 > 0) N = RN;
                        }
                    }
                    for (int k = 0; k < 3; k++) {
                        f[13 + k] = skinned ? P[k] : M[k] * P[0] + M[4 + k] * P[1] + M[8 + k] * P[2] + M[12 + k];
                        f[10 + k] = N ? (skinned ? N[k] : M[k] * N[0] + M[4 + k] * N[1] + M[8 + k] * N[2]) : (k == 1 ? 1.0f : 0.0f);
                    }
                    out[q] = (uint32_t)nl_vertex(l, f);
                    if (seen_list && seen_at) { seen_list[v] = cur; seen_at[v] = (int)out[q]; }
                }
                nl_triangle(l, out[0], out[1], out[2]);
            }
            free(pos); free(nrm); free(uv); free(col); free(jn); free(wt); free(idx); free(seen_list); free(seen_at);
        }
    }
    if (!nlists) { fprintf(stderr, "models: %s: no triangles in %s\n", name, path); goto out; }

    for (int i = 0; i < nlists; i++) nl_build(&lists[i]);

    /* the game structures */
    {
        /* The game's own layout when each mesh became one list of a GLB with one
         * mesh per meshset (a round trip, or an edit that kept the meshes): each
         * list at its old number, in its old sub-object and place, with the old
         * nodes -- the object as the game built it, but for the geometry. */
        int same = by_order && !nmoved && !nforeign && nlists == nms_all && nlists == nvl;
        int *list_of = (int *)malloc(sizeof(int) * (size_t)(nms_all + 1));
        for (int k = 0; list_of && k < nms_all; k++) list_of[k] = -1;
        if (!list_of) same = 0;
        for (int i = 0; same && i < nlists; i++) {
            const int k = lists[i].mesh;
            if (k < 0 || k >= nms_all || list_of[k] >= 0 || ms_vl[k] < 0 || ms_vl[k] >= nvl) same = 0;
            else list_of[k] = i;
        }
        if (same) {                                                    /* the old list numbers: each once */
            char *used = (char *)calloc((size_t)nvl + 1, 1);
            for (int k = 0; used && k < nms_all && same; k++) { if (used[ms_vl[k]]) same = 0; used[ms_vl[k]] = 1; }
            free(used);
        }
        const uint32_t vlist = pool_alloc((uint32_t)nlists * 8), sublist = pool_alloc((uint32_t)nsub * 0x14), o2 = pool_alloc(0x60);
        /* Meshset numbers (+32): the game's own for a GLB with one mesh per meshset,
         * else new ones. One primitive entry per number. */
        const int keep_num = by_order && !nmoved && !nforeign;
        const int nprim0 = (int)psp_read32(obj + 0x20);
        int nnum = nlists > nprim0 ? nlists : nprim0;
        for (int i = 0; keep_num && i < nms_all; i++) if (ms_num[i] + 1 > nnum) nnum = ms_num[i] + 1;
        const uint32_t plist = pool_alloc((uint32_t)nnum * 8);
        if (!vlist || !sublist || !o2 || !plist) { fprintf(stderr, "models: host RAM for mod models is full\n"); goto out; }
        int count[16] = { 0 };
        /* the model's position scale: the game's (all its lists share one), or
         * larger when the new geometry reaches further */
        float pscale = 0;
        for (int i = 0; i < nvl; i++) {
            const uint32_t v0 = psp_read32(vls + (uint32_t)i * 8 + 4);
            if (((psp_read32(v0 + 4) >> 7) & 3u) == 2u) { const float t = psp_read_f32(v0 + 0x2C); if (t > pscale) pscale = t; }
        }
        for (int i = 0; i < nlists; i++)
            for (int q = 0; q < lists[i].nv; q++)
                for (int k = 0; k < 3; k++) { const float a = fabsf(lists[i].v[16 * q + 13 + k]) * 1.0001f; if (a > pscale) pscale = a; }
        if (pscale <= 0) pscale = 1.0f;
        int *vidx = (int *)malloc(sizeof(int) * (size_t)(nlists + 1)), *sub_of = (int *)malloc(sizeof(int) * (size_t)(nlists + 1));
        if (!vidx || !sub_of) { free(vidx); free(sub_of); free(list_of); goto out; }
        for (int i = 0; i < nlists; i++) {
            const int k = lists[i].mesh;
            vidx[i] = same ? ms_vl[k] : i;
            sub_of[i] = same ? ms_sub[k] : lists[i].material < 1024 && mat_sub[lists[i].material] >= 0 ? mat_sub[lists[i].material] : 0;
            if (sub_of[i] < 0 || sub_of[i] >= 16) sub_of[i] = 0;
            const uint32_t tmpl = same ? psp_read32(vls + (uint32_t)vidx[i] * 8 + 4) : lists[i].skinned ? tmpl_skin : tmpl_rigid;
            const uint32_t v = write_vtxlist(&lists[i], tmpl, pscale);
            if (!v) { fprintf(stderr, "models: host RAM for mod models is full\n"); free(vidx); free(sub_of); free(list_of); goto out; }
            psp_write32(vlist + (uint32_t)vidx[i] * 8, same ? psp_read32(vls + (uint32_t)vidx[i] * 8) : lists[i].skinned ? tmpl_skin_t : tmpl_rigid_t);
            psp_write32(vlist + (uint32_t)vidx[i] * 8 + 4, v);
            count[sub_of[i]]++;
        }
        for (int s = 0; s < nsub; s++) {                              /* the sub-objects: the game's, with new meshsets */
            const uint32_t so = subs + (uint32_t)s * 0x14, nso = sublist + (uint32_t)s * 0x14;
            for (uint32_t k = 0; k < 0x14; k += 4) psp_write32(nso + k, psp_read32(so + k));
            const uint32_t msl = count[s] ? pool_alloc((uint32_t)count[s] * 0x24) : 0;
            if (count[s] && !msl) goto out;
            psp_write32(nso + 4, (uint32_t)count[s]);
            psp_write32(nso + 8, msl);
            int m = 0;
            for (int j = 0; j < nlists; j++) {
                const int i = same ? list_of[j] : j;                  /* (same: the old meshset order) */
                if (sub_of[i] != s) continue;
                const uint32_t ms = msl + (uint32_t)m++ * 0x24;
                float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
                for (int q = 0; q < lists[i].nv; q++)
                    for (int k = 0; k < 3; k++) { const float x = lists[i].v[16 * q + 13 + k]; if (x < mn[k]) mn[k] = x; if (x > mx[k]) mx[k] = x; }
                float r = 0;
                for (int k = 0; k < 3; k++) { psp_write_f32(ms + (uint32_t)k * 4, (mn[k] + mx[k]) / 2); r += (mx[k] - mn[k]) * (mx[k] - mn[k]); }
                psp_write_f32(ms + 12, sqrtf(r) / 2);
                const int im = lists[i].material;
                const int mnode = lists[i].skinned && im < 1024 && mat_node[im] >= 0 ? mat_node[im] : lists[i].node;
                psp_write32(ms + 16, same ? psp_read32(ms_addr[lists[i].mesh] + 16) : (uint32_t)mnode);
                psp_write32(ms + 20, same ? psp_read32(ms_addr[lists[i].mesh] + 20)
                                          : lists[i].skinned ? 0xFFFFFFFFu : (uint32_t)mnode);   /* rigid: the node it moves with */
                psp_write32(ms + 24, (uint32_t)im);
                psp_write32(ms + 28, (uint32_t)vidx[i]);
                const int mi2 = lists[i].mesh;
                psp_write32(ms + 32, (uint32_t)(keep_num && mi2 >= 0 && mi2 < nms_all ? ms_num[mi2] : i));
            }
        }
        for (uint32_t k = 0; k < 0x60; k += 4) psp_write32(o2 + k, psp_read32(obj + k));   /* the game's header: materials, nodes, bounds */
        psp_write32(o2 + 0x18, (uint32_t)nlists);
        psp_write32(o2 + 0x1C, vlist);
        for (int i = 0; i < nnum * 2; i++) psp_write32(plist + (uint32_t)i * 4, 0);
        psp_write32(o2 + 0x20, (uint32_t)nnum);
        psp_write32(o2 + 0x24, plist);
        psp_write32(o2 + 0x3C, sublist);
        newobj = o2;
        free(vidx); free(sub_of); free(list_of);
    }

    /* images named after textures: replacements */
    {
        const js *imgs = js_get(g.root, "images");
        int nrep = 0;
        for (int i = 0; imgs && i < imgs->n; i++) {
            const char *nm = js_str(js_get(js_at(imgs, i), "name"));
            const size_t l = nm ? strlen(nm) : 0;
            if (l < 5 || l >= 32 || strcmp(nm + l - 4, ".uvr")) continue;
            int ours = 0;                                             /* another model's: placed above */
            for (int k = 0; k < ntex; k++) ours |= !strcmp(nm, tnames[k]);
            if (!ours && ntex && !own_image(nm, name)) continue;
            size_t n = 0;
            const uint8_t *png = glb_image_bytes(&g, i, &n);
            if (png) { queue_image(nm, png, n); nrep++; }
        }
        (void)nrep;
    }
    {
        long ov = 0, nv2 = 0;                                         /* vertices drawn: the game's, ours */
        for (int i = 0; i < nvl; i++) ov += (long)psp_read32(psp_read32(vls + (uint32_t)i * 8 + 4) + 0x20);
        for (int i = 0; i < nlists; i++) nv2 += lists[i].ns;
        fprintf(stderr, "models: %s from %s (%d vertex lists, %ld vertices, the game's %d / %ld; %u KB of host RAM in use)\n",
                name, path, nlists, nv2, nvl, ov, (g_pool - PSP_HOST_RAM_BASE) / 1024u);
    }
out:
    for (int i = 0; i < nlists; i++) { free(lists[i].v); free(lists[i].strip); free(lists[i].tri); }
    free(lists);
    free(game_of); free(gparent); free(named); free(moved); free(gw); free(gmat_of);
    /* g stays read (glb_get) */
    return newobj;
}

/* The model resource's setup (resource, data, chunks): after it, the object at +0x10. */
static void hook_model_setup(void (*original)(void)) {
    const uint32_t res = psp_cpu.r[4];
    original();
    if (!g_load_on || !g_data_dir[0]) return;
    const uint32_t obj = psp_read32(res + 0x10);
    if (!ok_ptr(obj, 0x48) || obj >= PSP_HOST_RAM_BASE) return;
    const rec *r = NULL;
    for (int i = g_nrec - 1; i >= 0 && !r; i--) if (g_rec[i].res == res && g_rec[i].tag == TAG_UNJ) r = &g_rec[i];
    if (!r) return;
    char stem[48];
    snprintf(stem, sizeof stem, "%s", r->name);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    char path[800];
    snprintf(path, sizeof path, "%s/Data/%s/%s.glb", g_data_dir, group_of(stem), stem);
    if (!file_exists(path)) return;
    const double t0 = now_ms();
    const uint32_t o2 = import_model(path, obj, r);
    fprintf(stderr, "models: %s took %.1f ms\n", r->name, now_ms() - t0);
    if (o2) {
        psp_write32(res + 0x10, o2); g_loaded++;
        if (g_nswap < 512) { g_swap[g_nswap].mine = o2; g_swap[g_nswap].orig = obj; g_nswap++; }
        const char *dump = getenv("PSP2I_MODELX_DUMP");       /* dev: the model as loaded, back to GLB */
        if (dump && *dump) {
            char out[800];
            snprintf(out, sizeof out, "%s/%s.glb", dump, stem);
            export_model(r, o2, out);
        }
    }
    else g_load_failed++;
}

void modelx_start(const char *base_dir, int load) {
    snprintf(g_data_dir, sizeof g_data_dir, "%s", base_dir);
    g_load_on = load;
    if (load) read_ahead();
    psp_hook_set(0x08D59624u, hook_model_setup);
}
