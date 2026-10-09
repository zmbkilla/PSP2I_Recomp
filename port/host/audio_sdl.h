/* Audio out through SDL3 (loaded at run time), plus PSP2I_AUDIO_DUMP=file.wav
 * recording. See audio_sdl.c. */
#ifndef PSP2I_AUDIO_SDL_H
#define PSP2I_AUDIO_SDL_H

#include <stdbool.h>
#include <stdint.h>

/* Install the runtime's audio sink; `want_device` opens the SDL playback
 * device (without it only a requested WAV recording happens). */
int  audio_init(int want_device);
void audio_shutdown(void);
/* Master volume, 0..1, for the device output. */
void audio_set_master(float v);
/* Attenuation while other programs play sound: -6, -12, -20 dB, or 0 = off. */
void audio_set_attenuation_db(int db);

#endif /* PSP2I_AUDIO_SDL_H */
