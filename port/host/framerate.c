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
 * which stay per frame. (Steps of 1 as at 60 -- 1, 0 at 120 -- were tried:
 * smoother rolls, but the roll's animation then ran ahead of its movement
 * and the lobby lost fps, so the 30 fps steps stay.)
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
 * not speed. Frames over 1/15 s (loading) are counted as 1/15 s. At 120 Hz
 * some frame lengths have no rate of their own; see adapt().
 *
 * ## 120 fps: frames over 8.3 ms
 *
 * At 120 Hz the vblanks are 8.3 ms apart, and the lobby's frames often take
 * a little longer (~10 ms: under 16.7 ms nearly always, with a third of the
 * time spent waiting). Each such frame misses its vblank and waits for the
 * next: 16.7 ms, 60 fps. Letting a late frame skip that wait
 * (psp_display_skip_next_wait) was tried and made it worse -- a tenth of the
 * lobby's frames then took 25-33 ms of work, down to 30 fps: the game's
 * threads hand work over at the vblank. The late frames are counted (the
 * 30 s [rate] line); each frame's step comes from the host time since the
 * previous frame ended. */

#include "framerate.h"

#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/hle.h>
#include <psprecomp/mem.h>

#include <stdio.h>
#include <stdlib.h>

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
static double g_debt;            /* seconds of real frame time not yet given to the game as steps */
static uint64_t g_frame_end_us;  /* host time the previous frame ended */
static uint64_t g_late_frames;   /* frames at 120 Hz that missed their vblank and did not wait */
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

/* ## Integer 60 / fps above 60 (port/testwork/fps_notes.md)
 *
 * Two places compute 60 / fps as an integer, exact at 30 (2) and 60 (1) and 0
 * at 120: an enemy hazard's per-frame update counts obj+200 down by it, so
 * at 120 its timer never ran out; and a stepped object's constructor sets
 * its steps per frame (obj+72) to 2 * (60 / fps), whose update steps until
 * the end in one frame when that is 0. At 120 the hazard sees 60 fps on every
 * other frame (1, 0, 1, 0: 60 a second), and the stepped object is built
 * with 60's numbers and updated on every other frame. */
#define FN_HAZARD_UPDATE 0x08A94C30u   /* vtable 0x08E3A5D8 slot 5: obj+200 -= 60 / fps */
#define FN_STEPPED_NEW   0x08933CECu   /* obj+72 = *0x08E94690 * (60 / fps) */
#define FN_STEPPED_UPD   0x08933F18u   /* runs obj+72 steps; to the end when obj+72 <= 0 */
static uint32_t g_frame_no;            /* frames ended (hook_frame) */

/* Run `original` with fps_int 60 in the timing state (restored after). */
static void as_60(void (*original)(void)) {
    const uint32_t fps = psp_read32(TIMING_FPS_INT);
    psp_write32(TIMING_FPS_INT, 60);
    original();
    psp_write32(TIMING_FPS_INT, fps);
}

static void hook_hazard_update(void (*original)(void)) {
    if (psp_read32(TIMING_FPS_INT) > 60 && (g_frame_no & 1)) as_60(original);
    else original();
}

static void hook_stepped_new(void (*original)(void)) {
    if (psp_read32(TIMING_FPS_INT) > 60) as_60(original);
    else original();
}

static void hook_stepped_update(void (*original)(void)) {
    if (psp_read32(TIMING_FPS_INT) > 60 && !(g_frame_no & 1)) return;   /* as at 60 */
    original();
}

/* ## The effects' clock
 *
 * Effects advance their time (effect+144) by the effect manager's step, +4,
 * in 1/300 s: 300 / fps as an integer, 10 at 30 and 5 at 60, but 2 at 120
 * (2.5 is right): every effect animated 20% slow. After each frame the step
 * for the next is set from the real frame length (2, 3, 2, 3 at 120), unless
 * the game set a step of its own (0x08AE5D48, e.g. a slow motion). */
