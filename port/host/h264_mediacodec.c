/* H.264 decoding for sceMpeg movies (psp_mpeg_set_video_codec) through
 * Android's own decoder, MediaCodec (the NDK's libmediandk): part of the
 * system on every device, usually in hardware, so nothing is shipped or
 * downloaded. The Windows build uses OpenH264 (h264_openh264.c), the desktop
 * SDL builds FFmpeg (h264_libav.c); without any, movies play black with sound.
 *
 * The decoder is configured without a surface, so pictures come back in
 * memory, as I420 (COLOR_FormatYUV420Planar, 19) or NV12
 * (COLOR_FormatYUV420SemiPlanar, 21) -- what devices give for that request;
 * both are copied into I420 planes here, which is what the runtime takes.
 * MediaCodec works ahead of its output, so a unit fed in often comes back a
 * few calls later: pictures are queued, one handed out per call, and the
 * runtime's drain calls (au == NULL) send end-of-stream and collect the rest.
 * Full-range pictures are brought to studio range, as h264_libav.c does. */

#include "h264_mediacodec.h"

#include <psprecomp/hle.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __ANDROID__
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#define MAX_W 1024
#define MAX_H 1024
#define QUEUE 8

typedef struct {
    uint8_t *y, *u, *v;           /* I420, w x h, w/2 x h/2 */
    int w, h;
    int64_t pts;
} pic;

typedef struct {
    AMediaCodec *codec;
    int color, stride, slice, crop_l, crop_t, crop_r, crop_b, width, height, full_range;
    pic q[QUEUE];
    int head, count;              /* queued pictures */
    pic shown;                    /* the one handed out last */
    int eos_sent, eos_seen;
    unsigned frames;
} decoder;

static void read_format(decoder *d) {
    AMediaFormat *f = AMediaCodec_getOutputFormat(d->codec);
    if (!f) return;
    int32_t v;
    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &v)) d->color = v;
    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &v)) d->width = v;
    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &v)) d->height = v;
    d->stride = AMediaFormat_getInt32(f, "stride", &v) && v > 0 ? v : d->width;
    d->slice = AMediaFormat_getInt32(f, "slice-height", &v) && v > 0 ? v : d->height;
    d->crop_l = d->crop_t = 0;
    d->crop_r = d->width - 1;
    d->crop_b = d->height - 1;
    if (AMediaFormat_getInt32(f, "crop-left", &v)) d->crop_l = v;
    if (AMediaFormat_getInt32(f, "crop-top", &v)) d->crop_t = v;
    if (AMediaFormat_getInt32(f, "crop-right", &v)) d->crop_r = v;
    if (AMediaFormat_getInt32(f, "crop-bottom", &v)) d->crop_b = v;
    d->full_range = AMediaFormat_getInt32(f, "color-range", &v) && v == 1;   /* COLOR_RANGE_FULL */
    AMediaFormat_delete(f);
    fprintf(stderr, "video: MediaCodec output %dx%d, colour format %d, stride %d, slice %d%s\n",
            d->width, d->height, d->color, d->stride, d->slice, d->full_range ? ", full range" : "");
}

static void free_pic(pic *p) { free(p->y); memset(p, 0, sizeof *p); }

static int alloc_pic(pic *p, int w, int h) {
    if (p->y && p->w == w && p->h == h) return 0;
    free(p->y);
    p->y = (uint8_t *)malloc((size_t)w * h * 3 / 2);
    if (!p->y) return -1;
    p->u = p->y + (size_t)w * h;
    p->v = p->u + (size_t)(w / 2) * (h / 2);
    p->w = w; p->h = h;
    return 0;
}

/* Full range (0..255) to studio range (Y 16..235, C 16..240), in place. */
static void to_studio(pic *p) {
    static uint8_t ly[256], lc[256];
    static int ready;
    if (!ready) {
        for (int i = 0; i < 256; i++) {
            ly[i] = (uint8_t)(16 + (i * 219 + 127) / 255);
            lc[i] = (uint8_t)(128 + ((i - 128) * 224 + (i >= 128 ? 127 : -127)) / 255);
        }
        ready = 1;
    }
    const size_t ny = (size_t)p->w * p->h, nc = (size_t)(p->w / 2) * (p->h / 2);
    for (size_t i = 0; i < ny; i++) p->y[i] = ly[p->y[i]];
    for (size_t i = 0; i < nc; i++) { p->u[i] = lc[p->u[i]]; p->v[i] = lc[p->v[i]]; }
}

