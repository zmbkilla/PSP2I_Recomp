/* OpenGL GE backend and window blit (gl_ge.c): portable, needs a current
 * OpenGL 3.3 core context with the entry points loaded (gl_load). */
#ifndef PSP2I_GL_GE_H
#define PSP2I_GL_GE_H

#include <stdint.h>
#include <stdio.h>

/* Install the backend (psp_gpu_set_backend). 0 on success. */
int  gl_ge_init(void);
/* As d3d11_present: 1 = `out` holds the displayed frame from an asynchronous
 * copy, 2 = keep the last picture, 0 = read VRAM (it is current). */
int  gl_ge_present(uint32_t addr, uint32_t stride, int fmt, uint32_t *out, int w, int h);
/* Draw the composed frame (0xAARRGGBB, w x h) into the default framebuffer:
 * cleared to black (cw x ch), the frame in the rect x, y, vw, vh (top-left
 * origin). The platform swaps buffers afterwards. 0 on success. */
int  gl_blit_frame(const uint32_t *px, int w, int h, int x, int y, int vw, int vh, int cw, int ch);
void gl_ge_report(FILE *out);

/* The platform layer (gl_wgl.c on Windows). */
int  gl_platform_init(void);            /* context on a hidden window; 0 on success */
int  gl_platform_attach(void *window);  /* render to this window from now on (HWND) */
void gl_platform_swap(void);

#endif