#define EFFECT_GLOBAL 0x08ED92FCu      /* -> { [0] the effect manager, [4] loaded effects } */
static double g_eff_acc;
static uint32_t g_eff_ours;            /* the step written last (0: none) */

static void effect_step(void) {
    const uint32_t g = psp_read32(EFFECT_GLOBAL);
    const uint32_t mgr = g ? psp_read32(g) : 0;
    if (!mgr || g_applied <= 60) { g_eff_ours = 0; g_eff_acc = 0; return; }   /* exact there */
    const uint32_t cur = psp_read32(mgr + 4);
    const uint32_t game = 300u / (uint32_t)g_applied ? 300u / (uint32_t)g_applied : 1u;
    if (cur != game && cur != g_eff_ours) { g_eff_ours = 0; return; }        /* the game's own step */
    g_eff_acc += 300.0 / (double)g_applied;
    uint32_t step = (uint32_t)g_eff_acc;
    g_eff_acc -= step;
    if (!step) step = 1;
    psp_write32(mgr + 4, step);
    g_eff_ours = step;
}

/* ## File requests served when the game checks for them
 *
 * The game's file thread (fio_th, priority 36) serves one request at a time
 * from a queue the main thread (32) fills; the main thread asks whether its
 * request is done (0x08B8C3B4) once per step of its loading logic -- once a
 * frame. Being lower in priority, the file thread only runs while the main
 * thread waits (its vblank wait), and it sleeps 8 ms when its queue is
 * empty, so a request was often still queued at the first check: the step
 * came a frame later (requests 16.7 / 66.7 ms apart; loading a save with
 * imported DLC opens its ~150 DLC files two to four times each).
 *
 * So at that check -- after letting the file thread finish a request it
 * was interrupted in (a nap of at most 5 ms) -- the queued
 * requests are served there and then, in order, by the file thread's own
 * code (take, serve, store the result, complete: its loop at 0x08B8D8E8),
 * and the check finds them done. The file work is the same; frames and the
 * game's steps are untouched -- only the wait between them is gone. */
#define FN_FIO_REQUEST  0x08B8D6B0u    /* serve one request (a0): the result */
#define FN_FIO_TAKE     0x08B8BF74u    /* (queue, &request): -1 when empty */
#define FN_FIO_DONE_A   0x08B8C0C8u    /* (queue, request): complete (flag 0x80 or result -8) */
#define FN_FIO_DONE_B   0x08B8C064u    /* (queue, request): complete */
#define FN_FIO_CHECK    0x08B8C3B4u    /* (request id): its state, from the main thread */
#define FIO_QUEUE       0x08EDE9D0u
#define FIO_RING 16
static uint64_t g_fio_at[FIO_RING];    /* host time of the last requests (ring) */
static unsigned g_fio_n;
static int g_fio_holding;              /* the file thread has taken a request and not completed it */
static uint32_t g_fio_req;             /* ... that request */
static int g_serving;                  /* the port is serving requests at a check */
static int g_dlc_spin;                 /* a DLC file's stages run back to back (hook_load_wait) */
static uint64_t g_served;
static void (*g_log)(const char *fmt, ...);   /* the game log (framerate_set_log) */
void framerate_set_log(void (*fn)(const char *fmt, ...)) { g_log = fn; }

static void hook_fio_request(void (*original)(void)) {
    const uint64_t t0 = psp_sched_now_us();
    const uint64_t prev = g_fio_n ? g_fio_at[(g_fio_n - 1) % FIO_RING] : t0;
    const uint32_t req = psp_cpu.r[4];
    const uint32_t type = req ? psp_read16(req + 20) & 0x7F : 0;
    g_fio_at[g_fio_n++ % FIO_RING] = t0;
    original();
    /* the first 400 requests: what was served, when, and by whom (load pacing) */
    static int file_log = -1;
    if (file_log < 0) { const char *e = getenv("PSP2I_FILE_LOG"); file_log = e && *e == '1'; }
    if (g_log && file_log && g_fio_n <= 400)
        g_log("[fio] request %u type %u: %.1f ms after the previous, served in %.2f ms by the %s (vblank %llu)", g_fio_n,
              type, (double)(t0 - prev) / 1000.0, (double)(psp_sched_now_us() - t0) / 1000.0,
              g_serving ? "check" : "file thread", (unsigned long long)psp_sched_vblank_count());
}

