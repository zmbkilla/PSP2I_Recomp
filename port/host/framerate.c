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
 * With --fps 60 the hook turns requests for 30 and 20 into 60 before the
 * original runs, so all the derived timing is consistent with 60 -- the
 * same places the PPSSPP 60 FPS cheat rewrites one constant at a time
 * (0x0896E914, 0x08B24A50, 0x08B34B04/08/78, 0x08B82284, the table bytes).
 * The 20 fps fallback exists for the PSP's CPU budget, which a native port
 * does not have. 15 (the table's slow modes) is left as the game asks.
 * Hooked through allegrexrecomp --hooks (port/hooks.txt). */

#include "framerate.h"

#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>

#include <stdio.h>

#define FN_SET_FRAME_RATE 0x08B81890u

static int g_target = 30;

static void hook_set_frame_rate(void (*original)(void)) {
    const uint32_t asked = psp_cpu.r[4];                /* $a0: fps */
    if (g_target == 60 && (asked == 30 || asked == 20)) psp_cpu.r[4] = 60;
    original();
}

void framerate_init(int fps) {
    g_target = fps == 60 ? 60 : 30;
    if (g_target == 60) {
        psp_hook_set(FN_SET_FRAME_RATE, hook_set_frame_rate);
        fprintf(stderr, "framerate: 60 fps (the game's 30/20 fps requests run at 60 through its own timing)\n");
    }
}
