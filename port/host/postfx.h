/* Post-processing of the final picture (the engine menu's GRAPHICS page):
 * scaling filter, FXAA, sharpening, scanlines, brightness and saturation.
 * The settings live here; present.c (Direct3D 11) and gl_ge.c (OpenGL) apply
 * them in the shader that scales the 480x272 frame into the window. All off
 * (nearest scaling, 100%) is the plain picture. */
#ifndef PSP2I_POSTFX_H
#define PSP2I_POSTFX_H

enum { PF_NEAREST, PF_BILINEAR, PF_SHARP_BILINEAR, PF_FILTERS };   /* scaling */
enum { PF_OFF, PF_LOW, PF_HIGH, PF_LEVELS };                       /* sharpen, scanlines */

typedef struct {
    int scaling;      /* PF_NEAREST / PF_BILINEAR / PF_SHARP_BILINEAR */
    int fxaa;         /* 0 / 1 */
    int sharpen;      /* PF_OFF / PF_LOW / PF_HIGH */
    int scanlines;    /* PF_OFF / PF_LOW / PF_HIGH */
    int brightness;   /* tenths: 7..13 (70..130%) */
    int saturation;   /* tenths: 0..20 (0..200%) */
} postfx;

#define POSTFX_BRIGHT_MIN 7
#define POSTFX_BRIGHT_MAX 13
#define POSTFX_SAT_MAX    20

void postfx_defaults(postfx *p);
void postfx_set(const postfx *p);
const postfx *postfx_get(void);

/* The shader's parameters for a frame of fw x fh shown vw pixels wide:
 * p0 = (scaling, fxaa, sharpen strength, scanline strength),
 * p1 = (brightness, saturation, window pixels per frame pixel, 0).
 * linear: whether to sample with bilinear filtering. */
void postfx_params(int fw, int vw, float p0[4], float p1[4], int *linear);

/* Names for the menu and psp2i_display.ini. */
const char *postfx_scaling_name(int s);      /* "NEAREST", "BILINEAR", "SHARP BILINEAR" */
const char *postfx_level_name(int l);        /* "OFF", "LOW", "HIGH" */
int postfx_scaling_parse(const char *s);     /* -1 if unknown */
int postfx_level_parse(const char *s);       /* -1 if unknown */

#endif /* PSP2I_POSTFX_H */
