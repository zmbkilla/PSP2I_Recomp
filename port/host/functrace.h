/* Function-activity recorder (F9): which game functions run during an
 * action. See functrace.c. */
#ifndef PSP2I_FUNCTRACE_H
#define PSP2I_FUNCTRACE_H

void functrace_init(const char *exe_dir);
void functrace_toggle(void);
int  functrace_recording(void);

#endif /* PSP2I_FUNCTRACE_H */
