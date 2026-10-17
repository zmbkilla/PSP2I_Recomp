/* The game's debug printf, to game_log.txt. See gamelog.c. */
#ifndef PSP2I_GAMELOG_H
#define PSP2I_GAMELOG_H

void gamelog_init(const char *exe_dir);
/* A line of our own in game_log.txt (and online_log.txt). */
void gamelog_note(const char *fmt, ...);

#endif
