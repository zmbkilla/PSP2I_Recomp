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
 * ## 120 fps: a 120 Hz display
 *
 * The frame function never waits less than one vblank, so 60 is as fast as
 * the PSP's 59.94 Hz display allows. At a 120 fps target the runtime's
 * emulated display refreshes twice as fast (psp_sched_set_vblank_rate(2)):
 * the frame function's vblank waits end every 8.3 ms, the requests for 30,
 * 20 and 60 become 120 (one vblank per frame), and the game's own slow modes
 * (15) are doubled to stay real time, since their vblank intervals now pass
 * twice as fast. Anything else in the game that counts vblanks as 1/60 s now
 * runs twice as fast too -- what port/host/rateaudit.c is there to find.
 *
 * ## Menu auto-repeat at 120 fps
 *
 * The input library's repeat (0x08D5B844, every held button each frame)
 * advances a per-button timer by 0x08D63128's 60 / fps -- 2 at 30, 1 at 60,
 * and 0 at 120 (integer division), where a timer of 0 means "fire": a held
 * button repeated every frame, so one press on a menu (the character select)
 * moved two or three steps. At over 60 fps the hook runs it with the timer
 * in finer steps -- 60 / fps read as 1 (the fps object's +4 set to 60 for the
 * call) -- and the repeat delay and interval (0x08EE1FC4 / 0x08EE1FC8)
 * multiplied to match (x2 at 120), all put back afterwards: the same repeat
 * in real time as at 30 and 60.
 *
 * ## The weapon palette's animation window
 *
 * A slot switch sets the palette's +364 to 8 (0x089E1400 / 1478 / 15B0 /
 * 16F0; 0x089E184C and 1C38 OR in 2), and its per-frame update (0x089E1F50)
 * steps the palette's UI animation by the frame time only while that counts
 * down -- one per frame -- and rarely after. 8 frames were 267 ms at 30 fps,
 * 133 ms at 60 (just the animation's length) and 67 ms at 120: the switch
 * stopped half way and crawled to its end (the press audit's curves: frame
 * 0 -> 2.25 at the normal pace in 161 ms, then +0.25 every ~150 ms). Above 30
 * fps the hook puts back the 1 it took off on frames a 30 fps game would not
 * have, so the window lasts 8 x 33 ms again; the animation still steps every
 * frame.
 *
 * ## The game's 1/60 s counters, paced as at 30 (always above 60 fps)
 *
 * Besides dt and the 30 Hz step, the timing fill stores the frame's length in
 * 1/60 s ticks as a whole number, round(60/fps) (+0x14, 0x08EDA560), and the
 * game counts a lot of time with it: 114 places add it to a clock (33),
 * count a timer down by it (71) or multiply a per-tick speed by it (6) --
 * animation clocks (the character select's carousel, 0x089F0E08 on each
 * character), fades, cooldowns. At 30 fps it is 2, exact; at 60 it is 1,
 * exact but every such clock then advances in steps the game never made on a
 * PSP; at 120 it would be 0.5 and is rounded to 1, so all of them ran twice
 * as fast (found with rateaudit.c's press audit: the character select
 * finished its motion in 433 ms at 120 fps against 800 at 30).
 *
 * Above 60 fps (always; confirmed at 120: PP regenerating too fast, enemies
 * dying sooner, the roll and the quest-start countdown out of step all came
 * from it) and, with the engine menu's TESTS > TICK PACING, also at 60, the
 * hook sets it after every frame the way 30 fps would see it: 2 once two ticks of real frame time have passed, 0 in between --
 * 2, 0 at 60; 2, 0, 0, 0 at 120. The counters keep real time and advance by
 * exactly the steps they do at 30 (no division by it anywhere, and a 0 is a
 * frame where they stand still). Smooth motion uses dt and the 30 Hz step,
 * which stay per frame.
 *
 * ## Test: logic kept at 30 Hz (TESTS > 30 HZ LOGIC)
 *
 * Some game logic counts frames outright -- no step, no ticks: the
 * character select's update (0x08B20E34, a virtual of its scene object) counts
 * a countdown down by one each call and steps through its three slots
 * (0x08B211AC: +44 -= 1, +48 = (+48 + 1) % 3), so its carousel ran four times
 * as fast at 120 fps (rateaudit.c's press audit: settled in ~435 ms against
 * ~700 at 30). Switched on in the engine menu (TESTS, off by default), such
 * functions run only on the frames a 30 fps game would have -- those that get the 2-tick step above, every 2nd frame at
 * 60 and every 4th at 120 -- so they run exactly as at 30. Drawing stays per
 * frame. LOGIC_30HZ lists them.
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
#include <psprecomp/hle.h>
#include <psprecomp/mem.h>

#include <stdio.h>

#define FN_SET_FRAME_RATE 0x08B81890u
#define FN_FRAME          0x08B85AB0u   /* the main loop's per-frame pacing call (from 0x08B86320) */
#define TIMING_FPS_INT    0x08EDA558u   /* the timing state's +0x0C: fps as an int */
#define FN_TIMING_FILL    0x08B81830u   /* (state, fps): fps, 60/fps, 30/fps, 1/fps into the state */
#define TIMING_STATE      0x08EDA54Cu
#define FRAME_END_VCOUNT  0x08EDA698u   /* the vcount the frame function records at its end */
#define FPS_OBJECT_PTR    0x08EE2138u   /* the object whose +4 0x08D6845C sets to fps */
#define TIMING_TICKS      0x08EDA560u   /* the timing state's +0x14: the frame in 1/60 s ticks, round(60/fps) */

static int g_target = 30;
static int g_asked;      /* the game's last own request (before mapping); 0 = none yet */
static int g_pending;    /* the target changed: re-issue g_asked at the next frame */
static int g_base;       /* the rate the game runs at (its request after mapping) */
static int g_applied;    /* the rate the timing state holds now (g_base, or lower after a long frame) */
static int g_adapt_log;  /* rate changes logged so far */
static double g_tick_acc; /* 1/60 s ticks of frame time not yet given to the game */
static int g_tick_log;
static uint32_t g_ticks_now = 2; /* the ticks the current frame sees (2 = a frame a 30 fps game would have) */
static int g_test_ticks, g_test_logic;   /* the engine menu's TESTS (off by default) */
static int g_ticks_ours;  /* the tick field holds a value written here */

void framerate_set_tests(int ticks, int logic) {
    g_test_ticks = ticks != 0;
    g_test_logic = logic != 0;
    fprintf(stderr, "framerate: tests -- tick pacing %s, 30 Hz logic %s\n", g_test_ticks ? "on" : "off", g_test_logic ? "on" : "off");
}

/* Game logic that counts frames, run as at 30 (see the top). */
static const uint32_t LOGIC_30HZ[] = {
    0x08B20E34u,                 /* character select's update: carousel countdown and slots (0x08B211AC) */
};
static int g_gate_log;

static void hook_30hz(void (*original)(void)) {
    if (!g_test_logic || g_applied <= 30 || g_ticks_now == 2) { original(); return; }
    if (!g_gate_log) {
        g_gate_log = 1;
        fprintf(stderr, "framerate: frame-counted logic (character select ...) runs on every %d%s frame only, as at 30\n",
                g_applied / 30, g_applied / 30 == 2 ? "nd" : "th");
    }
}

#define FN_PALETTE_UPDATE 0x089E1F50u  /* weapon palette, per frame: +364 -= 1 and a UI step while > 0 */

/* The palette's window counted on 30 Hz frames only (see the top). */
static void hook_palette_update(void (*original)(void)) {
    const uint32_t obj = psp_cpu.r[4];
    const uint32_t before = obj ? psp_read32(obj + 364) : 0;
    original();
    if (g_applied > 30 && g_ticks_now != 2 && before > 0 && psp_read32(obj + 364) == before - 1)
        psp_write32(obj + 364, before);
}

#define FN_INPUT_REPEAT  0x08D5B844u   /* held buttons' repeat, every frame (from 0x08D5B604) */
#define REPEAT_DELAY_AT  0x08EE1FC4u   /* timer value of the first repeat */
#define REPEAT_NEXT_AT   0x08EE1FC8u   /* (minus the delay) the interval of the following ones */

/* The repeat at over 60 fps: the timer in 1/(60 k) s steps, the delay and
 * interval multiplied by k (see the top). */
static void hook_input_repeat(void (*original)(void)) {
    const uint32_t obj = psp_read32(FPS_OBJECT_PTR);
    const uint32_t fps = obj ? psp_read32(obj + 4) : 0;
    if (fps <= 60) { original(); return; }
    const uint32_t k = fps / 60;
    const uint32_t delay = psp_read32(REPEAT_DELAY_AT), next = psp_read32(REPEAT_NEXT_AT);
    psp_write32(obj + 4, 60);                     /* 0x08D63128: 60 / 60 = one step per frame */
    psp_write32(REPEAT_DELAY_AT, delay * k);
    psp_write32(REPEAT_NEXT_AT, next * k);
    original();
    psp_write32(obj + 4, fps);
    psp_write32(REPEAT_DELAY_AT, delay);
    psp_write32(REPEAT_NEXT_AT, next);
}

/* After a frame at 60 / 120 fps: the ticks the next frame sees, as at 30 fps
 * (see the top): 2 once two ticks of frame time have passed, else 0. */
static void pace_ticks(void) {
    if (g_applied <= 30 || (g_applied <= 60 && !g_test_ticks)) {   /* exact there: the game's own value */
        if (g_ticks_ours && g_applied > 0) {          /* switched off: put back what the timing fill gives */
            psp_write32(TIMING_TICKS, (uint32_t)(60.0 / (double)g_applied + 0.5));
            g_ticks_ours = 0;
        }
        g_tick_acc = 0;
        g_ticks_now = 2;
        return;
    }
    g_ticks_ours = 1;
    g_tick_acc += 60.0 / (double)g_applied;
    uint32_t ticks = 0;
    if (g_tick_acc >= 2.0 - 1e-9) { ticks = 2; g_tick_acc -= 2.0; }
    psp_write32(TIMING_TICKS, ticks);
    g_ticks_now = ticks;
    if (!g_tick_log) {
        g_tick_log = 1;
        fprintf(stderr, "framerate: the game's 1/60 s tick counters paced as at 30 fps (2 ticks every %d frames)\n",
                g_applied / 30);
    }
}

int framerate_normalize(int fps) { return fps >= 120 ? 120 : fps == 60 ? 60 : 30; }

/* The emulated display's refresh for the target: 120 Hz for 120 fps. */
static int rate_for(int target) { return target == 120 ? 2 : 1; }

static void hook_set_frame_rate(void (*original)(void)) {
    const uint32_t asked = psp_cpu.r[4];                /* $a0: fps */
    g_asked = (int)asked;
    if (g_target >= 60 && (asked == 30 || asked == 20 || asked == 60)) psp_cpu.r[4] = (uint32_t)g_target;
    else if (rate_for(g_target) == 2 && asked > 0 && asked < 30) psp_cpu.r[4] = asked * 2;   /* 15 at 120 Hz */
    g_base = g_applied = (int)psp_cpu.r[4];
    original();
}

/* The next frame's timing for a frame that took `n` vblanks (of the emulated
 * display: 1/120 s each at 120 fps): the rate that matches, among those the
 * game's own timing fill handles (60/fps a whole number of vblanks). */
static void adapt(uint32_t n) {
    if (g_base <= 0) return;
    const int hz = 60 * psp_sched_vblank_rate();
    if (n < 1) n = 1;
    if (n > (uint32_t)(hz / 15)) n = (uint32_t)(hz / 15);
    static const int RATES[] = { 120, 60, 30, 20, 15 };
    int eff = 15;
    for (size_t k = 0; k < sizeof RATES / sizeof RATES[0]; k++)
        if (RATES[k] * (int)n <= hz) { eff = RATES[k]; break; }
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
        psp_sched_set_vblank_rate(rate_for(g_target));
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
    pace_ticks();
}

void framerate_init(int fps) {
    g_target = framerate_normalize(fps);
    psp_sched_set_vblank_rate(rate_for(g_target));
    /* Both hooks always: at a 30 fps target they pass everything through
     * unchanged, and they are what lets the target change at run time. */
    psp_hook_set(FN_SET_FRAME_RATE, hook_set_frame_rate);
    psp_hook_set(FN_FRAME, hook_frame);
    for (size_t i = 0; i < sizeof LOGIC_30HZ / sizeof LOGIC_30HZ[0]; i++) psp_hook_set(LOGIC_30HZ[i], hook_30hz);
    psp_hook_set(FN_INPUT_REPEAT, hook_input_repeat);
    psp_hook_set(FN_PALETTE_UPDATE, hook_palette_update);
    if (g_target >= 60)
        fprintf(stderr, "framerate: %d fps (the game's 30/20 fps requests run at %d through its own timing%s)\n",
                g_target, g_target, g_target == 120 ? "; the display refreshes at 120 Hz" : "");
}

void framerate_set(int fps) {
    fps = framerate_normalize(fps);
    if (fps == g_target) return;
    g_target = fps;
    g_pending = 1;
}

int framerate_target(void) { return g_target; }

int framerate_game_fps(void) { return g_asked ? (int)psp_read32(TIMING_FPS_INT) : 0; }
