/* The game's own debug messages.
 *
 * PSP2i keeps its debug printf calls (0x0886EA30) in the release build, but
 * the function is a stub that discards them. Hooked at entry (port/hooks.txt),
 * the message is formatted here from the game's arguments and written to
 * game_log.txt next to the exe -- the game narrating what it is doing, e.g.
 * "TPspNpMatching2::getWorldInfoListCb(): ...". Lines about the online
 * client (matching, NP, the infra/SEGA client, rooms) also go to
 * online_log.txt. A line repeated back to back is written once with a count.
 *
 * Arguments follow the PSP EABI: a1..a3, t0..t3, then the caller's stack; a
 * 64-bit value takes an even/odd register pair (or an 8-byte-aligned stack
 * slot). */

#include "gamelog.h"
#include "online.h"

#include <psprecomp/dispatch.h>
#include <psprecomp/hle.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define FN_GAME_PRINTF 0x0886EA30u

static FILE *g_file;
static char g_dir[600];
static char g_last[1024];
static unsigned g_repeat;

typedef struct { int slot; } va;          /* next argument slot: 0 = $a1 */

static uint32_t arg32(va *v) {
    const int s = v->slot++;
    if (s < 7) return psp_cpu.r[5 + s];   /* $a1..$a3 = r5..r7, $t0..$t3 = r8..r11 */
    return psp_read32(psp_cpu.r[29] + (uint32_t)(s - 7) * 4u);
}

static uint64_t arg64(va *v) {
    if (v->slot < 7) { if ((5 + v->slot) & 1) v->slot++; }   /* even register first */
    else if ((v->slot - 7) & 1) v->slot++;                    /* 8-byte stack slot */
    const uint64_t lo = arg32(v), hi = arg32(v);
    return lo | hi << 32;
}

/* printf with the guest's format and arguments. */
static void format(char *out, size_t cap, uint32_t fmt_addr) {
    char fmt[512];
    psp_str(fmt_addr, fmt, sizeof fmt);
    va v = { 0 };
    size_t n = 0;
    for (const char *p = fmt; *p && n + 1 < cap;) {
        if (*p != '%') { out[n++] = *p++; continue; }
        char spec[32];
        size_t k = 0;
        spec[k++] = *p++;
        while (*p && strchr("-+ #0", *p) && k < 20) spec[k++] = *p++;
        if (*p == '*') { p++; k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", (int)arg32(&v)); }
        while (*p && isdigit((unsigned char)*p) && k < 24) spec[k++] = *p++;
        if (*p == '.') {
            spec[k++] = *p++;
            if (*p == '*') { p++; k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", (int)arg32(&v)); }
            while (*p && isdigit((unsigned char)*p) && k < 28) spec[k++] = *p++;
        }
        int longs = 0;
        while (*p == 'l' || *p == 'h' || *p == 'z' || *p == 'j' || *p == 't') { if (*p == 'l') longs++; p++; }
        const char c = *p ? *p++ : '\0';
        char piece[600];
        piece[0] = '\0';
        switch (c) {
        case '%': snprintf(piece, sizeof piece, "%%"); break;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            if (longs >= 2) {
                spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = c; spec[k] = '\0';
                const uint64_t x = arg64(&v);
                if (c == 'd' || c == 'i') snprintf(piece, sizeof piece, spec, (long long)x);
                else snprintf(piece, sizeof piece, spec, (unsigned long long)x);
            } else {
                spec[k++] = c; spec[k] = '\0';
                const uint32_t x = arg32(&v);
                if (c == 'd' || c == 'i') snprintf(piece, sizeof piece, spec, (int)x);
                else snprintf(piece, sizeof piece, spec, (unsigned)x);
            }
            break;
        }
        case 'c': spec[k++] = 'c'; spec[k] = '\0'; snprintf(piece, sizeof piece, spec, (int)(arg32(&v) & 0xFF)); break;
        case 'p': snprintf(piece, sizeof piece, "0x%08X", arg32(&v)); break;
        case 's': {
            const uint32_t a = arg32(&v);
            char s[512];
            if (a) psp_str(a, s, sizeof s); else snprintf(s, sizeof s, "(null)");
            spec[k++] = 's'; spec[k] = '\0';
            snprintf(piece, sizeof piece, spec, s);
            break;
        }
        case 'f': case 'g': case 'e': {               /* doubles: an even register pair */
            uint64_t bits = arg64(&v);
            double d;
            memcpy(&d, &bits, 8);
            spec[k++] = c; spec[k] = '\0';
            snprintf(piece, sizeof piece, spec, d);
            break;
        }
        default: snprintf(piece, sizeof piece, "%%%c", c); break;
        }
        for (const char *q = piece; *q && n + 1 < cap;) out[n++] = *q++;
    }
    out[n] = '\0';
}

static int is_online_line(const char *s) {
    static const char *const KEYS[] = { "Matching", "matching", "Np", "Infra", "infra", "Room", "room", "Lobby", "lobby",
                                        "Signaling", "signaling", "World", "world", "Server", "server", "Login", "login" };
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) if (strstr(s, KEYS[i])) return 1;
    return 0;
}

static void emit(const char *line, unsigned repeat) {
    if (!g_file) {
        char p[700];
        snprintf(p, sizeof p, "%s/game_log.txt", g_dir);
        g_file = fopen(p, "a");
        if (g_file) { time_t t = time(NULL); fprintf(g_file, "\n==== session %s", ctime(&t)); }
    }
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (g_file) {
        if (repeat > 1) fprintf(g_file, "[%02d:%02d:%02d] (previous line x%u)\n", tm->tm_hour, tm->tm_min, tm->tm_sec, repeat);
        else fprintf(g_file, "[%02d:%02d:%02d] %s\n", tm->tm_hour, tm->tm_min, tm->tm_sec, line);
        fflush(g_file);
    }
    if (repeat <= 1 && is_online_line(line)) online_log("game: %s", line);
}

static void hook_printf(void (*original)(void)) {
    char line[1024];
    format(line, sizeof line, psp_cpu.r[4]);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    if (n) {
        if (!strcmp(line, g_last)) g_repeat++;
        else {
            if (g_repeat > 1) emit(g_last, g_repeat);
            g_repeat = 1;
            snprintf(g_last, sizeof g_last, "%s", line);
            emit(line, 1);
        }
    }
    original();                                   /* the stub: returns */
}

void gamelog_init(const char *exe_dir) {
    snprintf(g_dir, sizeof g_dir, "%s", exe_dir);
    psp_hook_set(FN_GAME_PRINTF, hook_printf);
}
