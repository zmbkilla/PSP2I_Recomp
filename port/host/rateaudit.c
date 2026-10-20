/* Frame-rate audit: finds game logic that counts frames or vblanks instead of
 * time -- what runs too fast at 60 / 120 fps (or too slow).
 *
 * PSP2i is frame-rate aware where it uses its timing state (dt = 1/fps, the
 * 30 Hz step 30/fps; framerate.c), but code that adds a constant per frame,
 * or counts vblanks as 1/60 s, speeds up with the frame rate: at 60 fps twice
 * as fast, at 120 fps four times (per frame) or twice (per vblank: the
 * display refreshes at 120 Hz there). Two tools find it:
 *
 * 1. The vblank-use census (always on). The runtime reports every
 *    sceDisplayWaitVblank* and sceDisplayGetVcount with its caller
 *    (psp_display_set_vblank_use_hook). Each new calling site is logged once
 *    in game_log.txt with the game's call stack ("[rate] vblank wait at
 *    ..."), and at 60 / 120 fps a summary every 30 s gives the actual frame
 *    rate and the busiest sites per second. Only the frame function
 *    (0x08B85AB0, framerate.c) is meant to pace by vblanks; another site
 *    waiting in a loop is a fade or delay that runs faster at 120 Hz.
 *
 * 2. The audit (F7). Hold still or keep doing one thing (idle in a lobby with
 *    NPCs and effects moving, or an attack combo on repeat), press F7 and
 *    keep doing it for ~10 s:
 *      - 3 s at the current target (120 if it is 30): game RAM sampled three
 *        times, 1.5 s apart;
 *      - the target switched to 30, 1.5 s to settle, 3 s sampled again;
 *      - the target put back, then 1.5 s of write tracing on the suspects.
 *    Every word that changed steadily (the same direction, at a similar
 *    pace, in both halves of both phases) has a rate per second in each
 *    phase; their ratio says what drives it:
 *      ~1                     time-based: right at any frame rate
 *      ~frames_A / frames_B   advanced per frame: too fast at the higher rate
 *      ~vblanks_A / vblanks_B advanced per vblank (only differs at 120)
 *    logs/rate_audit_N.txt lists the per-frame and per-vblank values (address,
 *    float or int, both rates, the ratio), the game functions that wrote the
 *    first of them during the trace, and the vblank-use sites of both phases.
 *    Nearby addresses are usually one object; the writer stacks say which
 *    code to fix (as fixes.txt did for the NPC arrival radius at 60 fps).
 *
 * 3. The press audit (F6), for motion that only follows a button press (a
 *    menu slide, the character select's carousel): the screen asks for one
 *    press at the current target (120 at 30), then one at 30, then one more.
 *    After each of the first two, RAM is compared every ~33 ms for 2.5 s:
 *    every word that changed, from when to when after the press. A word that
 *    settles in both is part of what the press set moving; how long it moved
 *    at A over how long at B says what times it -- ~1 time, ~frames_B /
 *    frames_A (0.25 at 120 vs 30) frames, ~0.5 vblanks. The third press is
 *    write-traced for the frame-timed ones' writers. logs/press_audit_N.txt.
 *
 * Development builds only (main.c leaves it out of the community build). */

#include "rateaudit.h"
#include "framerate.h"
#include "gamelog.h"
#include "online.h"

#include <psprecomp/hle.h>
#include <psprecomp/mem.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int game_stack(char *out, size_t cap, int max_frames);     /* main.c */

#define RAM_BASE 0x08800000u
#define RAM_SIZE 0x01800000u
#define RAM_WORDS (RAM_SIZE / 4)

static char g_dir[600];

/* logs/<prefix>_N.txt with the first N not taken yet (earlier runs are kept). */
static int next_log(char *path, size_t cap, const char *prefix) {
    for (int n = 1; n < 10000; n++) {
        char name[64];
        snprintf(name, sizeof name, "%s_%d.txt", prefix, n);
        log_path(path, cap, g_dir, name);
        FILE *f = fopen(path, "r");
        if (!f) return n;
        fclose(f);
    }
    return 0;
}

/* ---- the vblank-use census -------------------------------------------------- */

#define MAX_SITES 128
typedef struct { uint32_t ra; int kind; uint64_t n, phase[2]; } site;
static site g_site[MAX_SITES];
static int  g_nsites;
static int  g_phase = -1;                /* the audit phase counting (0 = A, 1 = B), -1 = none */

static void vblank_use(int kind, uint32_t ra) {
    int i = 0;
    while (i < g_nsites && !(g_site[i].ra == ra && g_site[i].kind == kind)) i++;
    if (i == g_nsites) {
        if (g_nsites == MAX_SITES) return;
        g_nsites++;
        memset(&g_site[i], 0, sizeof g_site[i]);
        g_site[i].ra = ra;
        g_site[i].kind = kind;
        char where[400] = "";
        game_stack(where, sizeof where, 12);
        gamelog_note("[rate] %s called from 0x%08X (new; game: %s) at %d fps, %d Hz display",
                     kind ? "sceDisplayGetVcount" : "vblank wait", ra - 8, where[0] ? where : "symbols still loading",
                     framerate_target(), 60 * psp_sched_vblank_rate());
    }
    g_site[i].n++;
    if (g_phase >= 0) g_site[i].phase[g_phase]++;
}

