/* Attenuation: notice other programs playing sound (duck_win.c). */
#ifndef PSP2I_DUCK_H
#define PSP2I_DUCK_H

void duck_start(void);                       /* begin watching (a background thread) */
void duck_stop(void);
int  duck_others_heard_within(unsigned ms);  /* 1 if another program made sound in the last `ms` */

#endif
