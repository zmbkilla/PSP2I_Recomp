/* Right-stick camera, reconstructed from the PPSSPP cheat "Right Analog Camera
 * Support" (rightstick.txt at the project root). See camera.c. */
#ifndef PSP2I_CAMERA_H
#define PSP2I_CAMERA_H

#include <stdint.h>

/* Sensitivity: the cheat's speed float (1.0, 1.25, 1.5 in its variants),
 * here 1.00x..2.00x in 0.25 steps, chosen in the settings menu. */
#define CAMERA_SPEED_MIN   1.0f
#define CAMERA_SPEED_MAX   2.0f
#define CAMERA_SPEED_STEP  0.25f

/* One axis as the cheat computes it from a PSP-style stick byte (0..255,
 * 128 = centre): v = byte - 128, zero for -8 <= v <= 7 (its dead zone),
 * else v / 128 * speed. */
float camera_axis(uint8_t byte, float speed);

/* Install the two hooks (0x08A16794, 0x08AFA530). */
void  camera_init(float speed);
void  camera_set_speed(float speed);
float camera_speed(void);

/* The right stick, once per vblank (PSP-style bytes). */
void  camera_set_stick(uint8_t rx, uint8_t ry);

/* What the hooks apply, for tests and diagnostics. */
float camera_x(void);
float camera_y(void);

#include <stdio.h>
void  camera_report(FILE *out);

#endif /* PSP2I_CAMERA_H */
