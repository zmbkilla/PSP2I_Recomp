/* PSP2i frame rate (30 = the game's own, 60 = through its frame-rate API).
 * See framerate.c. */
#ifndef PSP2I_FRAMERATE_H
#define PSP2I_FRAMERATE_H

void framerate_init(int fps);

/* Change the target (30 or 60) at run time. Takes effect at the game's next
 * frame, through its own setFrameRate. */
void framerate_set(int fps);
int  framerate_target(void);

/* The frame rate in the game's timing state (what it is actually pacing and
 * stepping for), or 0 before the game first sets one. */
int  framerate_game_fps(void);

#endif /* PSP2I_FRAMERATE_H */
