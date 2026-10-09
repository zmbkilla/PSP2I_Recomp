/* Audio out through SDL3, and optional recording to a WAV file.
 *
 * The runtime hands every buffer a game outputs through sceAudio to a sink
 * (psp_audio_set_sink) as stereo s16 at 44.1 kHz with the channel volume
 * applied. Here each PSP channel gets its own SDL audio stream on the default
 * playback device (SDL mixes the streams). The emulator already paces the
 * game's audio thread to real time, so the queues stay short; a queue that
 * grows past AUDIO_MAX_QUEUE (a stall, a debugger) is cleared rather than
 * letting latency build up.
 *
 * SDL3.dll is loaded at run time, as for input (input_sdl.c), and only these
 * SDL3 functions are used: SDL_InitSubSystem, SDL_QuitSubSystem,
 * SDL_OpenAudioDeviceStream, SDL_ResumeAudioStreamDevice,
 * SDL_PutAudioStreamData, SDL_GetAudioStreamQueued, SDL_ClearAudioStream,
 * SDL_DestroyAudioStream, SDL_GetError. The one structure passed is
 * SDL_AudioSpec { SDL_AudioFormat format; int channels; int freq; }.
 *
 * PSP2I_AUDIO_DUMP=file.wav also records the mixed output (headless too),
 * which is how decoding can be checked without listening.
 *
 * Volume: what reaches the device is scaled by the master volume (menu MASTER
 * VOLUME) times the attenuation (menu ATTENUATION): while another program is
 * playing sound (duck_win.c) the game drops by that many dB, and comes back
 * HOLD_MS after it stops. The gain moves smoothly, sample by sample (down in
 * about 0.15 s, up in about 0.5 s), so changes never click. The WAV recording
 * keeps the game's own level. Music and sound-effect volumes are applied where
 * those are produced (atrac_at3.c, the runtime's psp_sas_set_gain). */

#include "audio_sdl.h"
#include "duck.h"

#include <psprecomp/hle.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_RATE      44100
#define AUDIO_CHANNELS  8
#define AUDIO_MAX_QUEUE (AUDIO_RATE / 5 * 4)       /* 200 ms of stereo s16, in bytes */

static FILE    *g_wav;
static uint32_t g_wav_bytes;

/* ---- volume and attenuation ---------------------------------------------------- */

#define HOLD_MS 500
static float g_master = 1.0f;          /* menu MASTER VOLUME */
static float g_duck = 1.0f;            /* gain while other audio plays (1 = attenuation off) */
static float g_gain = 1.0f;            /* the gain applied now, moving toward the target */

void audio_set_master(float v) { g_master = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

void audio_set_attenuation_db(int db) {
    g_duck = db >= 0 ? 1.0f : powf(10.0f, (float)db / 20.0f);
    if (db < 0) duck_start(); else duck_stop();
}

/* Scale `frames` stereo frames into `out`, moving the gain toward the target
 * by at most the attack/release step per sample. */
static void apply_gain(const int16_t *in, int16_t *out, uint32_t frames) {
    const float target = g_master * (g_duck < 1.0f && duck_others_heard_within(HOLD_MS) ? g_duck : 1.0f);
    const float down = 1.0f / (0.15f * AUDIO_RATE), up = 1.0f / (0.5f * AUDIO_RATE);
    float g = g_gain;
    for (uint32_t i = 0; i < frames; i++) {
        if (g > target) { g -= down; if (g < target) g = target; }
        else if (g < target) { g += up; if (g > target) g = target; }
        for (int c = 0; c < 2; c++) {
            const float v = (float)in[i * 2 + c] * g;
            out[i * 2 + c] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
        }
    }
    g_gain = g;
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct SDL_AudioStream SDL_AudioStream;
typedef struct { int format; int channels; int freq; } SDL_AudioSpec;
#define SDL_INIT_AUDIO                    0x00000010u
#define SDL_AUDIO_S16LE                   0x8010
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK 0xFFFFFFFFu

static struct {
    bool             (*InitSubSystem)(uint32_t);
    void             (*QuitSubSystem)(uint32_t);
    const char      *(*GetError)(void);
    SDL_AudioStream *(*OpenAudioDeviceStream)(uint32_t, const SDL_AudioSpec *, void *, void *);
    bool             (*ResumeAudioStreamDevice)(SDL_AudioStream *);
    bool             (*PutAudioStreamData)(SDL_AudioStream *, const void *, int);
    int              (*GetAudioStreamQueued)(SDL_AudioStream *);
    bool             (*ClearAudioStream)(SDL_AudioStream *);
    void             (*DestroyAudioStream)(SDL_AudioStream *);
} A;

static HMODULE          g_dll;
static int              g_sdl;
static SDL_AudioStream *g_stream[AUDIO_CHANNELS];

static int sdl_load(void) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
    if (n && n < sizeof path) {
        char *slash = strrchr(path, '\\');
        if (slash) { strcpy(slash + 1, "SDL3.dll"); g_dll = LoadLibraryA(path); }
    }
    if (!g_dll) g_dll = LoadLibraryA("SDL3.dll");
    if (!g_dll) return -1;
#define GET(field, name) do { *(FARPROC *)&A.field = GetProcAddress(g_dll, name); \
        if (!A.field) { fprintf(stderr, "audio: SDL3.dll lacks %s\n", name); return -1; } } while (0)
    GET(InitSubSystem, "SDL_InitSubSystem");
    GET(QuitSubSystem, "SDL_QuitSubSystem");
    GET(GetError, "SDL_GetError");
    GET(OpenAudioDeviceStream, "SDL_OpenAudioDeviceStream");
    GET(ResumeAudioStreamDevice, "SDL_ResumeAudioStreamDevice");
    GET(PutAudioStreamData, "SDL_PutAudioStreamData");
    GET(GetAudioStreamQueued, "SDL_GetAudioStreamQueued");
    GET(ClearAudioStream, "SDL_ClearAudioStream");
    GET(DestroyAudioStream, "SDL_DestroyAudioStream");
#undef GET
    return 0;
}

