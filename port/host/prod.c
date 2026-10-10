/* Community ("prod") build: start-up check of everything the game needs.
 *
 * The prod build takes no command line. Everything is found from the folder
 * psp2i.exe is in ("the root"):
 *
 *   <root>\*.iso or <root>\GameData\*.iso            the game disc image, or else
 *   <root>\GameData\disc\PSP_GAME\PARAM.SFO, USRDIR\   the extracted disc
 *   <root>\EBOOT.BIN                                  a decrypted game EBOOT, or else the
 *                                                     disc's PSP_GAME\SYSDIR\EBOOT.BIN
 *   <root>\GameData\flash\font\*.pgf                  the PSP firmware fonts
 *   <root>\SDL3.dll                                   controllers, sound
 *   <root>\psp2i_atrac.dll                            the ATRAC music decoder
 *
 * The EBOOT must be a decrypted (plain ELF) NPJH50332 EBOOT: its code region
 * is checked against the code this build was recompiled from (the revival's
 * patched EBOOT differs only in data, so either is accepted). Anything missing
 * is listed in one message box, and the program exits.
 */
#include "prod.h"

#include <psprecomp/hle.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* The recompiled code's extent in the EBOOT, and its FNV-1a 64 hash. */
#define CODE_LO   0x08804040u
#define CODE_HI   0x08DFC170u
#define CODE_HASH 0xE94D5451B49E4AB4ull

static const char *const FONTS[] = {
    "jpn0.pgf", "kr0.pgf",
    "ltn0.pgf", "ltn1.pgf", "ltn2.pgf", "ltn3.pgf", "ltn4.pgf", "ltn5.pgf", "ltn6.pgf", "ltn7.pgf",
    "ltn8.pgf", "ltn9.pgf", "ltn10.pgf", "ltn11.pgf", "ltn12.pgf", "ltn13.pgf", "ltn14.pgf", "ltn15.pgf",
};

static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

