/* Window output through a D3D11 swap chain: the composed 480x272 frame is
 * uploaded and scaled to the window on the GPU, aspect kept, black bars.
 * Its own device, so it never touches the renderer's pipeline state. */
#ifndef PSP2I_PRESENT_H
#define PSP2I_PRESENT_H

#include <stdint.h>

#ifdef _WIN32
#include <windows.h>

/* 0 on success; nonzero: no swap chain (the caller keeps the GDI path). */
int  present_init(HWND wnd);
/* Show one frame (0xAARRGGBB, w x h). Returns 0, or nonzero if presenting
 * failed (the caller falls back to GDI from then on). */
int  present_frame(const uint32_t *px, int w, int h);
/* The window's client size changed: buffers are resized at the next frame. */
void present_resized(void);
int  present_active(void);
#endif

/* The largest rectangle with the aspect of fw x fh centred in cw x ch. */
void present_fit(int cw, int ch, int fw, int fh, int *x, int *y, int *w, int *h);

#endif
