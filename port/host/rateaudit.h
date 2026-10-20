/* Frame-rate audit: finds game logic that counts frames or vblanks instead of
 * time, i.e. what runs too fast (or too slow) at 60 / 120 fps. See
 * rateaudit.c. */
#ifndef PSP2I_RATEAUDIT_H
#define PSP2I_RATEAUDIT_H

/* At start: the vblank-use census (every game function that waits on
 * vblanks or reads the vblank counter, logged once with its stack). */
void rateaudit_init(const char *exe_dir);

/* Once per vblank: the periodic summary and the audit's steps. */
void rateaudit_vblank(void);

/* F7: measure 3 s at the current target (or 120 when at 30), 3 s at 30, then
 * write logs/rate_audit_N.txt. Ignored while one is running. */
void rateaudit_start(void);

/* F6: the press audit, for motion that follows one button press (a menu
 * slide, a selection, a door): the on-screen line asks for one press at the
 * current target (120 if it is 30), one at 30, and one more for the write
 * trace; logs/press_audit_N.txt. Ignored while an audit runs. */
void rateaudit_start_press(void);

/* While an audit runs: a line for the screen ("" when idle). */
const char *rateaudit_status(void);

#endif /* PSP2I_RATEAUDIT_H */
