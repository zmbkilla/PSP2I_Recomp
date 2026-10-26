#ifndef PSP2I_GLBREAD_H
#define PSP2I_GLBREAD_H
/* Reading glTF binary files (.glb) for model loading (modelx.c): a small JSON
 * tree, accessors as floats / integers, and PNG images (with inflate) as RGBA.
 * Self-contained: no libraries. */
#include <stddef.h>
#include <stdint.h>

typedef enum { JS_NULL, JS_BOOL, JS_NUM, JS_STR, JS_ARR, JS_OBJ } js_type;
typedef struct js js;
struct js {
    js_type type;
    double num;
    char *str;           /* JS_STR: the string; also the key of an object member */
    char *key;
    js *kids;            /* JS_ARR / JS_OBJ: n children */
    int n;
};

typedef struct {
    js *root;
    uint8_t *bin;        /* the BIN chunk */
    size_t nbin;
    uint8_t *file;
} glb;

int  glb_load(const char *path, glb *g);      /* 0 ok */
void glb_free(glb *g);

js *js_get(const js *o, const char *key);     /* object member, or NULL */
js *js_at(const js *a, int i);                /* array element, or NULL */
int  js_int(const js *v, int dflt);
double js_num(const js *v, double dflt);
const char *js_str(const js *v);

/* Accessor `index` read as `count` x `ncomp` floats (normalized integers
 * scaled to 0..1). Returns malloc'd data, or NULL; *count and *ncomp set. */
float *glb_floats(const glb *g, int index, int *count, int *ncomp);
/* Accessor as unsigned integers (indices, joints). */
uint32_t *glb_uints(const glb *g, int index, int *count, int *ncomp);
/* Image `index` decoded to RGBA (malloc'd), or NULL. PNG only. */
uint8_t *glb_image_rgba(const glb *g, int index, int *w, int *h);
/* Image `index`'s encoded bytes (inside the file), or NULL. */
const uint8_t *glb_image_bytes(const glb *g, int index, size_t *len);

/* PNG bytes -> RGBA (malloc'd), or NULL. */
uint8_t *png_decode(const uint8_t *p, size_t n, int *w, int *h);
#endif
