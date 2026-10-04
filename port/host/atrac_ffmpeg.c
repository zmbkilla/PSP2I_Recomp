/* ATRAC3plus decoding for the runtime (psp_atrac_set_codec) with FFmpeg's
 * libavcodec, loaded at run time.
 *
 * Only the shared libraries ship with the port (avcodec-*.dll, avutil-*.dll
 * and their dependencies, no headers or import libraries), so this file
 * declares the few functions it uses and resolves them with GetProcAddress;
 * a port to another platform does the same with dlopen/dlsym (or supplies a
 * different psp_atrac_codec altogether -- the runtime does not care).
 *
 * Struct access is kept to a minimum, because FFmpeg's struct layouts belong
 * to its headers, not its ABI promise:
 *   - the codec is configured by option NAME through an AVDictionary passed
 *     to avcodec_open2 ("ar", "ch_layout", "block_align"); options the
 *     library does not recognise are left in the dictionary, and any left
 *     over fails the open instead of decoding with a wrong configuration;
 *   - packets are built with av_packet_from_data (no AVPacket fields);
 *   - from AVFrame only data[0..1], nb_samples and format are read -- fields
 *     that have sat at the start of the struct, in this order, for over a
 *     decade -- and the first frame is checked (a known sample format and a
 *     plausible sample count) before any of it is trusted. If the check
 *     fails, decoding is switched off: music is silent rather than noise.
 *
 * ATRAC3 (not plus) needs codec extradata, which cannot be set by option;
 * it is reported unsupported (silent). PSP2i uses ATRAC3plus. */

#include "atrac_ffmpeg.h"

#include <psprecomp/hle.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct AVCodec AVCodec;
typedef struct AVCodecContext AVCodecContext;
typedef struct AVPacket AVPacket;
typedef struct AVFrame AVFrame;
typedef struct AVDictionary AVDictionary;

static struct {
    unsigned        (*avcodec_version)(void);
    const AVCodec  *(*avcodec_find_decoder_by_name)(const char *);
    AVCodecContext *(*avcodec_alloc_context3)(const AVCodec *);
    int             (*avcodec_open2)(AVCodecContext *, const AVCodec *, AVDictionary **);
    int             (*avcodec_send_packet)(AVCodecContext *, const AVPacket *);
    int             (*avcodec_receive_frame)(AVCodecContext *, AVFrame *);
    void            (*avcodec_flush_buffers)(AVCodecContext *);
    void            (*avcodec_free_context)(AVCodecContext **);
    AVPacket       *(*av_packet_alloc)(void);
    void            (*av_packet_free)(AVPacket **);
    int             (*av_packet_from_data)(AVPacket *, uint8_t *, int);
    void            (*av_packet_unref)(AVPacket *);
    int             (*av_dict_set)(AVDictionary **, const char *, const char *, int);
    int             (*av_dict_count)(const AVDictionary *);
    void            (*av_dict_free)(AVDictionary **);
    AVFrame        *(*av_frame_alloc)(void);
    void            (*av_frame_free)(AVFrame **);
    void            (*av_frame_unref)(AVFrame *);
    void           *(*av_malloc)(size_t);
    void            (*av_free)(void *);
} F;

/* AVFrame: uint8_t *data[8]; int linesize[8]; uint8_t **extended_data;
 * int width, height; int nb_samples; int format; ... */
#define FRAME_DATA(f)       ((uint8_t **)(void *)(f))
#define FRAME_NB_SAMPLES(f) (*(const int *)((const char *)(f) + 8 * sizeof(void *) + 8 * sizeof(int) + sizeof(void *) + 2 * sizeof(int)))
#define FRAME_FORMAT(f)     (*(const int *)((const char *)(f) + 8 * sizeof(void *) + 8 * sizeof(int) + sizeof(void *) + 3 * sizeof(int)))

/* AVSampleFormat values (stable): S16 1, FLT 3, S16P 6, FLTP 8. */
enum { FMT_S16 = 1, FMT_FLT = 3, FMT_S16P = 6, FMT_FLTP = 8 };
#define PACKET_PADDING 64          /* AV_INPUT_BUFFER_PADDING_SIZE */
#define ERR_EAGAIN     (-11)       /* AVERROR(EAGAIN) */

static int  g_loaded;
static int  g_frame_ok = -1;       /* -1 unchecked, 0 layout check failed, 1 ok */

typedef struct {
    const AVCodec  *codec;
    AVCodecContext *ctx;
    AVPacket       *pkt;
    AVFrame        *frame;
    int             fails;
} dec;

static int16_t to16(float v) {
    int x = (int)(v * 32767.0f);
    return (int16_t)(x < -32768 ? -32768 : x > 32767 ? 32767 : x);
}