static void hook_fio_take(void (*original)(void)) {
    /* While a DLC file's stages run back to back, the file thread finds the
     * queue empty: the checks serve those requests at once, instead of
     * napping until the file thread -- which took one while the main thread
     * napped -- finishes it (1-2 ms a nap on Windows; ~27 ms a DLC file). */
    if (g_dlc_spin && !g_serving) {
        if (psp_cpu.r[5]) psp_write32(psp_cpu.r[5], 0);
        psp_cpu.r[2] = 0xFFFFFFFFu;
        return;
    }
    const uint32_t out = psp_cpu.r[5];
    original();
    if (!g_serving && (int32_t)psp_cpu.r[2] != -1 && out) { g_fio_holding = 1; g_fio_req = psp_read32(out); }
}

/* (queue, request): the file thread's request done -- by whoever completes
 * it (keyed by the request: clearing only on the file thread's own
 * completions left the flag set whenever the check completed it, and every
 * later check then napped its full 5 ms). */
static void hook_fio_done(void (*original)(void)) {
    const uint32_t req = psp_cpu.r[5];
    original();
    if (g_fio_holding && req == g_fio_req) g_fio_holding = 0;
}

/* One guest call from a hook, the CPU state kept. */
static uint32_t guest_call(uint32_t fn, uint32_t a0, uint32_t a1) {
    psp_fn_t f = psp_lookup(fn);
    if (!f) return 0xFFFFFFFFu;
    psp_cpu.r[4] = a0;
    psp_cpu.r[5] = a1;
    psp_cpu.r[31] = 0;
    f();
    return psp_cpu.r[2];
}

static void hook_fio_check(void (*original)(void)) {
    /* The file thread may hold a request it started and was interrupted in
     * (the main thread outranks it): let it finish first -- under 1 ms of
     * work -- by napping here, at most 5 ms, inside the frame. Skipping the
     * serving then left the request half done for frames (100 ms gaps). */
    for (int k = 0; k < 10 && g_fio_holding && !g_serving; k++) {
        psp_sched_sleep_until(psp_sched_now_us() + 500);
    }
    if (!g_fio_holding && !g_serving) {
        const psp_cpu_state saved = psp_cpu;
        g_serving = 1;
        const uint32_t out = psp_cpu.r[29] - 64;          /* below the caller's stack: the taken request */
        for (int k = 0; k < 64; k++) {                    /* the queue as it stands, in order */
            psp_cpu = saved;
            if ((int32_t)guest_call(FN_FIO_TAKE, FIO_QUEUE, out) == -1) break;
            const uint32_t req = psp_read32(out);
            if (!req) break;
            psp_cpu = saved;
            const uint32_t res = guest_call(FN_FIO_REQUEST, req, 0);
            psp_write32(req + 24, res);
            psp_cpu = saved;
            if ((psp_read16(req + 20) & 0x80) || (int32_t)res == -8) guest_call(FN_FIO_DONE_A, FIO_QUEUE, req);
            else guest_call(FN_FIO_DONE_B, FIO_QUEUE, req);
            g_served++;
        }
        g_serving = 0;
        psp_cpu = saved;
    }
    original();
}

unsigned long long framerate_served_at_check(void) { return g_served; }