static void sdl_put(int ch, const int16_t *pcm, uint32_t frames) {
    if (!g_sdl || ch < 0 || ch >= AUDIO_CHANNELS) return;
    if (!g_stream[ch]) {
        const SDL_AudioSpec spec = { SDL_AUDIO_S16LE, 2, AUDIO_RATE };
        g_stream[ch] = A.OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (!g_stream[ch]) {
            fprintf(stderr, "audio: cannot open a stream for channel %d: %s\n", ch, A.GetError());
            g_sdl = 0;
            return;
        }
        A.ResumeAudioStreamDevice(g_stream[ch]);
    }
    if (A.GetAudioStreamQueued(g_stream[ch]) > AUDIO_MAX_QUEUE) A.ClearAudioStream(g_stream[ch]);
    if (g_gain == 1.0f && g_master == 1.0f && !(g_duck < 1.0f && duck_others_heard_within(HOLD_MS))) {
        A.PutAudioStreamData(g_stream[ch], pcm, (int)(frames * 4));   /* full level: as the game made it */
        return;
    }
    int16_t tmp[2048 * 2];
    for (uint32_t done = 0; done < frames; ) {
        const uint32_t n = frames - done < 2048 ? frames - done : 2048;
        apply_gain(pcm + done * 2, tmp, n);
        A.PutAudioStreamData(g_stream[ch], tmp, (int)(n * 4));
        done += n;
    }
}
#else
static void sdl_put(int ch, const int16_t *pcm, uint32_t frames) { (void)ch; (void)pcm; (void)frames; }
#endif

/* ---- WAV recording (all channels summed) --------------------------------------- */

static void wav_header(FILE *f, uint32_t data_bytes) {
    uint8_t h[44];
    const uint32_t rate = AUDIO_RATE, byte_rate = AUDIO_RATE * 4;
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + data_bytes; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    uint16_t s = 1; memcpy(h + 20, &s, 2);            /* PCM */
    s = 2; memcpy(h + 22, &s, 2);                     /* stereo */
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    s = 4; memcpy(h + 32, &s, 2);
    s = 16; memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
    fseek(f, 0, SEEK_END);
}

static void sink(int ch, const int16_t *pcm, uint32_t frames) {
    sdl_put(ch, pcm, frames);
    if (g_wav) {
        /* Channels are recorded one after another as they arrive; a game
         * that mixes into one channel (PSP2i does) records exactly. */
        fwrite(pcm, 4, frames, g_wav);
        g_wav_bytes += frames * 4;
    }
}

int audio_init(int want_device) {
    const char *dump = getenv("PSP2I_AUDIO_DUMP");
    if (dump && dump[0]) {
        g_wav = fopen(dump, "wb");
        if (g_wav) { wav_header(g_wav, 0); fprintf(stderr, "audio: recording output to %s\n", dump); }
    }
#ifdef _WIN32
    if (want_device) {
        if (sdl_load() != 0) fprintf(stderr, "audio: SDL3.dll not available; no sound\n");
        else if (!A.InitSubSystem(SDL_INIT_AUDIO)) fprintf(stderr, "audio: SDL audio init failed: %s; no sound\n", A.GetError());
        else { g_sdl = 1; fprintf(stderr, "audio: playing through SDL3 (44.1 kHz stereo)\n"); }
    }
#else
    (void)want_device;
#endif
    if (g_wav || want_device) psp_audio_set_sink(sink);
    return 0;
}

void audio_shutdown(void) {
#ifdef _WIN32
    for (int i = 0; i < AUDIO_CHANNELS; i++)
        if (g_stream[i]) { A.DestroyAudioStream(g_stream[i]); g_stream[i] = NULL; }
    if (g_sdl) { A.QuitSubSystem(SDL_INIT_AUDIO); g_sdl = 0; }
#endif
    if (g_wav) {
        wav_header(g_wav, g_wav_bytes);
        fclose(g_wav);
        g_wav = NULL;
        fprintf(stderr, "audio: recorded %.1f s\n", g_wav_bytes / 4.0 / AUDIO_RATE);
    }
}
