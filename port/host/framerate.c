/* PSP2i's frame rate, through the game's own frame-rate API.
 *
 * The engine is frame-rate aware: setFrameRate(fps) (0x08B81890) stores fps
 * in its timing state (0x08EDA54C, via 0x08B81830) and derives everything
 * else from it there -- dt = 1/fps (+0x1C), the 30 Hz step 30/fps (+0x18),
 * vblanks per frame 60/fps (+0x10/+0x14) -- then sets the vblank interval
 * (0x08B85A20, 60/fps). The game normally asks for 30 (scene entry:
 * 0x0896E914, 0x08B24A50, 0x08B82284; "back to 30": 0x08B34B78), drops to
 * 20 when a scene is heavy (0x08B34AE8 picks 30 or 20), and has a mode
 * table at 0x08E9ABE8 of [30, 20, 15, 15] (0x08B81F8C).
 *
 * At a 60 fps target the hook turns requests for 30 and 20 into 60 before the
 * original runs, so all the derived timing is consistent with 60 -- the
 * same places the PPSSPP 60 FPS cheat rewrites one constant at a time
 * (0x0896E914, 0x08B24A50, 0x08B34B04/08/78, 0x08B82284, the table bytes).
 * The 20 fps fallback exists for the PSP's CPU budget, which a native port
 * does not have. 15 (the table's slow modes) is left as the game asks.
 * Hooked through allegrexrecomp --hooks (port/hooks.txt).
 *
 * The target can change at run time (the settings menu, framerate_set); see
 * hook_frame.
 *
 * ## No slowdown: the step follows the frames actually shown
 *
 * The frame function (0x08B85AB0) paces frames by vblanks: it waits out the
 * interval (60/fps vblanks) since the vcount it recorded when the previous
 * frame ended (0x08EDA698), and always waits for at least the next vblank.
 * A frame that runs over its interval therefore lasts a whole extra vblank,
 * while the game still advances its world by the step for the rate it asked
 * for -- at 60 fps, one frame of 17 ms is shown for two vblanks yet moves the
 * world half as far: slow motion, which is what the slowdowns were.
 *
 * So after each frame the hook counts the vblanks that frame really took (N)
 * and gives the next frame the timing of the rate that matches, 60/N fps (60,
 * 30, 20 or 15; never above the rate the game runs at), through the game's
 * own timing fill (0x08B81830 on the timing state, plus the fps field of the
 * object 0x08D6845C keeps), exactly as when the game itself moves between 30
 * and 20 fps. The vblank interval is left alone, so the game keeps aiming for
 * its full rate and returns to it as soon as frames fit again. Each frame
 * carries the previous frame's duration, so the steps add up to the time
 * that passed: the game keeps its speed, and a heavy moment costs frames,
 * not speed. Frames over 4 vblanks (loading) are counted as 4. */

#include "framerate.h"

#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/mem.h>

#include <stdio.h>

#define FN_SET_FRAME_RATE 0x08B81890u
#define FN_FRAME          0x08B85AB0u   /* the main loop's per-frame pacing call (from 0x08B86320) */
#define TIMING_FPS_INT    0x08EDA558u   /* the timing state's +0x0C: fps as an int */
#define FN_TIMING_FILL    0x08B81830u   /* (state, fps): fps, 60/fps, 30/fps, 1/fps into the state */
#define TIMING_STATE      0x08EDA54Cu
#define FRAME_END_VCOUNT  0x08EDA698u   /* the vcount the frame function records at its end */
#define FPS_OBJECT_PTR    0x08EE2138u   /* the object whose +4 0x08D6845C sets to fps */

static int g_target = 30;
static int g_asked;      /* the game's last own request (before mapping); 0 = none yet */
static int g_pending;    /* the target changed: re-issue g_asked at the next frame */
static int g_base;       /* the rate the game runs at (its request after mapping) */
static int g_applied;    /* the rate the timing state holds now (g_base, or lower after a long frame) */
static int g_adapt_log;  /* rate changes logged so far */