/* Append the decoded frame to `out`; returns samples written. */
static int take_frame(const AVFrame *fr, int16_t *out, int room) {
    const int fmt = FRAME_FORMAT(fr), n = FRAME_NB_SAMPLES(fr);
    if (g_frame_ok < 0) {
        g_frame_ok = (fmt == FMT_S16 || fmt == FMT_FLT || fmt == FMT_S16P || fmt == FMT_FLTP) && n > 0 && n <= 8192;
        if (!g_frame_ok)
            fprintf(stderr, "audio: libavcodec frame check failed (format %d, %d samples): this FFmpeg's "
                            "AVFrame layout differs; ATRAC music disabled\n", fmt, n);
    }
    if (!g_frame_ok) return -1;
    uint8_t **d = FRAME_DATA(fr);
    const int k = n < room ? n : room;
    /* Mono has no second plane/sample: duplicate the first. */
    const int stereo = (fmt == FMT_FLTP || fmt == FMT_S16P) ? d[1] != NULL : 1;
    for (int i = 0; i < k; i++) {
        int16_t l, r;
        switch (fmt) {
        case FMT_FLTP: l = to16(((const float *)d[0])[i]); r = stereo ? to16(((const float *)d[1])[i]) : l; break;
        case FMT_S16P: l = ((const int16_t *)d[0])[i]; r = stereo ? ((const int16_t *)d[1])[i] : l; break;
        case FMT_FLT:  l = to16(((const float *)d[0])[i * 2]); r = to16(((const float *)d[0])[i * 2 + 1]); break;
        default:       l = ((const int16_t *)d[0])[i * 2]; r = ((const int16_t *)d[0])[i * 2 + 1]; break;
        }
        out[i * 2] = l;
        out[i * 2 + 1] = r;
    }
    return k;
}

static void dec_close(void *p) {
    dec *d = (dec *)p;
    if (!d) return;
    if (d->frame) F.av_frame_free(&d->frame);
    if (d->pkt) F.av_packet_free(&d->pkt);
    if (d->ctx) F.avcodec_free_context(&d->ctx);
    free(d);
}

