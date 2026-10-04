/* ATRAC3plus decoding with FFmpeg's libavcodec, loaded at run time, handed
 * to the runtime through psp_atrac_set_codec. See atrac_ffmpeg.c. */
#ifndef PSP2I_ATRAC_FFMPEG_H
#define PSP2I_ATRAC_FFMPEG_H

/* Find and load avcodec/avutil; 0 on success. On failure ATRAC is silent. */
int atrac_ffmpeg_init(const char *exedir);

#endif /* PSP2I_ATRAC_FFMPEG_H */