/* ---- the audit ---------------------------------------------------------------- */

typedef struct { uint32_t addr; uint8_t is_float; double rate, v0, v1; } steady;
typedef struct { steady *v; size_t n, cap; double secs, fps; int target, hz; uint64_t flips0; } phase;

enum { A_IDLE, A_SETTLE_A, A_SNAP_A, A_SETTLE_B, A_SNAP_B, A_TRACE };
static int g_state = A_IDLE;
static uint64_t g_t0;                    /* host us: the current step's start */
static int g_snaps;                      /* snapshots taken in this phase */
static uint32_t *g_snap[3];
static uint64_t g_snap_t[3];
static phase g_ph[2];
static int g_restore;                    /* the target to go back to */
static int g_runs;
static char g_status[96];

#define SETTLE_US 1500000u
#define SNAP_GAP_US 1500000u
#define TRACE_US 1500000u

static int plausible_float(uint32_t w) {
    if (w == 0 || w == 0x80000000u) return 1;
    const uint32_t e = (w >> 23) & 0xFF;
    return e >= 127 - 20 && e <= 127 + 24;               /* about 1e-6 .. 1.6e7 */
}
static double as_float(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }

static void push(phase *p, steady s) {
    if (p->n == p->cap) {
        size_t c = p->cap ? p->cap * 2 : 4096;
        steady *v = (steady *)realloc(p->v, c * sizeof *v);
        if (!v) return;
        p->v = v; p->cap = c;
    }
    p->v[p->n++] = s;
}

/* Steadily changing words between the three snapshots: the same direction and
 * a similar pace in both halves. */
static void analyze(phase *p) {
    p->n = 0;
    const double secs = (double)(g_snap_t[2] - g_snap_t[0]) / 1e6;
    p->secs = secs;
    const uint32_t *a = g_snap[0], *b = g_snap[1], *c = g_snap[2];
    for (uint32_t i = 0; i < RAM_WORDS; i++) {
        if (a[i] == c[i] && a[i] == b[i]) continue;
        double x, y, z;
        int fl;
        if (plausible_float(a[i]) && plausible_float(b[i]) && plausible_float(c[i]) && (a[i] | b[i] | c[i]) & 0x7F800000u) {
            x = as_float(a[i]); y = as_float(b[i]); z = as_float(c[i]); fl = 1;
        } else {
            x = (double)(int32_t)a[i]; y = (double)(int32_t)b[i]; z = (double)(int32_t)c[i]; fl = 0;
            if (fabs(z - x) > 1e6 || ((a[i] >> 24) >= 0x08 && (a[i] >> 24) <= 0x0A)) continue;   /* not a counter / a pointer */
        }
        const double d1 = y - x, d2 = z - y;
        if (d1 == 0 || d2 == 0 || (d1 > 0) != (d2 > 0)) continue;
        const double q = d1 / d2;
        if (q < 0.6 || q > 1.67) continue;
        steady s = { RAM_BASE + i * 4, (uint8_t)fl, (z - x) / secs, x, z };
        push(p, s);
    }
}

static int snapshot(int k) {
    const uint8_t *ram = (const uint8_t *)psp_mem_ptr(RAM_BASE, RAM_SIZE);
    if (!ram) return -1;
    if (!g_snap[k] && !(g_snap[k] = (uint32_t *)malloc(RAM_SIZE))) return -1;
    memcpy(g_snap[k], ram, RAM_SIZE);
    g_snap_t[k] = psp_sched_now_us();
    return 0;
}

