/* FFmpeg H.264 backend for the SDL host. The runtime already demultiplexes
 * PSMF and supplies Annex-B access units. Inspired by kion/psp2i-recomp's
 * h264_libav.c; this adapter uses psprecomp's codec contract directly. */
#include "h264_libav.h"
#include <psprecomp/hle.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct picture {
    AVFrame *frame;
    struct picture *next;
} picture;
typedef struct {
    AVCodecContext *ctx;
    AVPacket *packet;
    AVFrame *shown;
    picture *head, *tail;
    int draining;
    unsigned frames;
} decoder;

static void close_decoder(void *opaque) {
    decoder *d = opaque;
    if (!d) return;
    while (d->head) {
        picture *p = d->head;
        d->head = p->next;
        av_frame_free(&p->frame);
        free(p);
    }
    if (d->frames) fprintf(stderr, "video: decoded %u H.264 frames\n", d->frames);
    av_frame_free(&d->shown);
    av_packet_free(&d->packet);
    avcodec_free_context(&d->ctx);
    free(d);
}
static void *open_decoder(void) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return NULL;
    decoder *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->ctx = avcodec_alloc_context3(codec);
    d->packet = av_packet_alloc();
    if (!d->ctx || !d->packet) { close_decoder(d); return NULL; }
    d->ctx->pkt_timebase = (AVRational){1, 90000};
    d->ctx->thread_count = 1;
    if (avcodec_open2(d->ctx, codec, NULL) < 0) { close_decoder(d); return NULL; }
    return d;
}
/* Receive everything before sending more input: EAGAIN must never discard an
 * access unit. Queue frames because the runtime accepts one picture per call. */
static int receive_pictures(decoder *d) {
    for (;;) {
        AVFrame *f = av_frame_alloc();
        if (!f) return -1;
        int rc = avcodec_receive_frame(d->ctx, f);
        if (rc < 0) {
            av_frame_free(&f);
            return rc == AVERROR(EAGAIN) || rc == AVERROR_EOF ? 0 : -1;
        }
        if (f->format != AV_PIX_FMT_YUV420P || f->width <= 0 || f->height <= 0 ||
            f->width > 1024 || f->height > 1024 || f->linesize[1] != f->linesize[2]) {
            av_frame_free(&f);
            return -1;
        }
        picture *p = calloc(1, sizeof *p);
        if (!p) { av_frame_free(&f); return -1; }
        p->frame = f;
        if (d->tail) d->tail->next = p;
        else d->head = p;
        d->tail = p;
    }
}
static int decode(void *opaque, const uint8_t *au, int size, int64_t pts, psp_video_frame *out) {
    decoder *d = opaque;
    av_frame_free(&d->shown);
    if (au) {
        if (size <= 0 || d->draining) return -1;
        av_packet_unref(d->packet);
        /* av_new_packet provides the zero padding required by libavcodec. */
        if (av_new_packet(d->packet, size) < 0) return -1;
        memcpy(d->packet->data, au, (size_t)size);
        d->packet->pts = pts;
        d->packet->dts = AV_NOPTS_VALUE;
    }
    if (au || !d->draining) {
        int rc = avcodec_send_packet(d->ctx, au ? d->packet : NULL);
        if (rc == AVERROR(EAGAIN)) {
            if (receive_pictures(d) < 0) return -1;
            rc = avcodec_send_packet(d->ctx, au ? d->packet : NULL);
        }
        av_packet_unref(d->packet);
        if (rc < 0 && rc != AVERROR_EOF) return -1;
        if (!au) d->draining = 1;
    }
    if (receive_pictures(d) < 0) return -1;
    if (!d->head) return 0;
    picture *p = d->head;
    d->head = p->next;
    if (!d->head) d->tail = NULL;
    d->shown = p->frame;
    free(p);
    AVFrame *f = d->shown;
    out->width = f->width; out->height = f->height;
    out->y = f->data[0]; out->u = f->data[1]; out->v = f->data[2];
    out->ystride = f->linesize[0]; out->uvstride = f->linesize[1];
    out->pts = f->pts;
    if (++d->frames == 1) fprintf(stderr, "video: first H.264 frame %dx%d\n", f->width, f->height);
    return 1;
}
static const psp_video_codec codec = {"FFmpeg H.264", open_decoder, decode, close_decoder};
int h264_libav_init(void) {
    if (!avcodec_find_decoder(AV_CODEC_ID_H264)) return -1;
    psp_mpeg_set_video_codec(&codec);
    fprintf(stderr, "video: FFmpeg H.264 decoder registered\n");
    return 0;
}
