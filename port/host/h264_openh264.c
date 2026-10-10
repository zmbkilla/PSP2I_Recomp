/* H.264 decoding for sceMpeg movies (psp_mpeg_set_video_codec) through
 * Cisco's OpenH264.
 *
 * Cisco's H.264 patent licence covers its prebuilt OpenH264 binary when the
 * binary is downloaded from Cisco onto the machine that runs it, so the DLL
 * is not shipped with the game. On start, in the background:
 *
 *   - openh264-<version>-win64.dll next to the exe is loaded if its SHA-256
 *     is that of Cisco's release;
 *   - if it is missing, and the download is allowed (psp2i_display.ini
 *     openh264=on, the default), Cisco's .bz2 is fetched from
 *     ciscobinary.openh264.org, decompressed (bzip2), checked against the
 *     same SHA-256 and saved there. Nothing is loaded that fails the check.
 *
 * Until the codec is ready, or without it, movies play black with sound.
 * "OpenH264 Video Codec provided by Cisco Systems, Inc."
 *
 * Only the decoder's C interface is used (the headers are fetched by
 * cmake/FetchOpenH264.cmake and must match the DLL's version). */

#include "h264_openh264.h"

#include <psprecomp/hle.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && defined(PSP2I_OPENH264)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include "bzlib.h"
#include <wels/codec_api.h>

#define DLL_NAME  "openh264-2.6.0-win64.dll"
#define URL_HOST  L"ciscobinary.openh264.org"
#define URL_PATH  L"/openh264-2.6.0-win64.dll.bz2"
#define MAX_DLL   (8u << 20)

static const uint8_t DLL_SHA256[32] = {
    0x20, 0x76, 0xcb, 0x56, 0x75, 0xec, 0x6c, 0x1a, 0x4c, 0x70, 0xe7, 0xa2, 0xa3, 0x22, 0x55, 0x2f,
    0x54, 0x7b, 0x6e, 0xee, 0xd6, 0x49, 0xd6, 0xdf, 0xcd, 0x9e, 0x02, 0xa5, 0x43, 0xb2, 0x46, 0x91,
};

static long (*p_create)(ISVCDecoder **);
static void (*p_destroy)(ISVCDecoder *);

/* ---- the codec ------------------------------------------------------------- */

static void *dec_open(void) {
    ISVCDecoder *d = NULL;
    if (p_create(&d) != 0 || !d) return NULL;
    int quiet = WELS_LOG_QUIET;
    (*d)->SetOption(d, DECODER_OPTION_TRACE_LEVEL, &quiet);
    SDecodingParam prm;
    memset(&prm, 0, sizeof prm);
    prm.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    prm.eEcActiveIdc = ERROR_CON_SLICE_MV_COPY_CROSS_IDR_FREEZE_RES_CHANGE;
    if ((*d)->Initialize(d, &prm) != 0) { p_destroy(d); return NULL; }
    return d;
}

static int dec_decode(void *h, const uint8_t *au, int size, int64_t pts, psp_video_frame *out) {
    ISVCDecoder *d = (ISVCDecoder *)h;
    unsigned char *dst[3] = { NULL, NULL, NULL };
    SBufferInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.uiInBsTimeStamp = (unsigned long long)pts;
    if (au) (*d)->DecodeFrameNoDelay(d, au, size, dst, &bi);
    else    (*d)->FlushFrame(d, dst, &bi);          /* pictures held back for reordering */
    if (bi.iBufferStatus != 1 || !dst[0] || !dst[1] || !dst[2]) return 0;
    out->width = bi.UsrData.sSystemBuffer.iWidth;
    out->height = bi.UsrData.sSystemBuffer.iHeight;
    out->ystride = bi.UsrData.sSystemBuffer.iStride[0];
    out->uvstride = bi.UsrData.sSystemBuffer.iStride[1];
    out->y = dst[0];
    out->u = dst[1];
    out->v = dst[2];
    out->pts = (int64_t)bi.uiOutYuvTimeStamp;
    return 1;
}

static void dec_close(void *h) {
    ISVCDecoder *d = (ISVCDecoder *)h;
    (*d)->Uninitialize(d);
    p_destroy(d);
}

static const psp_video_codec CODEC = { "OpenH264 2.6.0 (Cisco)", dec_open, dec_decode, dec_close };

/* ---- fetching and checking the DLL ------------------------------------------- */