static const steady *find(const phase *p, uint32_t addr) {
    size_t lo = 0, hi = p->n;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (p->v[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return lo < p->n && p->v[lo].addr == addr ? &p->v[lo] : NULL;
}

/* The suspects whose writers are traced: one per 64-byte block, frame-locked first. */
#define MAX_SUSPECTS 32
static uint32_t g_sus[MAX_SUSPECTS];
static char *g_sus_stack[MAX_SUSPECTS];
static int g_nsus;

/* The press audit's curves: every write to a suspect after the traced
 * presses (C at the higher rate, D at 30): ms after the press, the value. */
#define CURVE_POINTS 96
typedef struct { uint16_t ms; uint32_t v; } curve_pt;
static curve_pt g_curve[2][MAX_SUSPECTS][CURVE_POINTS];
static int g_ncurve[2][MAX_SUSPECTS];
static int g_curve_phase = -1;           /* 0 = C, 1 = D, -1 = not recording */
static uint64_t g_curve_t0;

static void trace_write(uint32_t addr, uint32_t width, uint32_t value) {
    if (!g_nsus || addr + width <= g_sus[0] || addr > g_sus[g_nsus - 1]) return;
    for (int i = 0; i < g_nsus; i++) {
        if (addr + width <= g_sus[i] || addr > g_sus[i] + 3) continue;
        if (g_curve_phase >= 0 && width == 4 && addr == g_sus[i]) {
            int *n = &g_ncurve[g_curve_phase][i];
            const uint32_t ms = (uint32_t)((psp_sched_now_us() - g_curve_t0) / 1000u);
            curve_pt *last = *n ? &g_curve[g_curve_phase][i][*n - 1] : NULL;
            if (!last || last->v != value) {                  /* changes only */
                if (*n < CURVE_POINTS) { g_curve[g_curve_phase][i][*n].ms = (uint16_t)(ms > 65535 ? 65535 : ms); g_curve[g_curve_phase][i][*n].v = value; (*n)++; }
            }
        }
        if (g_sus_stack[i]) return;
        char where[400] = "";
        game_stack(where, sizeof where, 12);
        g_sus_stack[i] = (char *)malloc(strlen(where) + 1);
        if (g_sus_stack[i]) strcpy(g_sus_stack[i], where);
        return;
    }
}

enum { K_FRAME, K_VBLANK, K_TIME, K_OTHER };

static int classify(double r, double fr, double vr) {
    const double tol = 0.2;
    if (fabs(fr - 1.0) > 0.3 && fabs(r / fr - 1.0) < tol) return K_FRAME;
    if (fabs(vr - 1.0) > 0.3 && fabs(r / vr - 1.0) < tol) return K_VBLANK;
    if (fabs(r - 1.0) < tol) return K_TIME;
    return K_OTHER;
}

static int by_addr(const void *x, const void *y) {
    const uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
    return a < b ? -1 : a > b;
}

/* After phase B: the suspects for the write trace. */
static void pick_suspects(void) {
    const phase *A = &g_ph[0], *B = &g_ph[1];
    const double fr = A->fps / (B->fps > 0 ? B->fps : 1), vr = (double)A->hz / B->hz;
    g_nsus = 0;
    for (int want = K_FRAME; want <= K_VBLANK && g_nsus < MAX_SUSPECTS; want++)
        for (size_t i = 0; i < A->n && g_nsus < MAX_SUSPECTS; i++) {
            const steady *b = find(B, A->v[i].addr);
            if (!b || b->rate == 0 || classify(A->v[i].rate / b->rate, fr, vr) != want) continue;
            int dup = 0;
            for (int k = 0; k < g_nsus && !dup; k++) dup = (g_sus[k] >> 6) == (A->v[i].addr >> 6);
            if (!dup) g_sus[g_nsus++] = A->v[i].addr;
        }
    qsort(g_sus, (size_t)g_nsus, sizeof g_sus[0], by_addr);
    for (int i = 0; i < MAX_SUSPECTS; i++) { free(g_sus_stack[i]); g_sus_stack[i] = NULL; }
}

static void report(void) {
    const phase *A = &g_ph[0], *B = &g_ph[1];
    const double fr = A->fps / (B->fps > 0 ? B->fps : 1), vr = (double)A->hz / B->hz;
    char name[40], path[800];
    g_runs = next_log(path, sizeof path, "rate_audit");
    FILE *f = fopen(path, "w");
    if (!f) { gamelog_note("[rate] audit: cannot write %s", path); return; }
    fprintf(f, "# frame-rate audit: phase A at a %d fps target (%.1f fps measured, %d Hz display), "
               "phase B at %d (%.1f fps, %d Hz)\n", A->target, A->fps, A->hz, B->target, B->fps, B->hz);
    fprintf(f, "# a value's rate per second in A / in B: ~1 time-based (right); ~%.2f per frame (too fast at %d);"
               " ~%.2f per vblank\n", fr, A->target, vr);
    fprintf(f, "# steady values: %zu in A, %zu in B\n\n", A->n, B->n);
    int count[4] = {0};
    for (size_t i = 0; i < A->n; i++) {
        const steady *b = find(B, A->v[i].addr);
        if (b && b->rate != 0) count[classify(A->v[i].rate / b->rate, fr, vr)]++;
    }
    fprintf(f, "both phases: %d per frame, %d per vblank, %d time-based, %d other\n", count[K_FRAME], count[K_VBLANK],
            count[K_TIME], count[K_OTHER]);
    static const char *const TITLE[4] = { "PER FRAME (too fast at the higher rate)", "PER VBLANK (too fast on the 120 Hz display)",
                                          "", "OTHER (changes with the frame rate some other way)" };
    for (int want = K_FRAME; want <= K_OTHER; want++) {
        if (want == K_TIME) continue;
        fprintf(f, "\n== %s ==\n%-10s %-5s %14s %14s %14s %7s\n", TITLE[want], "address", "type", "A start", "A rate/s",
                "B rate/s", "ratio");
        int shown = 0;
        for (size_t i = 0; i < A->n && shown < 250; i++) {
            const steady *b = find(B, A->v[i].addr);
            if (!b || b->rate == 0) continue;
            const double r = A->v[i].rate / b->rate;
            if (classify(r, fr, vr) != want) continue;
            fprintf(f, "0x%08X %-5s %14.4f %14.4f %14.4f %7.2f\n", A->v[i].addr, A->v[i].is_float ? "f32" : "s32",
                    A->v[i].v0, A->v[i].rate, b->rate, r);
            shown++;
        }
        if (!shown) fprintf(f, "(none)\n");
    }
    fprintf(f, "\n== WHO WRITES THEM (first write of each during %.1f s at %d fps) ==\n", TRACE_US / 1e6, g_restore);
    for (int i = 0; i < g_nsus; i++)
        fprintf(f, "0x%08X  %s\n", g_sus[i], g_sus_stack[i] ? (g_sus_stack[i][0] ? g_sus_stack[i] : "(no game stack)") : "(not written)");
    if (!g_nsus) fprintf(f, "(no suspects)\n");
    fprintf(f, "\n== VBLANK USE (calls per second) ==\n%-10s %-12s %10s %10s %7s\n", "caller", "what", "A /s", "B /s", "ratio");
    for (int i = 0; i < g_nsites; i++) {
        const double a = (double)g_site[i].phase[0] / A->secs, b = (double)g_site[i].phase[1] / B->secs;
        fprintf(f, "0x%08X %-12s %10.1f %10.1f %7s\n", g_site[i].ra - 8, g_site[i].kind ? "GetVcount" : "WaitVblank", a, b,
                b > 0 ? (snprintf(name, sizeof name, "%.2f", a / b), name) : "-");
    }
    fclose(f);
    gamelog_note("[rate] audit %d written to %s: %d per-frame, %d per-vblank, %d time-based values (A %d fps target, %.1f fps; B %d, %.1f fps)",
                 g_runs, path, count[K_FRAME], count[K_VBLANK], count[K_TIME], A->target, A->fps, B->target, B->fps);
}

static void begin_phase(int k) {
    g_ph[k].target = framerate_target();
    g_ph[k].hz = 60 * psp_sched_vblank_rate();
    g_ph[k].flips0 = psp_display_flips();
    g_snaps = 0;
    g_phase = k;
    for (int i = 0; i < g_nsites; i++) g_site[i].phase[k] = 0;
}

/* One step of a phase's snapshots; 1 when the phase is done. */
static int snap_step(int k, uint64_t now) {
    if (g_snaps < 3 && now >= g_t0 + (uint64_t)g_snaps * SNAP_GAP_US) {
        if (snapshot(g_snaps) != 0) { gamelog_note("[rate] audit: out of memory for the RAM snapshots"); g_state = A_IDLE; return 0; }
        g_snaps++;
    }
    if (g_snaps < 3) return 0;
    g_phase = -1;
    g_ph[k].fps = (double)(psp_display_flips() - g_ph[k].flips0) * 1e6 / (double)(g_snap_t[2] - g_snap_t[0]);
    analyze(&g_ph[k]);
    return 1;
}

static int press_busy(void);

void rateaudit_start(void) {
    if (g_state != A_IDLE || press_busy()) return;
    g_restore = framerate_target();
    if (g_restore == 30) framerate_set(120);              /* at 30: compare 120 with it */
    g_state = A_SETTLE_A;
    g_t0 = psp_sched_now_us();
    gamelog_note("[rate] audit started (target %d fps): keep doing the same thing for ~10 s", g_restore);
}

/* ---- the press audit ------------------------------------------------------------ */

#define PRESS_MS      2500u              /* recorded after each press */
#define PRESS_SAMPLE  33000u             /* us between RAM comparisons */
#define PRESS_SETTLED 300u               /* ms before the end: still moving then = not settled */
#define PRESS_BUTTONS 0xF3F9u            /* D-pad, face buttons, L/R, Start, Select */

enum { P_IDLE, P_WAIT_A, P_REC_A, P_SETTLE_B, P_WAIT_B, P_REC_B, P_SETTLE_C, P_WAIT_C, P_TRACE, P_SETTLE_D, P_WAIT_D, P_TRACE_D };
static int g_pstate = P_IDLE;
static uint64_t g_pt0;                   /* host us: the press, or the step's start */
static uint64_t g_psample;
static uint32_t *g_prev;                 /* RAM as of the last comparison */
static uint16_t *g_first[2], *g_last[2]; /* per word: ms after the press of the first / last change (0 = none) */
static uint8_t *g_nchg[2];               /* per word: samples that saw it change (saturating) */
static int g_ptarget[2], g_phz[2];
static uint64_t g_pflips[2];
static double g_pfps[2];
static uint32_t g_pbuttons;              /* the buttons last vblank */
static int g_pruns;
static int press_busy(void) { return g_pstate != P_IDLE; }

static int press_alloc(void) {
    if (!g_prev && !(g_prev = (uint32_t *)malloc(RAM_SIZE))) return -1;
    for (int k = 0; k < 2; k++) {
        if (!g_first[k] && !(g_first[k] = (uint16_t *)calloc(RAM_WORDS, 2))) return -1;
        if (!g_last[k] && !(g_last[k] = (uint16_t *)calloc(RAM_WORDS, 2))) return -1;
        if (!g_nchg[k] && !(g_nchg[k] = (uint8_t *)calloc(RAM_WORDS, 1))) return -1;
    }
    return 0;
}

static void press_free(void) {
    free(g_prev); g_prev = NULL;
    for (int k = 0; k < 2; k++) {
        free(g_first[k]); free(g_last[k]); free(g_nchg[k]);
        g_first[k] = g_last[k] = NULL; g_nchg[k] = NULL;
    }
}

static int pressed(void) {
    const uint32_t b = psp_ctrl_buttons() & PRESS_BUTTONS;
    const int p = (b & ~g_pbuttons) != 0;
    g_pbuttons = b;
    return p;
}

static void press_begin(int k, uint64_t now) {
    const uint8_t *ram = (const uint8_t *)psp_mem_ptr(RAM_BASE, RAM_SIZE);
    if (ram) memcpy(g_prev, ram, RAM_SIZE);
    memset(g_first[k], 0, RAM_WORDS * 2);
    memset(g_last[k], 0, RAM_WORDS * 2);
    memset(g_nchg[k], 0, RAM_WORDS);
    g_ptarget[k] = framerate_target();
    g_phz[k] = 60 * psp_sched_vblank_rate();
    g_pflips[k] = psp_display_flips();
    g_pt0 = g_psample = now;
}

/* One comparison: the words changed since the last, stamped. 1 when the
 * recording is over. */
static int press_sample(int k, uint64_t now) {
    if (now - g_psample < PRESS_SAMPLE) return 0;
    g_psample = now;
    const uint32_t *ram = (const uint32_t *)psp_mem_ptr(RAM_BASE, RAM_SIZE);
    if (!ram) return 1;
    uint32_t ms = (uint32_t)((now - g_pt0) / 1000u);
    if (ms < 1) ms = 1;
    if (ms > 65535) ms = 65535;
    uint16_t *first = g_first[k], *last = g_last[k];
    uint8_t *nchg = g_nchg[k];
    for (uint32_t i = 0; i < RAM_WORDS; i++) {
        if (ram[i] == g_prev[i]) continue;
        g_prev[i] = ram[i];
        if (!first[i]) first[i] = (uint16_t)ms;
        last[i] = (uint16_t)ms;
        if (nchg[i] < 255) nchg[i]++;
    }
    if (ms < PRESS_MS) return 0;
    g_pfps[k] = (double)(psp_display_flips() - g_pflips[k]) * 1e6 / (double)(now - g_pt0);
    return 1;
}

enum { PK_FRAME, PK_VBLANK, PK_TIME, PK_OTHER };

/* A word that moved after both presses and stopped well before the end. */
static int press_both(uint32_t i) {
    return g_first[0][i] && g_first[1][i] && g_last[0][i] + PRESS_SETTLED < PRESS_MS && g_last[1][i] + PRESS_SETTLED < PRESS_MS
        && g_last[1][i] >= 120;                          /* B long enough to time */
}

static int press_class(uint32_t i, double fr, double vr) {
    const double r = (double)g_last[0][i] / g_last[1][i];   /* how long it moved, A over B */
    if (fabs(r * fr - 1.0) < 0.3) return PK_FRAME;          /* A shorter by the frame-rate ratio */
    if (fabs(vr - 1.0) > 0.3 && fabs(r * vr - 1.0) < 0.3) return PK_VBLANK;
    if (fabs(r - 1.0) < 0.25) return PK_TIME;
    return PK_OTHER;
}

static int press_moving(uint32_t i);

static void press_suspects(double fr, double vr) {
    g_nsus = 0;
    /* moving frame-, vblank- and otherwise-timed words first (OTHER: e.g. motion that
     * finishes later at the higher rate), then the rest of frame and vblank */
    static const int WANT[5] = { PK_FRAME, PK_VBLANK, PK_OTHER, PK_FRAME, PK_VBLANK };
    for (int pass = 0; pass < 5 && g_nsus < MAX_SUSPECTS; pass++)
        for (uint32_t i = 0; i < RAM_WORDS && g_nsus < MAX_SUSPECTS; i++) {
            const int want = WANT[pass];
            if (!press_both(i) || press_class(i, fr, vr) != want || (pass < 3 && !press_moving(i))) continue;
            const uint32_t a = RAM_BASE + i * 4;
            int dup = 0;
            for (int k = 0; k < g_nsus && !dup; k++) dup = (g_sus[k] >> 6) == (a >> 6);
            if (!dup) g_sus[g_nsus++] = a;
        }
    qsort(g_sus, (size_t)g_nsus, sizeof g_sus[0], by_addr);
    for (int i = 0; i < MAX_SUSPECTS; i++) { free(g_sus_stack[i]); g_sus_stack[i] = NULL; }
}

/* Words that moved (changed in several samples) count for more than one-off
 * changes (a model or a texture swapped in): they are what a slide is. */
#define MOVING 3

static int press_moving(uint32_t i) { return g_nchg[0][i] >= MOVING && g_nchg[1][i] >= MOVING; }

typedef struct { uint32_t start, end; int words, moving, floats; uint16_t a0, a1, b0, b1; } run;

static int by_run(const void *x, const void *y) {
    const run *a = (const run *)x, *b = (const run *)y;
    if (a->moving != b->moving) return b->moving - a->moving;
    return b->words - a->words;
}

static void press_report(double fr, double vr) {
    char path[800];
    g_pruns = next_log(path, sizeof path, "press_audit");
    FILE *f = fopen(path, "w");
    if (!f) { gamelog_note("[rate] press audit: cannot write %s", path); return; }
    fprintf(f, "# press audit: press A at a %d fps target (%.1f fps, %d Hz display), press B at %d (%.1f fps, %d Hz)\n",
            g_ptarget[0], g_pfps[0], g_phz[0], g_ptarget[1], g_pfps[1], g_phz[1]);
    fprintf(f, "# per word: ms after the press of its first and last change, A / B, and how many of the 33 ms samples\n"
               "# saw it change (moving: %d or more in both). last A / last B ~1 timed in seconds (right); ~%.2f timed\n"
               "# in frames (too fast at %d); ~%.2f in vblanks\n\n", MOVING, 1.0 / fr, g_ptarget[0], 1.0 / vr);
    const uint32_t *ram = (const uint32_t *)psp_mem_ptr(RAM_BASE, RAM_SIZE);
    int count[4] = {0}, moving[4] = {0};
    for (uint32_t i = 0; i < RAM_WORDS; i++)
        if (press_both(i)) { const int c = press_class(i, fr, vr); count[c]++; moving[c] += press_moving(i); }
    fprintf(f, "moved after both presses and settled (moving / all): frame-timed %d / %d, vblank-timed %d / %d, "
               "time-timed %d / %d, other %d / %d\n", moving[PK_FRAME], count[PK_FRAME], moving[PK_VBLANK],
            count[PK_VBLANK], moving[PK_TIME], count[PK_TIME], moving[PK_OTHER], count[PK_OTHER]);
    static const char *const TITLE[4] = { "FRAME-TIMED (finished too soon at the higher rate)", "VBLANK-TIMED (twice as fast at 120 Hz)",
                                          "TIME-TIMED (right)", "OTHER" };
    run *runs = (run *)malloc(sizeof(run) * 65536);
    for (int want = PK_FRAME; want <= PK_OTHER && runs; want++) {
        /* runs of neighbouring words (gaps up to 32 bytes) of this kind */
        int nr = 0;
        for (uint32_t i = 0; i < RAM_WORDS; i++) {
            if (!press_both(i) || press_class(i, fr, vr) != want) continue;
            const uint32_t a = RAM_BASE + i * 4;
            run *r = nr ? &runs[nr - 1] : NULL;
            if (!r || a - r->end > 32) {
                if (nr == 65536) break;
                r = &runs[nr++];
                memset(r, 0, sizeof *r);
                r->start = a;
                r->a0 = r->b0 = 0xFFFF;
            }
            r->end = a;
            r->words++;
            r->moving += press_moving(i);
            r->floats += ram && plausible_float(ram[i]) && (ram[i] & 0x7F800000u);
            if (g_first[0][i] < r->a0) r->a0 = g_first[0][i];
            if (g_last[0][i] > r->a1) r->a1 = g_last[0][i];
            if (g_first[1][i] < r->b0) r->b0 = g_first[1][i];
            if (g_last[1][i] > r->b1) r->b1 = g_last[1][i];
        }
        qsort(runs, (size_t)nr, sizeof *runs, by_run);
        fprintf(f, "\n== %s: %d groups, moving ones first ==\n%-10s %-10s %6s %6s %6s %13s %13s\n", TITLE[want], nr,
                "from", "to", "words", "moving", "floats", "A ms", "B ms");
        for (int k = 0; k < nr && k < (want == PK_TIME ? 30 : 120); k++)
            fprintf(f, "0x%08X 0x%08X %6d %6d %6d %6u-%-6u %6u-%-6u\n", runs[k].start, runs[k].end + 3, runs[k].words,
                    runs[k].moving, runs[k].floats, runs[k].a0, runs[k].a1, runs[k].b0, runs[k].b1);
        if (want == PK_TIME) continue;
        /* the moving words themselves */
        fprintf(f, "-- moving words --\n%-10s %13s %13s %9s %6s  %s\n", "address", "A ms", "B ms", "samples", "last", "now (hex / float)");
        int shown = 0;
        for (uint32_t i = 0; i < RAM_WORDS && shown < 400; i++) {
            if (!press_both(i) || !press_moving(i) || press_class(i, fr, vr) != want) continue;
            const uint32_t w = ram ? ram[i] : 0;
            fprintf(f, "0x%08X %6u-%-6u %6u-%-6u %4u/%-4u %6.2f  %08X / %g\n", RAM_BASE + i * 4, g_first[0][i], g_last[0][i],
                    g_first[1][i], g_last[1][i], g_nchg[0][i], g_nchg[1][i], (double)g_last[0][i] / g_last[1][i], w, as_float(w));
            shown++;
        }
        if (!shown) fprintf(f, "(none)\n");
    }
    free(runs);
    fprintf(f, "\n== HOW THEY MOVE (every change after press C at %d fps, and after press D at 30: ms value) ==\n",
            g_ptarget[0]);
    for (int i = 0; i < g_nsus; i++) {
        if (!g_ncurve[0][i] && !g_ncurve[1][i]) continue;
        for (int ph = 0; ph < 2; ph++) {
            fprintf(f, "0x%08X %s:", g_sus[i], ph ? "30 " : "hi ");
            for (int k = 0; k < g_ncurve[ph][i]; k++) {
                const uint32_t v = g_curve[ph][i][k].v;
                if (plausible_float(v) && (v & 0x7F800000u)) fprintf(f, " %u:%g", g_curve[ph][i][k].ms, as_float(v));
                else fprintf(f, " %u:%d", g_curve[ph][i][k].ms, (int32_t)v);
            }
            fprintf(f, "%s\n", g_ncurve[ph][i] == CURVE_POINTS ? " ..." : "");
        }
    }
    fprintf(f, "\n== WHO WRITES THEM (first write of each after press C at %d fps; moving ones first) ==\n", g_restore);
    for (int i = 0; i < g_nsus; i++)
        fprintf(f, "0x%08X  %s\n", g_sus[i], g_sus_stack[i] ? (g_sus_stack[i][0] ? g_sus_stack[i] : "(no game stack)") : "(not written)");
    if (!g_nsus) fprintf(f, "(no suspects)\n");
    fclose(f);
    gamelog_note("[rate] press audit %d written to %s: moving / all words -- frame-timed %d / %d, vblank-timed %d / %d, "
                 "time-timed %d / %d (A %d fps target, %.1f fps; B %d, %.1f fps)", g_pruns, path, moving[PK_FRAME],
                 count[PK_FRAME], moving[PK_VBLANK], count[PK_VBLANK], moving[PK_TIME], count[PK_TIME],
                 g_ptarget[0], g_pfps[0], g_ptarget[1], g_pfps[1]);
}

void rateaudit_start_press(void) {
    if (g_state != A_IDLE || g_pstate != P_IDLE) return;
    if (press_alloc() != 0) { press_free(); gamelog_note("[rate] press audit: out of memory"); return; }
    g_restore = framerate_target();
    if (g_restore == 30) framerate_set(120);
    g_pstate = P_WAIT_A;
    g_pt0 = psp_sched_now_us();
    g_pbuttons = psp_ctrl_buttons() & PRESS_BUTTONS;
    gamelog_note("[rate] press audit started (target %d fps): press a button once when asked, three times", g_restore);
}

/* Waiting for a press: 30 s, then the audit gives up. */
static int press_wait(uint64_t now, const char *what) {
    snprintf(g_status, sizeof g_status, "PRESS AUDIT: %s", what);
    if (now - g_pt0 > 30000000u) {
        gamelog_note("[rate] press audit: no press within 30 s, stopped");
        framerate_set(g_restore);
        press_free();
        g_pstate = P_IDLE;
        return 0;
    }
    return pressed();
}

static void press_step(uint64_t now) {
    switch (g_pstate) {
    case P_IDLE: return;
    case P_WAIT_A: {
        char what[64];
        snprintf(what, sizeof what, "PRESS ONCE NOW (%d FPS)", framerate_target());
        if (now - g_pt0 < SETTLE_US) { pressed(); snprintf(g_status, sizeof g_status, "PRESS AUDIT: WAIT..."); return; }
        if (press_wait(now, what)) { press_begin(0, now); g_pstate = P_REC_A; }
        return;
    }
    case P_REC_A:
        snprintf(g_status, sizeof g_status, "PRESS AUDIT: RECORDING (DON'T PRESS)");
        if (!press_sample(0, now)) return;
        framerate_set(30);
        g_pt0 = now;
        g_pstate = P_SETTLE_B;
        return;
    case P_SETTLE_B:
        snprintf(g_status, sizeof g_status, "PRESS AUDIT: WAIT...");
        pressed();
        if (now - g_pt0 < SETTLE_US) return;
        g_pt0 = now;
        g_pstate = P_WAIT_B;
        return;
    case P_WAIT_B:
        if (press_wait(now, "PRESS ONCE NOW (30 FPS)")) { press_begin(1, now); g_pstate = P_REC_B; }
        return;
    case P_REC_B:
        snprintf(g_status, sizeof g_status, "PRESS AUDIT: RECORDING (DON'T PRESS)");
        if (!press_sample(1, now)) return;
        framerate_set(g_restore == 30 ? 120 : g_restore);  /* the trace at the higher rate */
        g_pt0 = now;
        g_pstate = P_SETTLE_C;
        return;
    case P_SETTLE_C:
        snprintf(g_status, sizeof g_status, "PRESS AUDIT: WAIT...");
        pressed();
        if (now - g_pt0 < SETTLE_US) return;
        g_pt0 = now;
        g_pstate = P_WAIT_C;
        return;
    case P_WAIT_C: {
        if (!press_wait(now, "PRESS ONCE MORE (FINDING THE CODE)")) return;
        const double fr = g_pfps[0] / (g_pfps[1] > 0 ? g_pfps[1] : 1), vr = (double)g_phz[0] / g_phz[1];
        press_suspects(fr, vr);
        memset(g_ncurve, 0, sizeof g_ncurve);
        g_curve_phase = 0;
        g_curve_t0 = now;
        psp_mem_trace_writes(trace_write);
        g_pt0 = now;
        g_pstate = P_TRACE;
        return;
    }
    case P_TRACE:
        snprintf(g_status, sizeof g_status, "PRESS AUDIT: FINDING THE CODE (DON'T PRESS)");
        if (now - g_pt0 < (uint64_t)PRESS_MS * 1000u) return;
        g_curve_phase = -1;
        psp_mem_trace_writes(NULL);
        framerate_set(30);                        /* press D: the same curves at 30 */
        g_pt0 = now;
        g_pstate = P_SETTLE_D;
        return;
    case P_SETTLE_D:
        snprintf(g_status, sizeof g_status, "PRESS AUDIT: WAIT...");
        pressed();
        if (now - g_pt0 < SETTLE_US) return;
        g_pt0 = now;
        g_pstate = P_WAIT_D;
        return;
    case P_WAIT_D:
        if (!press_wait(now, "PRESS ONCE MORE (LAST, 30 FPS)")) return;
        g_curve_phase = 1;
        g_curve_t0 = now;
        psp_mem_trace_writes(trace_write);
        g_pt0 = now;
        g_pstate = P_TRACE_D;
        return;
    case P_TRACE_D: {
        snprintf(g_status, sizeof g_status, "PRESS AUDIT: RECORDING (DON'T PRESS)");
        if (now - g_pt0 < (uint64_t)PRESS_MS * 1000u) return;
        g_curve_phase = -1;
        psp_mem_trace_writes(NULL);
        const double fr = g_pfps[0] / (g_pfps[1] > 0 ? g_pfps[1] : 1), vr = (double)g_phz[0] / g_phz[1];
        press_report(fr, vr);
        framerate_set(g_restore);
        press_free();
        g_pstate = P_IDLE;
        return;
    }
    }
}

const char *rateaudit_status(void) { return g_state == A_IDLE && g_pstate == P_IDLE ? "" : g_status; }

void rateaudit_vblank(void) {
    const uint64_t now = psp_sched_now_us();
    {   /* at 60 / 120 fps, every 30 s: the actual rate and the busiest vblank users */
        static uint64_t last_t, last_flips, last_n[MAX_SITES];
        if (!last_t) { last_t = now; last_flips = psp_display_flips(); }
        if (now - last_t >= 30000000u) {
            if (framerate_target() >= 60 && g_state == A_IDLE) {
                char line[600];
                const double secs = (double)(now - last_t) / 1e6;
                size_t n = (size_t)snprintf(line, sizeof line, "[rate] %d fps target: %.1f fps over %.0f s; vblank use /s:",
                                            framerate_target(), (double)(psp_display_flips() - last_flips) / secs, secs);
                for (int i = 0; i < g_nsites && n < sizeof line - 40; i++)
                    if (g_site[i].n > last_n[i])
                        n += (size_t)snprintf(line + n, sizeof line - n, " %s 0x%08X %.1f", g_site[i].kind ? "vcount" : "wait",
                                              g_site[i].ra - 8, (double)(g_site[i].n - last_n[i]) / secs);
                gamelog_note("%s", line);
            }
            last_t = now;
            last_flips = psp_display_flips();
            for (int i = 0; i < g_nsites; i++) last_n[i] = g_site[i].n;
        }
    }
    press_step(now);
    switch (g_state) {
    case A_IDLE: return;
    case A_SETTLE_A:
        snprintf(g_status, sizeof g_status, "RATE AUDIT: SETTLING AT %d FPS", framerate_target());
        if (now - g_t0 < SETTLE_US) return;
        begin_phase(0);
        g_t0 = now;
        g_state = A_SNAP_A;
        return;
    case A_SNAP_A:
        snprintf(g_status, sizeof g_status, "RATE AUDIT: MEASURING AT %d FPS (KEEP GOING)", g_ph[0].target);
        if (!snap_step(0, now)) return;
        framerate_set(30);
        g_t0 = now;
        g_state = A_SETTLE_B;
        return;
    case A_SETTLE_B:
        snprintf(g_status, sizeof g_status, "RATE AUDIT: SETTLING AT 30 FPS");
        if (now - g_t0 < SETTLE_US) return;
        begin_phase(1);
        g_t0 = now;
        g_state = A_SNAP_B;
        return;
    case A_SNAP_B:
        snprintf(g_status, sizeof g_status, "RATE AUDIT: MEASURING AT 30 FPS (KEEP GOING)");
        if (!snap_step(1, now)) return;
        framerate_set(g_restore);
        pick_suspects();
        psp_mem_trace_writes(trace_write);
        g_t0 = now;
        g_state = A_TRACE;
        return;
    case A_TRACE:
        snprintf(g_status, sizeof g_status, "RATE AUDIT: FINDING THE CODE");
        if (now - g_t0 < TRACE_US) return;
        psp_mem_trace_writes(NULL);
        report();
        for (int k = 0; k < 3; k++) { free(g_snap[k]); g_snap[k] = NULL; }
        g_state = A_IDLE;
        return;
    }
}

void rateaudit_init(const char *exe_dir) {
    snprintf(g_dir, sizeof g_dir, "%s", exe_dir);
    psp_display_set_vblank_use_hook(vblank_use);
}