static void *dec_open(int at3plus, int channels, int block_align, int sample_rate) {
    if (!g_loaded || g_frame_ok == 0) return NULL;
    if (!at3plus) {
        fprintf(stderr, "audio: ATRAC3 (not plus) needs codec extradata, not supported; that stream is silent\n");
        return NULL;
    }
    dec *d = (dec *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->codec = F.avcodec_find_decoder_by_name("atrac3plus");          /* the decoder's name */
    if (!d->codec) d->codec = F.avcodec_find_decoder_by_name("atrac3p");
    if (!d->codec) { fprintf(stderr, "audio: this libavcodec has no ATRAC3plus decoder\n"); free(d); return NULL; }
    d->ctx = F.avcodec_alloc_context3(d->codec);
    d->pkt = F.av_packet_alloc();
    d->frame = F.av_frame_alloc();
    if (!d->ctx || !d->pkt || !d->frame) { dec_close(d); return NULL; }

    char rate[16], align[16];
    snprintf(rate, sizeof rate, "%d", sample_rate);
    snprintf(align, sizeof align, "%d", block_align);
    AVDictionary *opt = NULL;
    F.av_dict_set(&opt, "ar", rate, 0);
    F.av_dict_set(&opt, "ch_layout", channels == 1 ? "mono" : "stereo", 0);
    F.av_dict_set(&opt, "block_align", align, 0);
    int rc = F.avcodec_open2(d->ctx, d->codec, &opt);
    int left = F.av_dict_count(opt);
    F.av_dict_free(&opt);
    if (rc < 0 || left) {
        fprintf(stderr, "audio: libavcodec atrac3p open failed (%d, %d option(s) not recognised)\n", rc, left);
        dec_close(d);
        return NULL;
    }
    return d;
}

static int dec_decode(void *p, const uint8_t *frame, int size, int16_t *out, int max) {
    dec *d = (dec *)p;
    if (g_frame_ok == 0) return -1;
    uint8_t *buf = (uint8_t *)F.av_malloc((size_t)size + PACKET_PADDING);
    if (!buf) return 0;
    memcpy(buf, frame, (size_t)size);
    memset(buf + size, 0, PACKET_PADDING);
    if (F.av_packet_from_data(d->pkt, buf, size) < 0) { F.av_free(buf); return 0; }   /* else owned by the packet */
    int rc = F.avcodec_send_packet(d->ctx, d->pkt);
    F.av_packet_unref(d->pkt);
    if (rc < 0 && rc != ERR_EAGAIN) {
        /* A bad frame is skipped (silence); a codec that never recovers is
         * given up on. */
        return ++d->fails > 64 ? -1 : 0;
    }
    int got = 0;
    while (got < max && F.avcodec_receive_frame(d->ctx, d->frame) == 0) {
        int k = take_frame(d->frame, out + got * 2, max - got);
        F.av_frame_unref(d->frame);
        if (k < 0) return -1;
        got += k;
    }
    d->fails = 0;
    return got;
}

static void dec_reset(void *p) { dec *d = (dec *)p; if (d && d->ctx) F.avcodec_flush_buffers(d->ctx); }

static const psp_atrac_codec CODEC = { "libavcodec (atrac3p)", dec_open, dec_decode, dec_reset, dec_close };

/* "<dir>\\<prefix>*.dll", the first match, loaded with its own directory on
 * the search path (so avcodec finds avutil and swresample next to it). */
static HMODULE load_from(const char *dir, const char *prefix) {
    char pat[900], path[900];
    snprintf(pat, sizeof pat, "%s\\%s*.dll", dir, prefix);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
    FindClose(h);
    return LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}

/* Directories that may hold the FFmpeg DLLs: PSP2I_FFMPEG_DIR, the exe's
 * own, an ffmpeg* folder's bin\ beside the exe or one level up (the project
 * root, for FinalBuild\), and the same in the working directory. */
static int find_dir(const char *exedir, char *out, size_t cap) {
    const char *env = getenv("PSP2I_FFMPEG_DIR");
    char pat[900];
    const char *bases[3] = { exedir, NULL, "." };
    char up[800];
    snprintf(up, sizeof up, "%s\\..", exedir);
    bases[1] = up;
    if (env && env[0]) {
        snprintf(pat, sizeof pat, "%s\\avcodec-*.dll", env);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) { FindClose(h); snprintf(out, cap, "%s", env); return 1; }
    }
    for (int b = 0; b < 3; b++) {
        WIN32_FIND_DATAA fd;
        snprintf(pat, sizeof pat, "%s\\avcodec-*.dll", bases[b]);
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) { FindClose(h); snprintf(out, cap, "%s", bases[b]); return 1; }
        snprintf(pat, sizeof pat, "%s\\ffmpeg*", bases[b]);
        h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            char bin[900], p2[900];
            WIN32_FIND_DATAA f2;
            snprintf(bin, sizeof bin, "%s\\%s\\bin", bases[b], fd.cFileName);
            snprintf(p2, sizeof p2, "%s\\avcodec-*.dll", bin);
            HANDLE h2 = FindFirstFileA(p2, &f2);
            if (h2 != INVALID_HANDLE_VALUE) { FindClose(h2); FindClose(h); snprintf(out, cap, "%s", bin); return 1; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return 0;
}

int atrac_ffmpeg_init(const char *exedir) {
    char dir[900];
    if (!find_dir(exedir, dir, sizeof dir)) {
        fprintf(stderr, "audio: FFmpeg libraries (avcodec-*.dll) not found; ATRAC music will be silent\n");
        return -1;
    }
    HMODULE util = load_from(dir, "avutil-"), codec = load_from(dir, "avcodec-");
    if (!util || !codec) {
        fprintf(stderr, "audio: could not load FFmpeg from %s; ATRAC music will be silent\n", dir);
        return -1;
    }
#define GETC(name) do { *(FARPROC *)&F.name = GetProcAddress(codec, #name); if (!F.name) goto missing; } while (0)
#define GETU(name) do { *(FARPROC *)&F.name = GetProcAddress(util, #name);  if (!F.name) goto missing; } while (0)
    GETC(avcodec_version); GETC(avcodec_find_decoder_by_name); GETC(avcodec_alloc_context3);
    GETC(avcodec_open2); GETC(avcodec_send_packet); GETC(avcodec_receive_frame);
    GETC(avcodec_flush_buffers); GETC(avcodec_free_context);
    GETC(av_packet_alloc); GETC(av_packet_free); GETC(av_packet_from_data); GETC(av_packet_unref);
    GETU(av_dict_set); GETU(av_dict_count); GETU(av_dict_free);
    GETU(av_frame_alloc); GETU(av_frame_free); GETU(av_frame_unref); GETU(av_malloc); GETU(av_free);
#undef GETC
#undef GETU
    {
        const unsigned v = F.avcodec_version();
        g_loaded = 1;
        psp_atrac_set_codec(&CODEC);
        fprintf(stderr, "audio: ATRAC music decoded with libavcodec %u.%u.%u from %s\n",
                v >> 16, (v >> 8) & 0xFF, v & 0xFF, dir);
        return 0;
    }
missing:
    fprintf(stderr, "audio: FFmpeg in %s lacks a needed function; ATRAC music will be silent\n", dir);
    return -1;
}

#else  /* another platform: supply a psp_atrac_codec of its own (dlopen) */
int atrac_ffmpeg_init(const char *exedir) { (void)exedir; return -1; }
#endif
