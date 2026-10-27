/* The game's log files. See logfile.h. */

#include "logfile.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/stat.h>
#endif

/* A log opened for appending; one over LOG_ROTATE_BYTES becomes <name>.old
 * first (replacing the previous .old), so the logs stop growing without end
 * (they reached 74 MB, written and rescanned all session). */
#define LOG_ROTATE_BYTES (16L * 1024 * 1024)
FILE *log_open_rotated(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) {
        long size = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : 0;
        fclose(f);
        if (size > LOG_ROTATE_BYTES) {
            char old[1024];
            snprintf(old, sizeof old, "%s.old", path);
            remove(old);
            rename(path, old);
        }
    }
    return fopen(path, "a");
}

/* <base>/logs/<name>: every log goes in the logs folder next to the exe (on
 * Android, in the data folder), made on first use. */
void log_path(char *out, size_t cap, const char *base, const char *name) {
    snprintf(out, cap, "%s/logs", base);
#ifdef _WIN32
    CreateDirectoryA(out, NULL);
#else
    mkdir(out, 0777);
#endif
    snprintf(out, cap, "%s/logs/%s", base, name);
}
