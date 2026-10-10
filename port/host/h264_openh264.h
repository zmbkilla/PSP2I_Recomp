/* H.264 movie decoding through Cisco's OpenH264 DLL, fetched from Cisco on
 * first use and handed to the runtime through psp_mpeg_set_video_codec. See
 * h264_openh264.c. */
#ifndef PSP2I_H264_OPENH264_H
#define PSP2I_H264_OPENH264_H

/* Load openh264 from the exe's directory in the background, downloading it
 * from Cisco first if it is missing and `allow_download` is set. Returns 0
 * if that started; movies are black (with sound) until the codec is ready. */
int h264_openh264_init(const char *exedir, int allow_download);
/* 1 once the codec is handed to the runtime, -1 once loading has given up,
 * 0 while it is still loading. */
int h264_openh264_state(void);

#endif /* PSP2I_H264_OPENH264_H */
