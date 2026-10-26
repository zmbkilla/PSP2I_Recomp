/* glTF binary reading for model loading -- see glbread.h. */
#include "glbread.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- JSON ----------------------------------------------------------------------- */

typedef struct { const char *p, *end; int bad; } jsp;

static void ws(jsp *s) { while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')) s->p++; }

static char *parse_string(jsp *s) {
    if (s->p >= s->end || *s->p != '"') { s->bad = 1; return NULL; }
    s->p++;
    size_t cap = 16, n = 0;
    char *out = (char *)malloc(cap);
    while (s->p < s->end && *s->p != '"') {
        char c = *s->p++;
        if (c == '\\' && s->p < s->end) {
            c = *s->p++;
            if (c == 'n') c = '\n'; else if (c == 't') c = '\t'; else if (c == 'r') c = '\r';
            else if (c == 'b') c = '\b'; else if (c == 'f') c = '\f';
            else if (c == 'u') {                     /* \uXXXX: kept as '?' unless ASCII */
                unsigned v = 0;
                for (int k = 0; k < 4 && s->p < s->end; k++) { const char h = *s->p++; v = v * 16 + (unsigned)(h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0'); }
                c = v < 128 ? (char)v : '?';
            }
        }
        if (n + 2 > cap) { cap *= 2; out = (char *)realloc(out, cap); }
        out[n++] = c;
    }
    if (s->p >= s->end) { s->bad = 1; free(out); return NULL; }
    s->p++;
    out[n] = 0;
    return out;
}

static int parse_value(jsp *s, js *v, int depth);

static int parse_list(jsp *s, js *v, int obj, int depth) {
    s->p++;
    int cap = 0;
    v->type = obj ? JS_OBJ : JS_ARR;
    ws(s);
    if (s->p < s->end && *s->p == (obj ? '}' : ']')) { s->p++; return 0; }
    for (;;) {
        if (v->n == cap) { cap = cap ? cap * 2 : 8; v->kids = (js *)realloc(v->kids, (size_t)cap * sizeof *v->kids); }
        js *k = &v->kids[v->n];
        memset(k, 0, sizeof *k);
        ws(s);
        if (obj) {
            k->key = parse_string(s);
            ws(s);
            if (s->bad || s->p >= s->end || *s->p != ':') { s->bad = 1; return -1; }
            s->p++;
        }
        if (parse_value(s, k, depth + 1) != 0) return -1;
        v->n++;
        ws(s);
        if (s->p < s->end && *s->p == ',') { s->p++; continue; }
        if (s->p < s->end && *s->p == (obj ? '}' : ']')) { s->p++; return 0; }
        s->bad = 1;
        return -1;
    }
}

static int parse_value(jsp *s, js *v, int depth) {
    ws(s);
    if (s->p >= s->end || depth > 64) { s->bad = 1; return -1; }
    const char c = *s->p;
    if (c == '{') return parse_list(s, v, 1, depth);
    if (c == '[') return parse_list(s, v, 0, depth);
    if (c == '"') { v->type = JS_STR; v->str = parse_string(s); return s->bad ? -1 : 0; }
    if (!strncmp(s->p, "true", 4)) { v->type = JS_BOOL; v->num = 1; s->p += 4; return 0; }
    if (!strncmp(s->p, "false", 5)) { v->type = JS_BOOL; v->num = 0; s->p += 5; return 0; }
    if (!strncmp(s->p, "null", 4)) { v->type = JS_NULL; s->p += 4; return 0; }
    char *e;
    v->type = JS_NUM;
    v->num = strtod(s->p, &e);
    if (e == s->p) { s->bad = 1; return -1; }
    s->p = e;
    return 0;
}

static void js_free(js *v) {
    for (int i = 0; i < v->n; i++) js_free(&v->kids[i]);
    free(v->kids); free(v->str); free(v->key);
}

js *js_get(const js *o, const char *key) {
    if (!o || o->type != JS_OBJ) return NULL;
    for (int i = 0; i < o->n; i++) if (o->kids[i].key && !strcmp(o->kids[i].key, key)) return &o->kids[i];
    return NULL;
}
js *js_at(const js *a, int i) { return a && a->type == JS_ARR && i >= 0 && i < a->n ? &a->kids[i] : NULL; }
int js_int(const js *v, int dflt) { return v && (v->type == JS_NUM || v->type == JS_BOOL) ? (int)v->num : dflt; }
double js_num(const js *v, double dflt) { return v && v->type == JS_NUM ? v->num : dflt; }
const char *js_str(const js *v) { return v && v->type == JS_STR ? v->str : NULL; }

/* ---- GLB ------------------------------------------------------------------------ */

int glb_load(const char *path, glb *g) {
    memset(g, 0, sizeof *g);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 28 || n > (256L << 20)) { fclose(f); return -1; }
    g->file = (uint8_t *)malloc((size_t)n);
    if (!g->file || fread(g->file, 1, (size_t)n, f) != (size_t)n) { fclose(f); glb_free(g); return -1; }
    fclose(f);
    uint32_t h[5];
    memcpy(h, g->file, 20);
    if (h[0] != 0x46546C67u || h[1] != 2 || h[4] != 0x4E4F534Au || 20 + (size_t)h[3] > (size_t)n) { glb_free(g); return -1; }
    const size_t jl = h[3];
    size_t o = 20 + jl;
    if (o + 8 <= (size_t)n) {
        uint32_t bh[2];
        memcpy(bh, g->file + o, 8);
        if (bh[1] == 0x004E4942u && o + 8 + bh[0] <= (size_t)n) { g->bin = g->file + o + 8; g->nbin = bh[0]; }
    }
    g->root = (js *)calloc(1, sizeof *g->root);
    jsp s = { (const char *)g->file + 20, (const char *)g->file + 20 + jl, 0 };
    if (parse_value(&s, g->root, 0) != 0 || g->root->type != JS_OBJ) { glb_free(g); return -1; }
    return 0;
}

void glb_free(glb *g) {
    if (g->root) { js_free(g->root); free(g->root); }
    free(g->file);
    memset(g, 0, sizeof *g);
}

static const uint8_t *view_bytes(const glb *g, int view, size_t *len) {
    const js *v = js_at(js_get(g->root, "bufferViews"), view);
    if (!v || js_int(js_get(v, "buffer"), 0) != 0 || !g->bin) return NULL;
    const size_t off = (size_t)js_num(js_get(v, "byteOffset"), 0), n = (size_t)js_num(js_get(v, "byteLength"), 0);
    if (off + n > g->nbin) return NULL;
    *len = n;
    return g->bin + off;
}

static int ncomp_of(const char *t) {
    if (!t) return 0;
    if (!strcmp(t, "SCALAR")) return 1;
    if (!strcmp(t, "VEC2")) return 2;
    if (!strcmp(t, "VEC3")) return 3;
    if (!strcmp(t, "VEC4")) return 4;
    if (!strcmp(t, "MAT4")) return 16;
    return 0;
}

/* One accessor's elements, as doubles through `put`. */
static int read_acc(const glb *g, int index, int *count, int *ncomp, double **out) {
    const js *a = js_at(js_get(g->root, "accessors"), index);
    if (!a) return -1;
    const int nc = ncomp_of(js_str(js_get(a, "type"))), ct = js_int(js_get(a, "componentType"), 0), n = js_int(js_get(a, "count"), 0);
    const int norm = js_int(js_get(a, "normalized"), 0);
    const int cs = ct == 5126 || ct == 5125 ? 4 : ct == 5123 || ct == 5122 ? 2 : ct == 5121 || ct == 5120 ? 1 : 0;
    if (!nc || !cs || n <= 0 || n > (1 << 24)) return -1;
    size_t len;
    const uint8_t *p = view_bytes(g, js_int(js_get(a, "bufferView"), -1), &len);
    if (!p) return -1;
    const js *v = js_at(js_get(g->root, "bufferViews"), js_int(js_get(a, "bufferView"), -1));
    const size_t stride = (size_t)js_int(js_get(v, "byteStride"), nc * cs), off = (size_t)js_num(js_get(a, "byteOffset"), 0);
    if (off + stride * (size_t)(n - 1) + (size_t)(nc * cs) > len) return -1;
    double *o = (double *)malloc(sizeof(double) * (size_t)n * (size_t)nc);
    if (!o) return -1;
    for (int i = 0; i < n; i++)
        for (int c = 0; c < nc; c++) {
            const uint8_t *e = p + off + stride * (size_t)i + (size_t)(c * cs);
            double x = 0;
            switch (ct) {
            case 5126: { float f; memcpy(&f, e, 4); x = f; break; }
            case 5125: { uint32_t u; memcpy(&u, e, 4); x = u; break; }
            case 5123: { uint16_t u; memcpy(&u, e, 2); x = norm ? u / 65535.0 : u; break; }
            case 5122: { int16_t u; memcpy(&u, e, 2); x = norm ? fmax(u / 32767.0, -1.0) : u; break; }
            case 5121: x = norm ? e[0] / 255.0 : e[0]; break;
            case 5120: x = norm ? fmax((int8_t)e[0] / 127.0, -1.0) : (int8_t)e[0]; break;
            }
            o[(size_t)i * nc + c] = x;
        }
    *count = n; *ncomp = nc; *out = o;
    return 0;
}

float *glb_floats(const glb *g, int index, int *count, int *ncomp) {
    double *d;
    if (read_acc(g, index, count, ncomp, &d) != 0) return NULL;
    const size_t n = (size_t)*count * (size_t)*ncomp;
    float *f = (float *)malloc(sizeof(float) * n);
    for (size_t i = 0; f && i < n; i++) f[i] = (float)d[i];
    free(d);
    return f;
}

uint32_t *glb_uints(const glb *g, int index, int *count, int *ncomp) {
    double *d;
    if (read_acc(g, index, count, ncomp, &d) != 0) return NULL;
    const size_t n = (size_t)*count * (size_t)*ncomp;
    uint32_t *u = (uint32_t *)malloc(sizeof(uint32_t) * n);
    for (size_t i = 0; u && i < n; i++) u[i] = d[i] < 0 ? 0 : (uint32_t)d[i];
    free(d);
    return u;
}

const uint8_t *glb_image_bytes(const glb *g, int index, size_t *len) {
    const js *im = js_at(js_get(g->root, "images"), index);
    return im ? view_bytes(g, js_int(js_get(im, "bufferView"), -1), len) : NULL;
}

uint8_t *glb_image_rgba(const glb *g, int index, int *w, int *h) {
    const js *im = js_at(js_get(g->root, "images"), index);
    if (!im) return NULL;
    size_t len;
    const uint8_t *p = view_bytes(g, js_int(js_get(im, "bufferView"), -1), &len);
    return p ? png_decode(p, len, w, h) : NULL;
}

/* ---- inflate (RFC 1951) ----------------------------------------------------------- */

typedef struct { const uint8_t *p, *end; uint32_t bits; int nbits; int bad; } bits_in;

static int getbits(bits_in *b, int n) {
    while (b->nbits < n) {
        if (b->p >= b->end) { b->bad = 1; return 0; }
        b->bits |= (uint32_t)*b->p++ << b->nbits;
        b->nbits += 8;
    }
    const int v = (int)(b->bits & ((1u << n) - 1));
    b->bits >>= n; b->nbits -= n;
    return v;
}

typedef struct { uint16_t count[16], sym[288]; } huff;

static void huff_build(huff *h, const uint8_t *len, int n) {
    uint16_t offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (uint16_t)i;
}

static int huff_decode(bits_in *b, const huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= getbits(b, 1);
        const int count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count; first += count;
        first <<= 1; code <<= 1;
        if (b->bad) return -1;
    }
    b->bad = 1;
    return -1;
}

static const uint16_t LBASE[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t LEXT[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t DBASE[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t DEXT[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

/* zlib stream -> malloc'd bytes (*n), or NULL. */
static uint8_t *zinflate(const uint8_t *p, size_t len, size_t *n) {
    if (len < 6) return NULL;
    bits_in b = { p + 2, p + len, 0, 0, 0 };
    size_t cap = len * 4 + 1024, o = 0;
    uint8_t *out = (uint8_t *)malloc(cap);
    if (!out) return NULL;
#define PUT(c) do { if (o == cap) { cap *= 2; uint8_t *q_ = (uint8_t *)realloc(out, cap); if (!q_) { free(out); return NULL; } out = q_; } out[o++] = (uint8_t)(c); } while (0)
    int last;
    do {
        last = getbits(&b, 1);
        const int type = getbits(&b, 2);
        if (type == 0) {
            b.bits = 0; b.nbits = 0;
            if (b.p + 4 > b.end) break;
            const unsigned l = b.p[0] | b.p[1] << 8;
            b.p += 4;
            if (b.p + l > b.end) { b.bad = 1; break; }
            for (unsigned i = 0; i < l; i++) PUT(b.p[i]);
            b.p += l;
        } else if (type == 1 || type == 2) {
            huff lit, dist;
            uint8_t lens[320];
            if (type == 1) {
                for (int i = 0; i < 144; i++) lens[i] = 8;
                for (int i = 144; i < 256; i++) lens[i] = 9;
                for (int i = 256; i < 280; i++) lens[i] = 7;
                for (int i = 280; i < 288; i++) lens[i] = 8;
                huff_build(&lit, lens, 288);
                for (int i = 0; i < 30; i++) lens[i] = 5;
                huff_build(&dist, lens, 30);
            } else {
                static const uint8_t ORD[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
                const int hlit = getbits(&b, 5) + 257, hdist = getbits(&b, 5) + 1, hclen = getbits(&b, 4) + 4;
                uint8_t cl[19] = { 0 };
                for (int i = 0; i < hclen; i++) cl[ORD[i]] = (uint8_t)getbits(&b, 3);
                huff clh;
                huff_build(&clh, cl, 19);
                int i = 0;
                while (i < hlit + hdist && !b.bad) {
                    const int sym = huff_decode(&b, &clh);
                    if (sym < 16) lens[i++] = (uint8_t)sym;
                    else {
                        int rep = 0, v = 0;
                        if (sym == 16) { if (!i) { b.bad = 1; break; } v = lens[i - 1]; rep = 3 + getbits(&b, 2); }
                        else if (sym == 17) rep = 3 + getbits(&b, 3);
                        else rep = 11 + getbits(&b, 7);
                        while (rep-- && i < hlit + hdist) lens[i++] = (uint8_t)v;
                    }
                }
                huff_build(&lit, lens, hlit);
                huff_build(&dist, lens + hlit, hdist);
            }
            for (;;) {
                const int sym = huff_decode(&b, &lit);
                if (b.bad || sym < 0) break;
                if (sym < 256) { PUT(sym); continue; }
                if (sym == 256) break;
                const int li = sym - 257;
                if (li >= 29) { b.bad = 1; break; }
                const int l = LBASE[li] + getbits(&b, LEXT[li]);
                const int di = huff_decode(&b, &dist);
                if (di < 0 || di >= 30) { b.bad = 1; break; }
                const size_t d = DBASE[di] + (size_t)getbits(&b, DEXT[di]);
                if (d > o) { b.bad = 1; break; }
                for (int k = 0; k < l; k++) PUT(out[o - d]);
            }
        } else b.bad = 1;
    } while (!last && !b.bad);
#undef PUT
    if (b.bad) { free(out); return NULL; }
    *n = o;
    return out;
}

/* ---- PNG ------------------------------------------------------------------------- */

static uint32_t be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

uint8_t *png_decode(const uint8_t *p, size_t n, int *pw, int *ph) {
    static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    if (n < 33 || memcmp(p, SIG, 8)) return NULL;
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = 0, inter = 0;
    uint8_t pal[256][4];
    int npal = 0;
    uint8_t *idat = NULL;
    size_t nidat = 0;
    for (size_t o = 8; o + 12 <= n; ) {
        const uint32_t len = be(p + o);
        const uint8_t *t = p + o + 4, *d = p + o + 8;
        if (o + 12 + len > n) break;
        if (!memcmp(t, "IHDR", 4) && len >= 13) { w = be(d); h = be(d + 4); depth = d[8]; ctype = d[9]; inter = d[12]; }
        else if (!memcmp(t, "PLTE", 4)) { npal = (int)(len / 3 > 256 ? 256 : len / 3); for (int i = 0; i < npal; i++) { pal[i][0] = d[i * 3]; pal[i][1] = d[i * 3 + 1]; pal[i][2] = d[i * 3 + 2]; pal[i][3] = 255; } }
        else if (!memcmp(t, "tRNS", 4) && ctype == 3) { for (uint32_t i = 0; i < len && i < 256; i++) pal[i][3] = d[i]; }
        else if (!memcmp(t, "IDAT", 4)) { uint8_t *q = (uint8_t *)realloc(idat, nidat + len); if (!q) { free(idat); return NULL; } idat = q; memcpy(idat + nidat, d, len); nidat += len; }
        else if (!memcmp(t, "IEND", 4)) break;
        o += 12 + len;
    }
    if (!w || !h || w > 8192 || h > 8192 || depth != 8 || inter || !idat) { free(idat); return NULL; }
    const int ch = ctype == 6 ? 4 : ctype == 2 ? 3 : ctype == 4 ? 2 : ctype == 0 || ctype == 3 ? 1 : 0;
    if (!ch) { free(idat); return NULL; }
    size_t nraw;
    uint8_t *raw = zinflate(idat, nidat, &nraw);
    free(idat);
    const size_t rowb = (size_t)w * ch;
    if (!raw || nraw < (rowb + 1) * h) { free(raw); return NULL; }
    uint8_t *img = (uint8_t *)malloc(rowb * h), *rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!img || !rgba) { free(raw); free(img); free(rgba); return NULL; }
    for (uint32_t y = 0; y < h; y++) {                   /* undo the filters */
        const uint8_t f = raw[y * (rowb + 1)], *src = raw + y * (rowb + 1) + 1;
        uint8_t *dst = img + y * rowb, *up = y ? dst - rowb : NULL;
        for (size_t x = 0; x < rowb; x++) {
            const int a = x >= (size_t)ch ? dst[x - ch] : 0, b = up ? up[x] : 0, c = up && x >= (size_t)ch ? up[x - ch] : 0;
            int v = src[x];
            switch (f) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: { const int pp = a + b - c, pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - c); v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c; break; }
            }
            dst[x] = (uint8_t)v;
        }
    }
    free(raw);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t *o = rgba + i * 4;
        const uint8_t *s = img + i * ch;
        switch (ctype) {
        case 6: memcpy(o, s, 4); break;
        case 2: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; break;
        case 4: o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; break;
        case 0: o[0] = o[1] = o[2] = s[0]; o[3] = 255; break;
        case 3: if (s[0] < npal) memcpy(o, pal[s[0]], 4); else memset(o, 0, 4); break;
        }
    }
    free(img);
    *pw = (int)w; *ph = (int)h;
    return rgba;
}