/* The DLC loaders -- 0x08A0A8F4.. (the PDR*.EDAT files of ms0:/PSP/GAME/
 * NPJH50043) and the DLC item code at 0x08CBDxxx (its ITM*.EDAT files), both
 * run when a save with imported DLC loads -- wait for each file in the game's
 * blocking load (0x08D56B5C(file, 0)): step the file object (its method 0:
 * one stage -- open, check, read -- per call), and if that is not done, run
 * the loading-screen frame (0x08D5601C) and the resource library's update
 * (0x08DA1264), then step again. One stage a frame: 4-5 frames a file
 * although every read is served at once now (above). For that loader only,
 * the stages run back to back first -- step, library update, step ... up to
 * 16 -- before the game's loop, which then finds the file done; the frames
 * still come at the game's rate. */
#define FN_LOAD_WAIT    0x08D56B5Cu    /* (file object, nonblocking): the game's blocking load */
#define FN_LIB_UPDATE   0x08DA1264u    /* the resource library's per-poll update */
static uint64_t g_dlc_files_quick;
/* Where the DLC loaders call the blocking load (return addresses: call + 8);
 * the game's other ~100 loads keep their pace. */
static const uint32_t DLC_LOAD_RA[] = { 0x08A0AB10u, 0x08A0AC14u, 0x08A0B1A8u, 0x08CBDA0Cu };

static int dlc_load_site(uint32_t ra) {
    for (size_t k = 0; k < sizeof DLC_LOAD_RA / sizeof DLC_LOAD_RA[0]; k++) if (ra == DLC_LOAD_RA[k]) return 1;
    return 0;
}

static void hook_load_wait(void (*original)(void)) {
    const uint32_t obj = psp_cpu.r[4], ra = psp_cpu.r[31];
    const int dlc = obj && psp_cpu.r[5] == 0 && dlc_load_site(ra);
    if (dlc) {
        const psp_cpu_state saved = psp_cpu;
        g_dlc_spin = 1;
        for (int k = 0; k < 16; k++) {
            psp_cpu = saved;
            const uint32_t vt = psp_read32(obj), step = vt ? psp_read32(vt) : 0;
            if (!step || guest_call(step, obj, 0) != 0) { if (step) g_dlc_files_quick++; break; }
            psp_cpu = saved;
            guest_call(FN_LIB_UPDATE, 0, 0);
        }
        g_dlc_spin = 0;
        psp_cpu = saved;
    }
    original();
}

unsigned long long framerate_dlc_files_quick(void) { return g_dlc_files_quick; }

/* The PDR loop (0x08A0AA08..) draws a whole loading-screen frame -- with its
 * vblank wait -- after every file (0x0896E8F8, called from 0x08A0AC9C), so it
 * reads one file a frame: ~400 frames for its two passes at 30 fps, a quarter
 * of that time at 120. Here that frame is drawn only once the frame period
 * has nearly passed since the last one; the files in between load back to
 * back. The loading screen still shows at the game's rate and every frame is
 * the game's own -- only the files per frame change. */
