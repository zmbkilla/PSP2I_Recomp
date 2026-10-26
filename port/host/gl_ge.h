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
/* The displayed frame drawn into the window as gl_blit_frame does, but
 * straight from its render target (no readback); 0 when the CPU's copy is the
 * current one (then gl_ge_present + gl_blit_frame). */
int  gl_ge_present_direct(uint32_t addr, uint32_t stride, int fmt, int w, int h,
                          int x, int y, int vw, int vh, int cw, int ch);
/* A small 0xAARRGGBB image over the window, scaled to vw x vh at x, y. */
int  gl_draw_overlay(const uint32_t *px, int w, int h, int x, int y, int vw, int vh, int cw, int ch);
int  gl_blit_frame(const uint32_t *px, int w, int h, int x, int y, int vw, int vh, int cw, int ch);
void gl_ge_report(FILE *out);
/* Texture replacement: fn(pixel address) -> RGBA (w x h) or NULL; gen() changes when the images do. */
void gl_ge_set_texture_source(const unsigned char *(*fn)(unsigned addr, int *w, int *h), unsigned (*gen)(void));
void gl_ge_state_counts(uint64_t *made, uint64_t *skipped);
void gl_ge_state_reset(void);           /* GL state was set outside gl_ge.c (its cache is stale) */
void gl_ge_vram_waits(uint64_t *n, uint64_t *us, char *top, size_t cap);

/* The platform layer (gl_wgl.c on Windows). */
int  gl_platform_init(void);            /* context on a hidden window; 0 on success */
int  gl_platform_attach(void *window);  /* render to this window from now on (HWND) */
void gl_platform_swap(void);
void gl_platform_release(void);         /* the context leaves this thread */
int  gl_platform_bind(void);             /* ... and is made current on this one */

#endif