static int sha256_matches(const uint8_t *p, size_t n) {
    BCRYPT_ALG_HANDLE alg;
    uint8_t h[32];
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return 0;
    const long st = BCryptHash(alg, NULL, 0, (PUCHAR)p, (ULONG)n, h, sizeof h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return st == 0 && !memcmp(h, DLL_SHA256, sizeof h);
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = (uint8_t *)malloc(MAX_DLL);
    size_t n = buf ? fread(buf, 1, MAX_DLL, f) : 0;
    fclose(f);
    if (!buf || !n) { free(buf); return NULL; }
    *len = n;
    return buf;
}

/* GET https://ciscobinary.openh264.org/<the .bz2> into memory. */
static uint8_t *download(size_t *len) {
    uint8_t *buf = NULL;
    size_t n = 0;
    HINTERNET s = WinHttpOpen(L"psp2i", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, URL_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", URL_PATH, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    DWORD status = 0, sz = sizeof status;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, NULL) &&
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX) &&
        status == 200 && (buf = (uint8_t *)malloc(MAX_DLL)) != NULL) {
        for (;;) {
            DWORD got = 0;
            if (n == MAX_DLL || !WinHttpReadData(r, buf + n, (DWORD)(MAX_DLL - n), &got)) { n = 0; break; }
            if (!got) break;
            n += got;
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    if (status && status != 200) fprintf(stderr, "video: Cisco's server answered %lu\n", (unsigned long)status);
    if (!n) { free(buf); return NULL; }
    *len = n;
    return buf;
}

/* Download, decompress, check, save. 0 on success. */
static int fetch_dll(const char *path) {
    size_t zlen;
    uint8_t *z = download(&zlen);
    if (!z) { fprintf(stderr, "video: could not download OpenH264 from Cisco; movies are black\n"); return -1; }
    uint8_t *dll = (uint8_t *)malloc(MAX_DLL);
    unsigned int dlen = MAX_DLL;
    const int bz = dll ? BZ2_bzBuffToBuffDecompress((char *)dll, &dlen, (char *)z, (unsigned int)zlen, 0, 0) : BZ_MEM_ERROR;
    free(z);
    if (bz != BZ_OK || !sha256_matches(dll, dlen)) {
        fprintf(stderr, "video: the OpenH264 download failed its check (bzip2 %d); not used\n", bz);
        free(dll);
        return -1;
    }
    char tmp[MAX_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    const int ok = f && fwrite(dll, 1, dlen, f) == dlen;
    if (f) fclose(f);
    free(dll);
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp);
        fprintf(stderr, "video: cannot write %s\n", path);
        return -1;
    }
    fprintf(stderr, "video: downloaded %s from Cisco\n", DLL_NAME);
    return 0;
}

static char g_path[MAX_PATH];
static int  g_allow;
static volatile LONG g_state;               /* see h264_openh264_state */

int h264_openh264_state(void) { return (int)g_state; }

static void loader_body(void);
static DWORD WINAPI loader(LPVOID unused) {
    (void)unused;
    loader_body();
    if (!g_state) InterlockedExchange(&g_state, -1);
    return 0;
}

static void loader_body(void) {
    size_t n;
    uint8_t *have = read_file(g_path, &n);
    if (have) {
        const int ok = sha256_matches(have, n);
        free(have);
        if (!ok) {
            fprintf(stderr, "video: %s is not Cisco's release (SHA-256 differs); not loaded, movies are black\n", DLL_NAME);
            return;
        }
    } else if (!g_allow) {
        fprintf(stderr, "video: OpenH264 download is off (psp2i_display.ini openh264=off); movies are black\n");
        return;
    } else if (fetch_dll(g_path) != 0) {
        return;
    }
    HMODULE m = LoadLibraryExA(g_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (m) {
        p_create = (long (*)(ISVCDecoder **))(void *)GetProcAddress(m, "WelsCreateDecoder");
        p_destroy = (void (*)(ISVCDecoder *))(void *)GetProcAddress(m, "WelsDestroyDecoder");
    }
    if (!p_create || !p_destroy) {
        fprintf(stderr, "video: cannot load %s; movies are black\n", DLL_NAME);
        return;
    }
    psp_mpeg_set_video_codec(&CODEC);
    InterlockedExchange(&g_state, 1);
    fprintf(stderr, "video: OpenH264 Video Codec provided by Cisco Systems, Inc.\n");
    return;
}

int h264_openh264_init(const char *exedir, int allow_download) {
    snprintf(g_path, sizeof g_path, "%s\\%s", exedir, DLL_NAME);
    g_allow = allow_download;
    HANDLE t = CreateThread(NULL, 0, loader, NULL, 0, NULL);
    if (!t) { g_state = -1; return -1; }
    CloseHandle(t);
    return 0;
}

#else

int h264_openh264_init(const char *exedir, int allow_download) {
    (void)exedir; (void)allow_download;
    fprintf(stderr, "video: built without OpenH264; movies are black\n");
    return -1;
}
int h264_openh264_state(void) { return -1; }

#endif
