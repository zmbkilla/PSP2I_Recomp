/* A minimal FlatBuffers builder and reader -- just what RPCN's matching
 * messages need (tables of scalars, strings, vectors of scalars and of
 * tables). See fbuf.c. Fields are addressed by slot: their order in the
 * schema (RPCN's np2_structs.fbs), from 0. */
#ifndef PSP2I_FBUF_H
#define PSP2I_FBUF_H

#include <stddef.h>
#include <stdint.h>

/* ---- builder (back to front, like the reference implementation) ---- */

typedef uint32_t fb_ref;                 /* an object's distance from the end; 0 = none */

typedef struct {
    uint8_t *buf;
    uint32_t cap, size, minalign;
    int failed;
    /* the table being built */
    uint32_t tstart;
    int nfields;
    struct { int slot; uint32_t at; } field[24];
} fb_builder;

void     fb_init(fb_builder *b);
void     fb_free(fb_builder *b);
fb_ref   fb_bytes(fb_builder *b, const void *data, uint32_t n);             /* [ubyte] */
fb_ref   fb_vec_u16(fb_builder *b, const uint16_t *v, uint32_t n);
fb_ref   fb_vec_u64(fb_builder *b, const uint64_t *v, uint32_t n);
fb_ref   fb_string(fb_builder *b, const char *s);
fb_ref   fb_vec_refs(fb_builder *b, const fb_ref *v, uint32_t n);           /* [table] / [string] */
void     fb_table_start(fb_builder *b);
void     fb_add_u8(fb_builder *b, int slot, uint8_t v);
void     fb_add_u16(fb_builder *b, int slot, uint16_t v);
void     fb_add_u32(fb_builder *b, int slot, uint32_t v);
void     fb_add_u64(fb_builder *b, int slot, uint64_t v);
void     fb_add_ref(fb_builder *b, int slot, fb_ref r);                    /* skipped when 0 */
fb_ref   fb_table_end(fb_builder *b);
/* The finished buffer (root table r); NULL if anything failed. */
const uint8_t *fb_finish(fb_builder *b, fb_ref root, uint32_t *len);

/* ---- reader (bounds-checked; absent or out-of-range gives the default) ---- */

typedef struct { const uint8_t *buf; uint32_t len; uint32_t pos; } fb_table;   /* pos 0 = none */
typedef struct { const uint8_t *buf; uint32_t len; uint32_t pos; uint32_t n; } fb_vec;

fb_table fb_root(const uint8_t *buf, uint32_t len);
int      fb_has(fb_table t, int slot);
uint8_t  fb_u8(fb_table t, int slot);
uint16_t fb_u16(fb_table t, int slot);
uint32_t fb_u32(fb_table t, int slot);
uint64_t fb_u64(fb_table t, int slot);
fb_table fb_sub(fb_table t, int slot);                                    /* table field */
fb_vec   fb_vec_of(fb_table t, int slot);                                 /* vector field */
/* string field into out (NUL-terminated, truncated to cap); returns its length or -1 if absent */
int      fb_str(fb_table t, int slot, char *out, size_t cap);
uint8_t  fb_vec_u8_at(fb_vec v, uint32_t i);
uint16_t fb_vec_u16_at(fb_vec v, uint32_t i);
uint64_t fb_vec_u64_at(fb_vec v, uint32_t i);
fb_table fb_vec_table_at(fb_vec v, uint32_t i);

#endif
