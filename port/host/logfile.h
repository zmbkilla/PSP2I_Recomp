/* The game's log files: the logs folder next to the exe, and logs that stop
 * growing without end. */
#ifndef PSP2I_LOGFILE_H
#define PSP2I_LOGFILE_H

#include <stddef.h>
#include <stdio.h>

/* A log opened for appending, rotated to <path>.old once it is over 16 MB. */
FILE *log_open_rotated(const char *path);
/* out = <base>/logs/<name>, the logs folder made if missing. */
void log_path(char *out, size_t cap, const char *base, const char *name);

#endif /* PSP2I_LOGFILE_H */
