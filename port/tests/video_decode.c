/* Standalone codec probe: Annex-B H.264 in, packed YUV420P out.
 * Does not require game code. Compare output with ffmpeg's rawvideo output. */
#include "h264_libav.h"
#include <psprecomp/hle.h>
#include <libavcodec/avcodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const psp_video_codec *codec;
void psp_mpeg_set_video_codec(const psp_video_codec *c) { codec = c; }
static int write_picture(FILE *out, const psp_video_frame *f) {
    for (int plane = 0; plane < 3; ++plane) {
        const uint8_t *data = plane == 0 ? f->y : plane == 1 ? f->u : f->v;
        int w = plane ? (f->width + 1) / 2 : f->width;
        int h = plane ? (f->height + 1) / 2 : f->height;
        int stride = plane ? f->uvstride : f->ystride;
        for (int y = 0; y < h; ++y)
            if (fwrite(data + y * stride, 1, w, out) != (size_t)w) return -1;
    }
    return 0;
}
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    if (!in || !out || h264_libav_init() < 0) return 2;
    AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(NULL);
    void *decoder = codec->open();
    if (!parser || !ctx || !decoder) return 2;
    uint8_t buf[2048 + AV_INPUT_BUFFER_PADDING_SIZE];
    int frames = 0, units = 0;
    for (;;) {
        int n = (int)fread(buf, 1, 2048, in);
        if (ferror(in)) return 3;
        memset(buf + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        uint8_t *p = buf;
        do {
            uint8_t *packet; int size;
            int used = av_parser_parse2(parser, ctx, &packet, &size, p, n,
                                       AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (used < 0) return 3;
            p += used; n -= used;
            if (size) {
                psp_video_frame frame;
                int rc = codec->decode(decoder, packet, size, (int64_t)units++ * 3003, &frame);
                if (rc < 0) return 4;
                if (rc == 1) { if (write_picture(out, &frame) < 0) return 5; ++frames; }
            }
        } while (n > 0);
        if (feof(in)) break;
    }
    {
        uint8_t *packet; int size;
        if (av_parser_parse2(parser, ctx, &packet, &size, NULL, 0,
                            AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0) < 0) return 3;
        if (size) {
            psp_video_frame frame;
            int rc = codec->decode(decoder, packet, size, (int64_t)units++ * 3003, &frame);
            if (rc < 0) return 4;
            if (rc == 1) { if (write_picture(out, &frame) < 0) return 5; ++frames; }
        }
    }
    for (;;) {
        psp_video_frame frame;
        int rc = codec->decode(decoder, NULL, 0, 0, &frame);
        if (rc < 0) return 4;
        if (!rc) break;
        if (write_picture(out, &frame) < 0) return 5;
        ++frames;
    }
    psp_video_frame frame;
    if (codec->decode(decoder, NULL, 0, 0, &frame) != 0) return 6;
    codec->close(decoder);
    av_parser_close(parser); avcodec_free_context(&ctx);
    fclose(in);
    if (fclose(out)) return 5;
    fprintf(stderr, "decoded %d frames from %d access units; clean EOF\n", frames, units);
    return frames ? 0 : 7;
}
