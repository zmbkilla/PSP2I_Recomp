/* The C interface of psp2i_atrac.dll (see CMakeLists.txt): the decoder's own
 * API (at3_decoders.h), exported with C names, plus a version number so the
 * game can refuse a DLL with a different interface. Planar float output, as
 * the decoder produces it; the game converts to its sample format. */
#include "at3_decoders.h"

#if defined(_WIN32)
#define AT3_EXPORT extern "C" __declspec(dllexport)
#else
#define AT3_EXPORT extern "C" __attribute__((visibility("default")))
#endif

AT3_EXPORT int psp2i_atrac_api(void) { return 1; }

/* ATRAC3plus. block_align: bytes per frame (0 = detect; updated). */
AT3_EXPORT void *psp2i_at3p_open(int channels, int *block_align) { return atrac3p_alloc(channels, block_align); }
AT3_EXPORT void psp2i_at3p_close(void *ctx) { atrac3p_free((ATRAC3PContext *)ctx); }
AT3_EXPORT void psp2i_at3p_flush(void *ctx) { atrac3p_flush_buffers((ATRAC3PContext *)ctx); }
/* One frame; left/right: room for 2048 samples each. Returns the bytes used
 * (< 0 on error); *samples = samples per channel written. */
AT3_EXPORT int psp2i_at3p_decode(void *ctx, float *left, float *right, int *samples, const uint8_t *buf, int size) {
    float *out[2] = { left, right };
    return atrac3p_decode_frame((ATRAC3PContext *)ctx, out, samples, buf, size);
}

/* ATRAC3: needs the stream's codec extra data (from its RIFF header). */
AT3_EXPORT void *psp2i_at3_open(int channels, int *block_align, const uint8_t *extra, int extra_size) {
    return atrac3_alloc(channels, block_align, extra, extra_size);
}
AT3_EXPORT void psp2i_at3_close(void *ctx) { atrac3_free((ATRAC3Context *)ctx); }
AT3_EXPORT void psp2i_at3_flush(void *ctx) { atrac3_flush_buffers((ATRAC3Context *)ctx); }
AT3_EXPORT int psp2i_at3_decode(void *ctx, float *left, float *right, int *samples, const uint8_t *buf, int size) {
    float *out[2] = { left, right };
    return atrac3_decode_frame((ATRAC3Context *)ctx, out, samples, buf, size);
}
