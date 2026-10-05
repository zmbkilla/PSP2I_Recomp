/* Community ("prod") build start-up check. See prod.c. */
#ifndef PSP2I_PROD_H
#define PSP2I_PROD_H

#include <stddef.h>

/* Find the game data from the exe's folder `dir`: fills root (GameData) and
 * eboot. Returns 0 when everything is there; otherwise shows one error
 * listing what is missing and returns nonzero (the caller exits). */
int prod_check(const char *dir, char *root, size_t root_cap, char *eboot, size_t eboot_cap);

#endif