static void hook_set_frame_rate(void (*original)(void)) {
    const uint32_t asked = psp_cpu.r[4];                /* $a0: fps */
    g_asked = (int)asked;
    if (g_target == 60 && (asked == 30 || asked == 20)) psp_cpu.r[4] = 60;
    g_base = g_applied = (int)psp_cpu.r[4];
    original();
}

/* The next frame's timing for a frame that took `n` vblanks. */
static void adapt(uint32_t n) {
    if (g_base <= 0) return;
    if (n < 1) n = 1;
    if (n > 4) n = 4;
    int eff = 60 / (int)n;
    if (eff > g_base) eff = g_base;
    if (eff == g_applied) return;
    psp_fn_t fill = psp_lookup(FN_TIMING_FILL);
    if (!fill) return;
    const psp_cpu_state saved = psp_cpu;
    psp_cpu.r[4] = TIMING_STATE;
    psp_cpu.r[5] = (uint32_t)eff;
    psp_cpu.r[31] = 0;
    fill();
    psp_cpu = saved;
    const uint32_t obj = psp_read32(FPS_OBJECT_PTR);
    if (obj) psp_write32(obj + 4, (uint32_t)eff);
    if (g_adapt_log < 20) {
        g_adapt_log++;
        fprintf(stderr, "framerate: a frame took %u vblank(s); the next one steps as %d fps (the game runs at %d)%s\n",
                n, eff, g_base, g_adapt_log == 20 ? " -- further changes not logged" : "");
    }
    g_applied = eff;
}

/* Runtime switching. The game calls setFrameRate only when it decides to
 * (scene entry, the 30/20 choice), so a new setting is applied by calling it
 * again with the game's own last request, from the game's main thread at a
 * frame boundary: the start of the per-frame pacing function, before it waits
 * out the vblank interval. The hook above maps that request for the new
 * target, so every derived value (dt, 30/fps, the vblank interval) changes
 * together, as when the game changes rate itself (it does, between 30 and 20,
 * mid-scene). The CPU registers are saved and restored around the call, so
 * the frame function runs with the state it was entered with. */
static void hook_frame(void (*original)(void)) {
    if (g_pending) {
        g_pending = 0;
        psp_fn_t set = g_asked ? psp_lookup(FN_SET_FRAME_RATE) : NULL;
        if (set) {
            const psp_cpu_state saved = psp_cpu;
            psp_cpu.r[4] = (uint32_t)g_asked;
            psp_cpu.r[31] = 0;           /* setFrameRate saves and restores $ra itself */
            set();
            psp_cpu = saved;
            fprintf(stderr, "framerate: switched to a %d fps target (game timing now %d fps)\n",
                    g_target, framerate_game_fps());
        }
    }
    const uint32_t before = psp_read32(FRAME_END_VCOUNT);
    original();                               /* the frame's wait: it ends at a vblank */
    if (before) adapt(psp_read32(FRAME_END_VCOUNT) - before);
}

void framerate_init(int fps) {
    g_target = fps == 60 ? 60 : 30;
    /* Both hooks always: at a 30 fps target they pass everything through
     * unchanged, and they are what lets the target change at run time. */
    psp_hook_set(FN_SET_FRAME_RATE, hook_set_frame_rate);
    psp_hook_set(FN_FRAME, hook_frame);
    if (g_target == 60)
        fprintf(stderr, "framerate: 60 fps (the game's 30/20 fps requests run at 60 through its own timing)\n");
}

void framerate_set(int fps) {
    fps = fps == 60 ? 60 : 30;
    if (fps == g_target) return;
    g_target = fps;
    g_pending = 1;
}

int framerate_target(void) { return g_target; }

int framerate_game_fps(void) { return g_asked ? (int)psp_read32(TIMING_FPS_INT) : 0; }
