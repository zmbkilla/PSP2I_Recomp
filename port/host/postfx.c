/* Post-processing settings. See postfx.h. */

#include "postfx.h"

#include <ctype.h>
#include <string.h>

static postfx g_pf = { PF_NEAREST, 0, PF_OFF, PF_OFF, 10, 10 };

void postfx_defaults(postfx *p) {
    p->scaling = PF_NEAREST; p->fxaa = 0; p->sharpen = PF_OFF; p->scanlines = PF_OFF;
    p->brightness = 10; p->saturation = 10;
}

void postfx_set(const postfx *p) {
    g_pf = *p;
    if (g_pf.scaling < 0 || g_pf.scaling >= PF_FILTERS) g_pf.scaling = PF_NEAREST;
    if (g_pf.sharpen < 0 || g_pf.sharpen >= PF_LEVELS) g_pf.sharpen = PF_OFF;
    if (g_pf.scanlines < 0 || g_pf.scanlines >= PF_LEVELS) g_pf.scanlines = PF_OFF;
    if (g_pf.brightness < POSTFX_BRIGHT_MIN || g_pf.brightness > POSTFX_BRIGHT_MAX) g_pf.brightness = 10;
    if (g_pf.saturation < 0 || g_pf.saturation > POSTFX_SAT_MAX) g_pf.saturation = 10;
    g_pf.fxaa = g_pf.fxaa != 0;
}

const postfx *postfx_get(void) { return &g_pf; }

void postfx_params(int fw, int vw, float p0[4], float p1[4], int *linear) {
    static const float SHARPEN[PF_LEVELS] = { 0.0f, 0.35f, 0.7f };
    static const float SCAN[PF_LEVELS] = { 0.0f, 0.25f, 0.5f };
    p0[0] = (float)g_pf.scaling;
    p0[1] = (float)g_pf.fxaa;
    p0[2] = SHARPEN[g_pf.sharpen];
    p0[3] = SCAN[g_pf.scanlines];
    p1[0] = (float)g_pf.brightness / 10.0f;
    p1[1] = (float)g_pf.saturation / 10.0f;
    p1[2] = fw > 0 && vw > 0 ? (float)vw / (float)fw : 1.0f;
    p1[3] = 0.0f;
    *linear = g_pf.scaling != PF_NEAREST || g_pf.fxaa;
}

static const char *const SCALING[PF_FILTERS] = { "NEAREST", "BILINEAR", "SHARP BILINEAR" };
static const char *const LEVEL[PF_LEVELS] = { "OFF", "LOW", "HIGH" };

const char *postfx_scaling_name(int s) { return s >= 0 && s < PF_FILTERS ? SCALING[s] : "?"; }
const char *postfx_level_name(int l) { return l >= 0 && l < PF_LEVELS ? LEVEL[l] : "?"; }

/* Case-insensitive match of name against s up to the end, a space-free
 * suffix ("sharp_bilinear" also matches "SHARP BILINEAR"). */
static int name_is(const char *name, const char *s) {
    for (; *name; name++, s++) {
        const char a = *name == ' ' ? '_' : (char)tolower((unsigned char)*name);
        const char b = *s == ' ' ? '_' : (char)tolower((unsigned char)*s);
        if (a != b) return 0;
    }
    return !*s || *s == '\r' || *s == '\n';
}

int postfx_scaling_parse(const char *s) {
    for (int i = 0; i < PF_FILTERS; i++) if (name_is(SCALING[i], s)) return i;
    return -1;
}
int postfx_level_parse(const char *s) {
    for (int i = 0; i < PF_LEVELS; i++) if (name_is(LEVEL[i], s)) return i;
    return -1;
}
