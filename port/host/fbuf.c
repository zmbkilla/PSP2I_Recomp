/* A minimal FlatBuffers implementation (see fbuf.h).
 *
 * Format, as https://flatbuffers.dev/internals describes it: little-endian;
 * the buffer starts with a uoffset to the root table. A table starts with a
 * soffset to its vtable (vtable = table - soffset); the vtable is u16 vtable
 * size, u16 table size, then one u16 per field slot: the field's offset in
 * the table, 0 = absent (default). uoffsets point forward from where they
 * are stored. A vector / string is a u32 count followed by the elements (a
 * string also has a NUL after its bytes, not counted). Built back to front
 * so every referenced object lies after the reference. */

#include "fbuf.h"

#include <stdlib.h>
#include <string.h>

/* ---- builder ---------------------------------------------------------------------- */

void fb_init(fb_builder *b) { memset(b, 0, sizeof *b); b->minalign = 1; }
void fb_free(fb_builder *b) { free(b->buf); memset(b, 0, sizeof *b); }

/* Room for n more bytes at the front. */
static int grow(fb_builder *b, uint32_t n) {
    if (b->failed) return -1;
    if (b->size + n <= b->cap) return 0;
    uint32_t cap = b->cap ? b->cap : 1024;
    while (cap < b->size + n) cap *= 2;
    uint8_t *nb = (uint8_t *)malloc(cap);
    if (!nb) { b->failed = 1; return -1; }
    if (b->size) memcpy(nb + cap - b->size, b->buf + b->cap - b->size, b->size);
    free(b->buf);
    b->buf = nb;
    b->cap = cap;
    return 0;
}

static void push(fb_builder *b, const void *p, uint32_t n) {
    if (grow(b, n)) return;
    b->size += n;
    memcpy(b->buf + b->cap - b->size, p, n);
}

static void pad(fb_builder *b, uint32_t n) {
    static const uint8_t z[8];
    while (n) { const uint32_t k = n > 8 ? 8 : n; push(b, z, k); n -= k; }
}

/* Align so that after `extra` more bytes the size is a multiple of `align`. */
static void prep(fb_builder *b, uint32_t align, uint32_t extra) {
    if (align > b->minalign) b->minalign = align;
    pad(b, (uint32_t)(-(int32_t)(b->size + extra)) & (align - 1));
}

static void put32_at(fb_builder *b, uint32_t ref, uint32_t v) {
    uint8_t *p = b->buf + b->cap - ref;
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void push_u16(fb_builder *b, uint16_t v) { uint8_t p[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; push(b, p, 2); }
static void push_u32(fb_builder *b, uint32_t v) { uint8_t p[4]; for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); push(b, p, 4); }
static void push_u64(fb_builder *b, uint64_t v) { uint8_t p[8]; for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); push(b, p, 8); }

/* A uoffset to `target`, stored at the position about to be pushed. */
static void push_ref(fb_builder *b, fb_ref target) {
    prep(b, 4, 4);
    push_u32(b, b->size + 4 - target);
}

static fb_ref vec_scalars(fb_builder *b, const void *data, uint32_t n, uint32_t elem) {
    prep(b, 4, n * elem);
    prep(b, elem > 4 ? elem : 4, n * elem);
    if (n) push(b, data, n * elem);       /* little-endian host: the bytes as they are */
    push_u32(b, n);
    return b->size;
}

fb_ref fb_bytes(fb_builder *b, const void *data, uint32_t n) { return vec_scalars(b, data, n, 1); }
fb_ref fb_vec_u16(fb_builder *b, const uint16_t *v, uint32_t n) { return vec_scalars(b, v, n, 2); }
fb_ref fb_vec_u64(fb_builder *b, const uint64_t *v, uint32_t n) { return vec_scalars(b, v, n, 8); }

fb_ref fb_string(fb_builder *b, const char *s) {
    const uint32_t n = (uint32_t)strlen(s);
    prep(b, 4, n + 1);
    const uint8_t nul = 0;
    push(b, &nul, 1);
    if (n) push(b, s, n);
    push_u32(b, n);
    return b->size;
}

fb_ref fb_vec_refs(fb_builder *b, const fb_ref *v, uint32_t n) {
    prep(b, 4, 4 * n);
    for (uint32_t i = n; i-- > 0;) push_ref(b, v[i]);
    push_u32(b, n);
    return b->size;
}

void fb_table_start(fb_builder *b) { b->tstart = b->size; b->nfields = 0; }

static void field(fb_builder *b, int slot) {
    if (b->nfields < (int)(sizeof b->field / sizeof b->field[0])) {
        b->field[b->nfields].slot = slot;
        b->field[b->nfields].at = b->size;
        b->nfields++;
    } else b->failed = 1;
}

void fb_add_u8(fb_builder *b, int slot, uint8_t v)   { prep(b, 1, 1); push(b, &v, 1); field(b, slot); }
void fb_add_u16(fb_builder *b, int slot, uint16_t v) { prep(b, 2, 2); push_u16(b, v); field(b, slot); }
void fb_add_u32(fb_builder *b, int slot, uint32_t v) { prep(b, 4, 4); push_u32(b, v); field(b, slot); }
void fb_add_u64(fb_builder *b, int slot, uint64_t v) { prep(b, 8, 8); push_u64(b, v); field(b, slot); }
void fb_add_ref(fb_builder *b, int slot, fb_ref r)   { if (!r) return; push_ref(b, r); field(b, slot); }

