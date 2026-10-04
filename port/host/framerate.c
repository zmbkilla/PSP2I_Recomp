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
 * hook_frame. */

#include "framerate.h"

#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/mem.h>

#include <stdio.h>

#define FN_SET_FRAME_RATE 0x08B81890u
#define FN_FRAME          0x08B85AB0u   /* the main loop's per-frame pacing call (from 0x08B86320) */
#define TIMING_FPS_INT    0x08EDA558u   /* the timing state's +0x0C: fps as an int */

static int g_target = 30;
static int g_asked;      /* the game's last own request (before mapping); 0 = none yet */
static int g_pending;    /* the target changed: re-issue g_asked at the next frame */

static void hook_set_frame_rate(void (*original)(void)) {
    const uint32_t asked = psp_cpu.r[4];                /* $a0: fps */
    g_asked = (int)asked;
    if (g_target == 60 && (asked == 30 || asked == 20)) psp_cpu.r[4] = 60;
    original();
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
    original();
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