#ifdef _WIN32
static int dir_exists(const char *p) {
    const DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static int glob_exists(const char *dir, const char *pattern) {
    char p[800];
    snprintf(p, sizeof p, "%s\\%s", dir, pattern);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(p, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    return 1;
}
#else
static int dir_exists(const char *p) { (void)p; return 1; }
static int glob_exists(const char *dir, const char *pattern) { (void)dir; (void)pattern; return 1; }
#endif

/* Append a line to the report. */
static void add(char *out, size_t cap, const char *fmt, const char *arg) {
    size_t n = strlen(out);
    if (n + 4 >= cap) return;
    snprintf(out + n, cap - n, fmt, arg);
}

static const char *check_eboot_buf(uint8_t *b, long len);

/* NULL if the EBOOT at `path` is usable, else why not. */
static const char *check_eboot(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return "cannot be read";
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = len > 0 ? (uint8_t *)malloc((size_t)len) : NULL;
    const int ok = b && fread(b, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    if (!ok) { free(b); return "cannot be read"; }
    return check_eboot_buf(b, len);
}

/* The same for an EBOOT in memory (taken and freed). */
static const char *check_eboot_buf(uint8_t *b, long len) {
    const char *why = NULL;
    if (len >= 4 && !memcmp(b, "~PSP", 4)) why = "is encrypted (~PSP): decrypt it first";
    else if (len < 52 || memcmp(b, "\x7F" "ELF", 4) != 0) why = "is not a PSP ELF";
    else {
        why = "is not the NPJH50332 (Phantasy Star Portable 2 Infinity) EBOOT this build was made from";
        const uint32_t phoff = *(const uint32_t *)(b + 0x1C);
        const uint16_t phentsize = *(const uint16_t *)(b + 0x2A), phnum = *(const uint16_t *)(b + 0x2C);
        for (unsigned i = 0; i < phnum && (size_t)phoff + (i + 1u) * phentsize <= (size_t)len; i++) {
            const uint32_t *ph = (const uint32_t *)(b + phoff + i * phentsize);
            const uint32_t type = ph[0], off = ph[1], vaddr = ph[2], filesz = ph[4];
            if (type != 1 || vaddr > CODE_LO || CODE_HI > vaddr + filesz) continue;
            const size_t lo = off + (CODE_LO - vaddr), hi = off + (CODE_HI - vaddr);
            if (hi > (size_t)len) break;
            uint64_t h = 0xCBF29CE484222325ull;
            for (size_t k = lo; k < hi; k++) h = (h ^ b[k]) * 0x100000001B3ull;
            if (h == CODE_HASH) why = NULL;
            break;
        }
    }
    free(b);
    return why;
}

/* The first *.iso in `d`, into out; 0 if there is none. */
static int find_iso_in(const char *d, char *out, size_t cap) {
#ifdef _WIN32
    char pat[800];
    snprintf(pat, sizeof pat, "%s\\*.iso", d);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    snprintf(out, cap, "%s\\%s", d, fd.cFileName);
    return 1;
#else
    (void)d; (void)out; (void)cap;
    return 0;
#endif
}

int prod_check(const char *dir, char *root, size_t root_cap, char *eboot, size_t eboot_cap,
               char *iso, size_t iso_cap) {
    char missing[4096] = "", p[800];
    snprintf(root, root_cap, "%s\\GameData", dir);

    if (!dir_exists(root)) add(missing, sizeof missing, "  - the GameData folder: %s\n", root);
    iso[0] = '\0';
    if (find_iso_in(dir, iso, iso_cap) || find_iso_in(root, iso, iso_cap)) {
        if (psp_io_open_disc_image(iso) != 0) {
            add(missing, sizeof missing, "  - a PSP disc image: %s is not one\n", iso);
        }
    } else {
        snprintf(p, sizeof p, "%s\\disc\\PSP_GAME\\PARAM.SFO", root);
        if (!file_exists(p)) add(missing, sizeof missing, "  - the game: an .iso next to psp2i.exe, or the extracted disc (%s)\n", p);
    }

    snprintf(eboot, eboot_cap, "%s\\EBOOT.BIN", dir);
    if (!file_exists(eboot)) {
        if (iso[0]) eboot[0] = '\0';
        else snprintf(eboot, eboot_cap, "%s\\disc\\PSP_GAME\\SYSDIR\\EBOOT.BIN", root);
    }
    const char *why = NULL;
    if (eboot[0]) {
        if (!file_exists(eboot)) add(missing, sizeof missing, "  - the game EBOOT: %s (or EBOOT.BIN next to psp2i.exe)\n", eboot);
        else why = check_eboot(eboot);
    } else if (psp_io_has_disc_image()) {
        uint32_t len = 0;
        uint8_t *b = psp_io_disc_file("PSP_GAME/SYSDIR/EBOOT.BIN", &len);
        why = b ? check_eboot_buf(b, (long)len) : "is missing from the disc image";
    }
    if (why) {
        char line[1000];
        snprintf(line, sizeof line, "  - a usable EBOOT: %s %s\n", eboot[0] ? eboot : "the disc image's EBOOT.BIN", why);
        add(missing, sizeof missing, "%s", line);
    }

    for (size_t i = 0; i < sizeof FONTS / sizeof FONTS[0]; i++) {
        snprintf(p, sizeof p, "%s\\flash\\font\\%s", root, FONTS[i]);
        if (!file_exists(p)) add(missing, sizeof missing, "  - a PSP firmware font: %s\n", p);
    }

    snprintf(p, sizeof p, "%s\\SDL3.dll", dir);
    if (!file_exists(p)) add(missing, sizeof missing, "  - %s\n", p);
    if (!glob_exists(dir, "psp2i_atrac.dll")) add(missing, sizeof missing, "  - the music decoder: psp2i_atrac.dll next to psp2i.exe%s\n", "");

    if (!missing[0]) return 0;
    char msg[5000];
    snprintf(msg, sizeof msg,
             "PSP2i cannot start. These are missing or unusable:\n\n%s\n"
             "Folder layout (next to psp2i.exe):\n"
             "  <game>.iso                    the game disc image (or GameData\\disc\\PSP_GAME\\...)\n"
             "  GameData\\flash\\font\\*.pgf    the PSP firmware fonts\n"
             "See README.txt.", missing);
    fprintf(stderr, "%s\n", msg);
#ifdef _WIN32
    MessageBoxA(NULL, msg, "PSP2i", MB_OK | MB_ICONERROR);
#endif
    return 1;
}