fb_ref fb_table_end(fb_builder *b) {
    prep(b, 4, 4);
    push_u32(b, 0);                                   /* soffset to the vtable, patched below */
    const fb_ref table = b->size;
    int nslots = 0;
    for (int i = 0; i < b->nfields; i++) if (b->field[i].slot + 1 > nslots) nslots = b->field[i].slot + 1;
    for (int s = nslots - 1; s >= 0; s--) {
        uint16_t off = 0;
        for (int i = 0; i < b->nfields; i++) if (b->field[i].slot == s) off = (uint16_t)(table - b->field[i].at);
        push_u16(b, off);
    }
    push_u16(b, (uint16_t)(table - b->tstart));       /* the table's size */
    push_u16(b, (uint16_t)(4 + 2 * nslots));          /* the vtable's size */
    const fb_ref vt = b->size;
    if (!b->failed) put32_at(b, table, vt - table);   /* table - vtable, in bytes (vtable before it) */
    b->nfields = 0;
    return table;
}

const uint8_t *fb_finish(fb_builder *b, fb_ref root, uint32_t *len) {
    prep(b, b->minalign > 4 ? b->minalign : 4, 4);
    push_ref(b, root);
    if (b->failed) return NULL;
    *len = b->size;
    return b->buf + b->cap - b->size;
}

/* ---- reader ------------------------------------------------------------------------ */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

fb_table fb_root(const uint8_t *buf, uint32_t len) {
    fb_table t = { buf, len, 0 };
    if (len < 8) return t;
    const uint32_t p = rd32(buf);
    if (p + 4 <= len) t.pos = p;
    return t;
}

/* The field's absolute position, or 0. */
static uint32_t field_at(fb_table t, int slot, uint32_t size) {
    if (!t.pos || t.pos + 4 > t.len) return 0;
    const int32_t so = (int32_t)rd32(t.buf + t.pos);
    const int64_t vt = (int64_t)t.pos - so;
    if (vt < 0 || vt + 4 > t.len) return 0;
    const uint16_t vsize = rd16(t.buf + vt);
    const uint32_t e = 4u + 2u * (uint32_t)slot;
    if (e + 2 > vsize || vt + e + 2 > t.len) return 0;
    const uint16_t off = rd16(t.buf + vt + e);
    if (!off || t.pos + off + size > t.len) return 0;
    return t.pos + off;
}

int fb_has(fb_table t, int slot) { return field_at(t, slot, 1) != 0; }
uint8_t  fb_u8(fb_table t, int slot)  { const uint32_t p = field_at(t, slot, 1); return p ? t.buf[p] : 0; }
uint16_t fb_u16(fb_table t, int slot) { const uint32_t p = field_at(t, slot, 2); return p ? rd16(t.buf + p) : 0; }
uint32_t fb_u32(fb_table t, int slot) { const uint32_t p = field_at(t, slot, 4); return p ? rd32(t.buf + p) : 0; }
uint64_t fb_u64(fb_table t, int slot) { const uint32_t p = field_at(t, slot, 8); return p ? (uint64_t)rd32(t.buf + p) | (uint64_t)rd32(t.buf + p + 4) << 32 : 0; }

/* Follow the uoffset stored at absolute position p. */
static uint32_t deref(const uint8_t *buf, uint32_t len, uint32_t p) {
    if (!p || p + 4 > len) return 0;
    const uint32_t q = p + rd32(buf + p);
    return q < len ? q : 0;
}

fb_table fb_sub(fb_table t, int slot) {
    fb_table r = { t.buf, t.len, 0 };
    r.pos = deref(t.buf, t.len, field_at(t, slot, 4));
    if (r.pos + 4 > t.len) r.pos = 0;
    return r;
}

fb_vec fb_vec_of(fb_table t, int slot) {
    fb_vec v = { t.buf, t.len, 0, 0 };
    const uint32_t q = deref(t.buf, t.len, field_at(t, slot, 4));
    if (q && q + 4 <= t.len) { v.pos = q + 4; v.n = rd32(t.buf + q); }
    return v;
}

int fb_str(fb_table t, int slot, char *out, size_t cap) {
    fb_vec v = fb_vec_of(t, slot);
    if (cap) out[0] = '\0';
    if (!v.pos || v.pos + v.n > v.len) return -1;
    const size_t k = v.n < cap - 1 ? v.n : cap - 1;
    memcpy(out, v.buf + v.pos, k);
    out[k] = '\0';
    return (int)v.n;
}

uint8_t  fb_vec_u8_at(fb_vec v, uint32_t i)  { return i < v.n && v.pos + i + 1 <= v.len ? v.buf[v.pos + i] : 0; }
uint16_t fb_vec_u16_at(fb_vec v, uint32_t i) { return i < v.n && v.pos + 2 * i + 2 <= v.len ? rd16(v.buf + v.pos + 2 * i) : 0; }
uint64_t fb_vec_u64_at(fb_vec v, uint32_t i) {
    if (i >= v.n || v.pos + 8 * i + 8 > v.len) return 0;
    return (uint64_t)rd32(v.buf + v.pos + 8 * i) | (uint64_t)rd32(v.buf + v.pos + 8 * i + 4) << 32;
}
fb_table fb_vec_table_at(fb_vec v, uint32_t i) {
    fb_table t = { v.buf, v.len, 0 };
    if (i < v.n) t.pos = deref(v.buf, v.len, v.pos + 4 * i);
    if (t.pos + 4 > v.len) t.pos = 0;
    return t;
}
