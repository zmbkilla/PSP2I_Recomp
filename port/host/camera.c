/* Right-stick camera control, reconstructed natively from the PPSSPP cheat in
 * rightstick.txt (not applied as a memory patch).
 *
 * What the cheat does (addresses are 0x08800000 + the cheat offset):
 *
 *   - 0x08D64074 (the pad update 0x08D64068) calls a routine in a code cave
 *     (0x08803940) that reads the right stick from the two spare bytes of
 *     SceCtrlData (sp+0x1A/0x1B, which PPSSPP fills), and for each axis:
 *     v = byte - 128; v = 0 if -8 <= v <= 7; camX/camY = v / 128 * speed
 *     (speed = the floats at 0x08803908/0C: 1.0, 1.25 or 1.5), stored at
 *     0x08803900/04.
 *   - 0x08A167DC / 0x08A16800, in 0x08A16794: that function computes the
 *     camera's yaw and pitch input (0x08A1681C, then a signed-square curve)
 *     and stores them to *a1 and *a2. The cheat adds: if the stored yaw is
 *     0 and camX != 0, store -camX; if the stored pitch is 0 and camY != 0,
 *     store camY. The game's own camera input keeps priority.
 *   - 0x08AF8C48: the call to 0x08AFA530 (a "camera is being controlled"
 *     test, from 0x08AF8BD0) also answers yes while the right stick is
 *     deflected: v0 = original || camX != 0 || camY != 0.
 *
 * Here the stick comes from SDL (input_sdl_right_stick), converted to the
 * same PSP-style byte, and the two game functions are hooked at entry
 * (port/hooks.txt): 0x08A16794 runs the original and then applies the
 * cheat's rule to its outputs; 0x08AFA530 runs the original and ORs in the
 * stick only for the call the cheat patched (return address 0x08AF8C50) --
 * its other four callers are left alone, as with the cheat. */

#include "camera.h"

#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/mem.h>

#include <stdio.h>

#define FN_CAMERA_INPUT  0x08A16794u   /* (?, float *yaw, float *pitch) */
#define FN_CAMERA_ACTIVE 0x08AFA530u
#define RA_CAMERA_ACTIVE 0x08AF8C50u   /* return address of the call at 0x08AF8C48 */

static float   g_speed = CAMERA_SPEED_MIN;
static uint64_t n_input_calls, n_yaw_set, n_pitch_set, n_active_calls, n_active_set;
static uint8_t g_rx = 128, g_ry = 128;

float camera_axis(uint8_t byte, float speed) {
    int v = (int)byte - 128;
    if (v >= -8 && v < 8) v = 0;
    return (float)v / 128.0f * speed;
}

float camera_x(void) { return camera_axis(g_rx, g_speed); }
float camera_y(void) { return camera_axis(g_ry, g_speed); }

void camera_set_stick(uint8_t rx, uint8_t ry) { g_rx = rx; g_ry = ry; }

void camera_set_speed(float speed) {
    if (speed < CAMERA_SPEED_MIN) speed = CAMERA_SPEED_MIN;
    if (speed > CAMERA_SPEED_MAX) speed = CAMERA_SPEED_MAX;
    g_speed = speed;
}
float camera_speed(void) { return g_speed; }

static void hook_camera_input(void (*original)(void)) {
    const uint32_t yaw = psp_cpu.r[5], pitch = psp_cpu.r[6];   /* $a1, $a2 */
    original();
    n_input_calls++;
    const float cx = camera_x(), cy = camera_y();
    if (cx != 0.0f && psp_read_f32(yaw) == 0.0f) { psp_write_f32(yaw, -cx); n_yaw_set++; }
    if (cy != 0.0f && psp_read_f32(pitch) == 0.0f) { psp_write_f32(pitch, cy); n_pitch_set++; }
}

static void hook_camera_active(void (*original)(void)) {
    const uint32_t ra = psp_cpu.r[31];
    original();
    if (ra == RA_CAMERA_ACTIVE) n_active_calls++;
    if (ra == RA_CAMERA_ACTIVE && psp_cpu.r[2] == 0 && (camera_x() != 0.0f || camera_y() != 0.0f)) {
        psp_cpu.r[2] = 1;
        n_active_set++;
    }
}

void camera_init(float speed) {
    camera_set_speed(speed);
    psp_hook_set(FN_CAMERA_INPUT, hook_camera_input);
    psp_hook_set(FN_CAMERA_ACTIVE, hook_camera_active);
}

void camera_report(FILE *out) {
    fprintf(out, "  right-stick camera  input calls %llu (yaw set %llu, pitch set %llu); camera-active calls %llu (forced %llu); %.2fx\n",
            (unsigned long long)n_input_calls, (unsigned long long)n_yaw_set, (unsigned long long)n_pitch_set,
            (unsigned long long)n_active_calls, (unsigned long long)n_active_set, (double)g_speed);
}
