/* Function-activity recorder: which game functions run during an action.
 *
 * F9 starts a recording, F9 again stops it and writes func_counts_N.txt next
 * to the exe: every recompiled function entered meanwhile, with its call
 * count and calls per second, most called first. Comparing a recording made
 * during an action (an NPC turning, an effect playing) with one made in the
 * same place without it shows the functions that drive that action -- the
 * way to find code with no symbols to search by.
 *
 * It needs the trace build (PSP2I_TRACE: the generated code reports every
 * function entry); in a normal build the recording comes out empty and says
 * so. */
#include "functrace.h"
#include "online.h"

#include <psprecomp/dispatch.h>
#include <psprecomp/hle.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psprecomp/cpu.h>
#include <psprecomp/mem.h>

#define FT_SLOTS (1u << 17)                 /* power of two, well above the game's function count */

typedef struct { uint32_t addr, count; } ft_slot;

static ft_slot *g_slots;
static int g_on, g_files;
static uint32_t g_used;
static uint64_t g_t0, g_vb0, g_flips0, g_calls;
static char g_dir[700];

static void enter(uint32_t addr) {
    if (!g_on) return;
    g_calls++;
    uint32_t i = (addr * 2654435761u) & (FT_SLOTS - 1);
    while (g_slots[i].addr && g_slots[i].addr != addr) i = (i + 1) & (FT_SLOTS - 1);
    if (!g_slots[i].addr) {
        if (g_used >= FT_SLOTS - FT_SLOTS / 8) return;   /* full: stop adding (never in practice) */
        g_slots[i].addr = addr;
        g_used++;
    }
    g_slots[i].count++;
}

/* While recording, also log the NPC look-at (0x08A75624) frame by frame to
 * lookat_N.txt: the remaining angle, the step and the joint angles at each of
 * its decision points, to see why it turns too far at 60 fps. */
#define LA_MAX_LINES 200000

static FILE *g_la;
static uint32_t g_la_lines;

static int32_t rd(uint32_t a) { return (int32_t)psp_read32(a); }

static void lookat_mark(uint32_t addr) {
    if (!g_la || g_la_lines >= LA_MAX_LINES) return;
    const uint32_t *r = psp_cpu.r;
    const uint32_t s0 = r[PSP_REG_S0], sp = r[PSP_REG_SP];
    const unsigned long long f = (unsigned long long)psp_display_flips();
    switch (addr) {
    case 0x08A75624u:                       /* entry: a0 = look-at state */
        fprintf(g_la, "%llu %08X enter state=%d flags=%X yaw=%d,%d,%d pitch=%d,%d,%d\n", f, r[PSP_REG_A0],
                rd(r[PSP_REG_A0] + 96), rd(r[PSP_REG_A0] + 92), rd(r[PSP_REG_A0] + 24), rd(r[PSP_REG_A0] + 52),
                rd(r[PSP_REG_A0] + 80), rd(r[PSP_REG_A0] + 20), rd(r[PSP_REG_A0] + 48), rd(r[PSP_REG_A0] + 76));
        break;
    case 0x08A756ECu:                       /* yaw: s2 remaining, s1 step */
        fprintf(g_la, "%llu %08X yaw rem=%d step=%d meas=%d target=%d,%d\n", f, s0, (int32_t)r[PSP_REG_S2],
                (int32_t)r[PSP_REG_S1], rd(s0 + 12), rd(sp), rd(sp + 4));
        break;
    case 0x08A75788u:                       /* yaw within the finish threshold: snap */
        fprintf(g_la, "%llu %08X yaw snap rem=%d\n", f, s0, (int32_t)r[PSP_REG_S2]);
        break;
    case 0x08A757D0u:                       /* yaw done (snapped, or turn limit reached) */
        fprintf(g_la, "%llu %08X yaw done\n", f, s0);
        break;
    case 0x08A75850u:                       /* pitch: s3 remaining, s1 step */
        fprintf(g_la, "%llu %08X pitch rem=%d step=%d meas=%d acc=%d\n", f, s0, (int32_t)r[PSP_REG_S3],
                (int32_t)r[PSP_REG_S1], rd(s0 + 8), rd(r[PSP_REG_S2] + 16));
        break;
    case 0x08A758C4u:                       /* not state 1: returning to neutral (state 2) */
        fprintf(g_la, "%llu %08X back\n", f, s0);
        break;
    case 0x08A537B4u: {                     /* body turn start: a0 turn state, a1 object, a2 target, a3 speed, t0 threshold */
        const uint32_t obj = r[PSP_REG_A1];
        fprintf(g_la, "%llu %08X turn start obj=%08X target=%d speed=%d threshold=%d facing=%d caller=%08X\n", f,
                r[PSP_REG_A0], obj, (int32_t)r[PSP_REG_A2], (int32_t)r[PSP_REG_A3], (int32_t)r[PSP_REG_T0],
                obj ? rd(obj + 320) : 0, r[PSP_REG_RA] - 8);
        break;
    }
    case 0x08A53494u: {                     /* body turn update, logged while turning (flag 4) */
        const uint32_t ts = r[PSP_REG_A0], obj = r[PSP_REG_A1];
        if (!obj || !(psp_read32(ts + 4) & 4)) return;
        fprintf(g_la, "%llu %08X turn obj=%08X target=%d step=%d facing=%d flags=%X\n", f, ts, obj, rd(ts + 20),
                rd(ts + 36), rd(obj + 320), psp_read32(ts + 4));
        break;
    }
    case 0x08A53670u:                       /* body turn within one step: snap to the target, done */
        fprintf(g_la, "%llu %08X turn snap\n", f, r[PSP_REG_S1]);
        break;
    default: return;
    }
    g_la_lines++;
}