/* One output buffer, if any is ready, into the queue. 1 = got one, 0 = none,
 * -1 = failure. */
static int take_output(decoder *d, int64_t timeout_us) {
    AMediaCodecBufferInfo info;
    for (;;) {
        const ssize_t i = AMediaCodec_dequeueOutputBuffer(d->codec, &info, timeout_us);
        if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { read_format(d); continue; }
        if (i == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (i < 0) return 0;                                  /* try again later */
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) d->eos_seen = 1;
        size_t cap = 0;
        const uint8_t *buf = AMediaCodec_getOutputBuffer(d->codec, (size_t)i, &cap);
        int ok = 0;
        const int w = (d->crop_r - d->crop_l + 1) & ~1, h = (d->crop_b - d->crop_t + 1) & ~1;
        if (buf && info.size > 0 && d->count < QUEUE && w > 0 && h > 0 && w <= MAX_W && h <= MAX_H &&
            (d->color == 19 || d->color == 21)) {
            pic *p = &d->q[(d->head + d->count) % QUEUE];
            if (alloc_pic(p, w, h) == 0) {
                const uint8_t *src = buf + info.offset;
                const size_t yplane = (size_t)d->stride * d->slice;
                const size_t need = d->color == 19 ? yplane + 2 * (size_t)(d->stride / 2) * (d->slice / 2)
                                                   : yplane + (size_t)d->stride * (d->slice / 2);
                if ((size_t)info.size >= need - (size_t)d->stride) {   /* some codecs omit the last row's padding */
                    for (int y = 0; y < h; y++)
                        memcpy(p->y + (size_t)y * w, src + (size_t)(y + d->crop_t) * d->stride + d->crop_l, (size_t)w);
                    const int cw = w / 2, ch = h / 2, cl = d->crop_l / 2, ct = d->crop_t / 2;
                    if (d->color == 19) {                     /* I420: U plane, then V */
                        const uint8_t *u = src + yplane, *v = u + (size_t)(d->stride / 2) * (d->slice / 2);
                        for (int y = 0; y < ch; y++) {
                            memcpy(p->u + (size_t)y * cw, u + (size_t)(y + ct) * (d->stride / 2) + cl, (size_t)cw);
                            memcpy(p->v + (size_t)y * cw, v + (size_t)(y + ct) * (d->stride / 2) + cl, (size_t)cw);
                        }
                    } else {                                  /* NV12: UV interleaved */
                        const uint8_t *uv = src + yplane;
                        for (int y = 0; y < ch; y++) {
                            const uint8_t *row = uv + (size_t)(y + ct) * d->stride + (size_t)cl * 2;
                            for (int x = 0; x < cw; x++) {
                                p->u[(size_t)y * cw + x] = row[2 * x];
                                p->v[(size_t)y * cw + x] = row[2 * x + 1];
                            }
                        }
                    }
                    if (d->full_range) to_studio(p);
                    p->pts = info.presentationTimeUs;
                    d->count++;
                    ok = 1;
                }
            }
        } else if (buf && info.size > 0 && d->color != 19 && d->color != 21 && !d->frames) {
            static int warned;
            if (!warned++) fprintf(stderr, "video: MediaCodec gives colour format %d, which is not read here\n", d->color);
        }
        AMediaCodec_releaseOutputBuffer(d->codec, (size_t)i, false);
        return ok;
    }
}

static void dec_close(void *opaque) {
    decoder *d = (decoder *)opaque;
    if (!d) return;
    if (d->frames) fprintf(stderr, "video: decoded %u H.264 frames (MediaCodec)\n", d->frames);
    if (d->codec) { AMediaCodec_stop(d->codec); AMediaCodec_delete(d->codec); }
    for (int i = 0; i < QUEUE; i++) free_pic(&d->q[i]);
    free_pic(&d->shown);
    free(d);
}

