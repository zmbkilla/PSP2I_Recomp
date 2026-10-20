/* PSP2i frame rate (30 = the game's own, 60 and 120 = through its frame-rate
 * API; 120 also doubles the emulated display's refresh). See framerate.c. */
#ifndef PSP2I_FRAMERATE_H
#define PSP2I_FRAMERATE_H

void framerate_init(int fps);

/* Change the target (30, 60 or 120) at run time. Takes effect at the game's
 * next frame, through its own setFrameRate. */
void framerate_set(int fps);
int  framerate_target(void);

/* The frame rate in the game's timing state (what it is actually pacing and
 * stepping for), or 0 before the game first sets one. */
int  framerate_game_fps(void);

/* 30, 60 or 120: the nearest supported target for an fps value (anything
 * else is 30). */
int  framerate_normalize(int fps);

/* The engine menu's TESTS page (both off by default; see framerate.c):
 * ticks = the game's 1/60 s tick field paced as at 30 at 60 too (above 60
 * it always is),
 * logic = frame-counted functions (LOGIC_30HZ) run on 30 Hz frames only. */
void framerate_set_tests(int ticks, int logic);

#endif /* PSP2I_FRAMERATE_H */