#define FN_LOAD_FRAME   0x0896E8F8u    /* the loading screen's frame (draw, wait, flip) */
#define PDR_FRAME_RA    0x08A0ACA4u    /* its call in the PDR loop (return address) */
static uint64_t g_load_frame_end;
static void hook_load_frame(void (*original)(void)) {
    if (psp_cpu.r[31] == PDR_FRAME_RA) {
        const uint64_t now = psp_sched_now_us();
        const uint64_t budget = 1000000u / (uint64_t)(g_target > 0 ? g_target : 30);
        /* the frame starts with 6 ms to spare, so its wait ends at the vblank it would have */
        if (g_load_frame_end && now - g_load_frame_end + 6000u < budget) { psp_cpu.r[2] = 0; return; }
        original();
        g_load_frame_end = psp_sched_now_us();
        return;
    }
    original();
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

static int g_reissuing;  /* hook_frame re-issues the game's request itself (a new target) */

static void hook_set_frame_rate(void (*original)(void)) {
    const uint32_t asked = psp_cpu.r[4];                /* $a0: fps */
    /* The same request again: 0x08B34B18 / 0x08B34B60 call this whenever the
     * current fps is not 30 / 20, which at 60 / 120 is always; going through
     * would reset the frame step and the time carry-over (adapt) each time. */
    if (!g_reissuing && g_base > 0 && (int)asked == g_asked) return;
    g_asked = (int)asked;
    if (g_target >= 60 && (asked == 30 || asked == 20 || asked == 60)) psp_cpu.r[4] = (uint32_t)g_target;
    else if (rate_for(g_target) == 2 && asked > 0 && asked < 30) psp_cpu.r[4] = asked * 2;   /* 15 at 120 Hz */
    g_base = g_applied = (int)psp_cpu.r[4];
    g_debt = 0;
    original();
}

/* The next frame's timing for a frame that took `secs`, among the rates the
 * game's own timing fill handles (60/fps a whole number of vblanks). Most
 * frame lengths have none: 25 ms (3 vblanks at 120 Hz) once was stepped as
 * the next lower rate, 30 fps (33 ms), and timers and countdowns ran up to a
 * third quicker whenever 120 was missed. So the real time not yet stepped is
 * carried over (g_debt) and each frame takes the rate whose step is nearest
 * it: 25 ms frames alternate 60 and 30, and the steps add up to the time
 * that passed. */
static void adapt(double secs) {
    if (g_base <= 0) return;
    static const int RATES[] = { 120, 60, 30, 20, 15 };
    g_debt += secs;
    if (g_debt > 1.0 / 15.0) g_debt = 1.0 / 15.0;    /* longer frames (loading) count as 1/15 s */
    int eff = 15;
    double best = 1e9;
    for (size_t k = 0; k < sizeof RATES / sizeof RATES[0]; k++) {
        if (RATES[k] > g_base) continue;
        const double d = g_debt > 1.0 / RATES[k] ? g_debt - 1.0 / RATES[k] : 1.0 / RATES[k] - g_debt;
        if (d < best - 1e-9) { best = d; eff = RATES[k]; }
    }
    g_debt -= 1.0 / eff;
    if (g_debt < -1.0 / 30.0) g_debt = -1.0 / 30.0;  /* short frames (fast loading) run ahead by at most a frame */
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
        fprintf(stderr, "framerate: a frame took %.1f ms; the next one steps as %d fps (the game runs at %d)%s\n",
                secs * 1000.0, eff, g_base, g_adapt_log == 20 ? " -- further changes not logged" : "");
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
            g_reissuing = 1;
            set();
            g_reissuing = 0;
            psp_cpu = saved;
            fprintf(stderr, "framerate: switched to a %d fps target (game timing now %d fps)\n",
                    g_target, framerate_game_fps());
        }
    }
    /* A frame at 120 Hz that missed its vblank: counted (see the top). */
    const uint32_t before = psp_read32(FRAME_END_VCOUNT);
    if (before && rate_for(g_target) == 2 && (uint32_t)psp_sched_vblank_count() != before) g_late_frames++;
    original();                               /* the frame's wait: it ends at a vblank */
    const uint64_t now = psp_sched_now_us();
    if (g_frame_end_us) adapt((double)(now - g_frame_end_us) / 1e6);
    g_frame_end_us = now;
    pace_ticks();
    g_frame_no++;
    effect_step();
}

unsigned long long framerate_late_frames(void) { return g_late_frames; }

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
    psp_hook_set(FN_HAZARD_UPDATE, hook_hazard_update);
    psp_hook_set(FN_STEPPED_NEW, hook_stepped_new);
    psp_hook_set(FN_STEPPED_UPD, hook_stepped_update);
    psp_hook_set(FN_FIO_REQUEST, hook_fio_request);
    psp_hook_set(FN_FIO_TAKE, hook_fio_take);
    psp_hook_set(FN_FIO_DONE_A, hook_fio_done);
    psp_hook_set(FN_FIO_DONE_B, hook_fio_done);
    psp_hook_set(FN_FIO_CHECK, hook_fio_check);
    psp_hook_set(FN_LOAD_WAIT, hook_load_wait);
    psp_hook_set(FN_LOAD_FRAME, hook_load_frame);
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