void functrace_init(const char *exe_dir) {
    snprintf(g_dir, sizeof g_dir, "%s", exe_dir);
    psp_trace_set_enter_hook(enter);
}

static int by_count(const void *a, const void *b) {
    const ft_slot *x = (const ft_slot *)a, *y = (const ft_slot *)b;
    if (x->count != y->count) return x->count < y->count ? 1 : -1;
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

static void stop_and_write(void) {
    g_on = 0;
    const double secs = (double)(psp_sched_now_us() - g_t0) / 1e6;
    const uint64_t vbl = psp_sched_vblank_count() - g_vb0, flips = psp_display_flips() - g_flips0;
    ft_slot *list = (ft_slot *)malloc((size_t)(g_used ? g_used : 1) * sizeof *list);
    uint32_t n = 0;
    for (uint32_t i = 0; i < FT_SLOTS && list; i++) if (g_slots[i].addr) list[n++] = g_slots[i];
    if (list) qsort(list, n, sizeof *list, by_count);
    char path[800];
    snprintf(path, sizeof path, "%s/func_counts_%d.txt", g_dir, ++g_files);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "# function activity: %.1f s, %llu vblanks, %llu frames (%.1f fps), %u functions, %llu calls\n",
                secs, (unsigned long long)vbl, (unsigned long long)flips, secs > 0 ? (double)flips / secs : 0.0,
                n, (unsigned long long)g_calls);
        if (!n) fprintf(f, "# nothing recorded: use the trace build (run_func_trace.bat)\n");
        fprintf(f, "# address    calls      per second  per frame\n");
        for (uint32_t i = 0; i < n; i++)
            fprintf(f, "0x%08X %10u %11.1f %10.3f\n", list[i].addr, list[i].count,
                    secs > 0 ? list[i].count / secs : 0.0, flips ? (double)list[i].count / (double)flips : 0.0);
        fclose(f);
    }
    free(list);
    psp_trace_set_mark_hook(NULL);
    if (g_la) { fclose(g_la); g_la = NULL; }
    online_log("functrace: recording %d stopped after %.1f s (%u functions) -> %s", g_files, secs, n, path);
}

void functrace_toggle(void) {
    if (g_on) { stop_and_write(); return; }
    if (!g_slots) g_slots = (ft_slot *)calloc(FT_SLOTS, sizeof *g_slots);
    if (!g_slots) return;
    memset(g_slots, 0, FT_SLOTS * sizeof *g_slots);
    g_used = 0;
    g_calls = 0;
    g_t0 = psp_sched_now_us();
    g_vb0 = psp_sched_vblank_count();
    g_flips0 = psp_display_flips();
    char path[800];
    snprintf(path, sizeof path, "%s/lookat_%d.txt", g_dir, g_files + 1);
    g_la = fopen(path, "w");
    g_la_lines = 0;
    if (g_la) {
        fprintf(g_la, "# NPC look-at per frame (frame, look-at state address, values); angles 0x10000 = 360 deg\n");
        psp_trace_set_mark_hook(lookat_mark);
    }
    g_on = 1;
    online_log("functrace: recording %d started (F9 again to stop)", g_files + 1);
}

int functrace_recording(void) { return g_on; }
