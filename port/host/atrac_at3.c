/* ATRAC3plus decoding for the runtime (psp_atrac_set_codec) through
 * psp2i_atrac.dll -- FFmpeg's ATRAC decoder as PPSSPP extracted it to stand
 * alone, built by port/atrac_dll (LGPL-2.1+). It replaces the FFmpeg DLLs.
 *
 * The DLL is loaded at run time and only its small C interface is used
 * (port/atrac_dll/at3_export.cpp), so this file and the rest of the game stay
 * MIT; anyone may swap in their own build of the decoder. Without the DLL,
 * ATRAC music is silent and everything else runs.
 *
 * ATRAC3 (not plus) needs the stream's codec extra data, which the runtime's
 * codec interface does not carry; it is reported unsupported (silent), as
 * before. PSP2i uses ATRAC3plus. */

#include "atrac_at3.h"

#include <psprecomp/hle.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static struct {
    int   (*api)(void);
    void *(*open)(int channels, int *block_align);
    void  (*close)(void *ctx);
    void  (*flush)(void *ctx);
    int   (*decode)(void *ctx, float *l, float *r, int *samples, const uint8_t *buf, int size);
} A;

typedef struct {
    void *ctx;
    int channels, block_align, fails;
    float l[2048], r[2048];
} dec;

static int16_t to16(float v) {
    int x = (int)(v * 32767.0f);
    return (int16_t)(x < -32768 ? -32768 : x > 32767 ? 32767 : x);
}

static void *dec_open(int at3plus, int channels, int block_align, int sample_rate) {
    (void)sample_rate;
    if (!at3plus) {
        fprintf(stderr, "audio: ATRAC3 (not plus) needs codec extradata, not supported; that stream is silent\n");
        return NULL;
    }
    dec *d = (dec *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->channels = channels == 1 ? 1 : 2;
    d->block_align = block_align;
    d->ctx = A.open(d->channels, &d->block_align);
    if (!d->ctx) { fprintf(stderr, "audio: ATRAC3plus decoder refused %d channel(s), %d-byte frames\n", channels, block_align); free(d); return NULL; }
    return d;
}

static int dec_decode(void *p, const uint8_t *frame, int size, int16_t *out, int max) {
    dec *d = (dec *)p;
    int n = 0;
    const int rc = A.decode(d->ctx, d->l, d->r, &n, frame, size);
    if (rc < 0) return ++d->fails > 64 ? -1 : 0;        /* a bad frame is silence; one that never recovers, given up */
    d->fails = 0;
    if (n > 2048) n = 2048;
    if (n > max) n = max;
    const float *r = d->channels == 2 ? d->r : d->l;    /* mono: duplicated */
    for (int i = 0; i < n; i++) { out[i * 2] = to16(d->l[i]); out[i * 2 + 1] = to16(r[i]); }
    return n;
}

static void dec_reset(void *p) { dec *d = (dec *)p; if (d && d->ctx) A.flush(d->ctx); }

static void dec_close(void *p) {
    dec *d = (dec *)p;
    if (!d) return;
    if (d->ctx) A.close(d->ctx);
    free(d);
}

static const psp_atrac_codec CODEC = { "psp2i_atrac (FFmpeg ATRAC3plus, LGPL)", dec_open, dec_decode, dec_reset, dec_close };

int atrac_at3_init(const char *exedir) {
    char path[900];
    snprintf(path, sizeof path, "%s\\psp2i_atrac.dll", exedir);
    HMODULE m = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) { fprintf(stderr, "audio: psp2i_atrac.dll not found next to the exe; ATRAC music will be silent\n"); return -1; }
    *(FARPROC *)&A.api    = GetProcAddress(m, "psp2i_atrac_api");
    *(FARPROC *)&A.open   = GetProcAddress(m, "psp2i_at3p_open");
    *(FARPROC *)&A.close  = GetProcAddress(m, "psp2i_at3p_close");
    *(FARPROC *)&A.flush  = GetProcAddress(m, "psp2i_at3p_flush");
    *(FARPROC *)&A.decode = GetProcAddress(m, "psp2i_at3p_decode");
    if (!A.api || !A.open || !A.close || !A.flush || !A.decode || A.api() != 1) {
        fprintf(stderr, "audio: psp2i_atrac.dll has another interface; ATRAC music will be silent\n");
        FreeLibrary(m);
        return -1;
    }
    psp_atrac_set_codec(&CODEC);
    fprintf(stderr, "audio: ATRAC music decoded by psp2i_atrac.dll\n");
    return 0;
}

#else  /* another platform: dlopen libpsp2i_atrac.so the same way */
int atrac_at3_init(const char *exedir) { (void)exedir; return -1; }
#endif