static void *dec_open(void) {
    decoder *d = (decoder *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->codec = AMediaCodec_createDecoderByType("video/avc");
    if (!d->codec) { free(d); return NULL; }
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, 480);     /* the PSP's movies; the stream's SPS rules */
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, 272);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 19); /* I420 asked; NV12 is read too */
    const media_status_t st = AMediaCodec_configure(d->codec, f, NULL, NULL, 0);
    AMediaFormat_delete(f);
    if (st != AMEDIA_OK || AMediaCodec_start(d->codec) != AMEDIA_OK) {
        fprintf(stderr, "video: MediaCodec H.264 decoder would not start (%d)\n", (int)st);
        AMediaCodec_delete(d->codec);
        free(d);
        return NULL;
    }
    d->width = 480; d->height = 272; d->stride = 480; d->slice = 272;
    d->crop_r = 479; d->crop_b = 271; d->color = 19;
    return d;
}

static int dec_decode(void *opaque, const uint8_t *au, int size, int64_t pts, psp_video_frame *out) {
    decoder *d = (decoder *)opaque;
    if (au) {
        if (size <= 0) return -1;
        /* An input buffer: if none is free the decoder is full, so take its
         * pictures out first (the runtime gets them one per call). */
        ssize_t in = -1;
        for (int tries = 0; tries < 20 && in < 0; tries++) {
            in = AMediaCodec_dequeueInputBuffer(d->codec, tries ? 2000 : 0);
            if (in < 0 && take_output(d, 0) < 0) return -1;
        }
        if (in < 0) { fprintf(stderr, "video: MediaCodec took no input; an access unit is dropped\n"); }
        else {
            size_t cap = 0;
            uint8_t *buf = AMediaCodec_getInputBuffer(d->codec, (size_t)in, &cap);
            if (!buf || (size_t)size > cap) {
                AMediaCodec_queueInputBuffer(d->codec, (size_t)in, 0, 0, (uint64_t)pts, 0);
                return -1;
            }
            memcpy(buf, au, (size_t)size);
            if (AMediaCodec_queueInputBuffer(d->codec, (size_t)in, 0, (size_t)size, (uint64_t)pts, 0) != AMEDIA_OK) return -1;
        }
        while (take_output(d, d->count ? 0 : 1000) > 0) {}
    } else {
        if (!d->eos_sent) {                                    /* drain: end of stream, then the held pictures */
            const ssize_t in = AMediaCodec_dequeueInputBuffer(d->codec, 10000);
            if (in >= 0) {
                AMediaCodec_queueInputBuffer(d->codec, (size_t)in, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                d->eos_sent = 1;
            }
        }
        if (!d->count && !d->eos_seen) take_output(d, 20000);
    }
    if (!d->count) return 0;
    /* hand out the oldest: swap it with the shown buffer (no copy) */
    pic tmp = d->shown;
    d->shown = d->q[d->head];
    d->q[d->head] = tmp;
    d->head = (d->head + 1) % QUEUE;
    d->count--;
    out->width = d->shown.w; out->height = d->shown.h;
    out->y = d->shown.y; out->u = d->shown.u; out->v = d->shown.v;
    out->ystride = d->shown.w; out->uvstride = d->shown.w / 2;
    out->pts = d->shown.pts;
    if (++d->frames == 1) fprintf(stderr, "video: first H.264 frame %dx%d (MediaCodec)\n", d->shown.w, d->shown.h);
    return 1;
}

static const psp_video_codec CODEC = { "MediaCodec H.264", dec_open, dec_decode, dec_close };

int h264_mediacodec_init(void) {
    AMediaCodec *probe = AMediaCodec_createDecoderByType("video/avc");
    if (!probe) { fprintf(stderr, "video: no H.264 decoder on this device; movies will be black\n"); return -1; }
    AMediaCodec_delete(probe);
    psp_mpeg_set_video_codec(&CODEC);
    fprintf(stderr, "video: Android MediaCodec H.264 decoder registered\n");
    return 0;
}

#else
int h264_mediacodec_init(void) { return -1; }
#endif
