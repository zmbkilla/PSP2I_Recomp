/* The game's text font on Windows (sceLibFont, psprecomp/src/hle/font.c),
 * drawn by GDI from a TrueType/OpenType font instead of the PSP's PGF files.
 *
 * The choices, in the settings menu's FONT item and psp2i_display.ini font=:
 *   system     Windows' message font (the user's setting; Segoe UI unless
 *              changed); characters it lacks come from Yu Gothic UI, Meiryo,
 *              MS Gothic, Malgun Gothic (Japanese, Korean)
 *   psp        the PSP's own fonts (the .pgf files in flash/font/), offered
 *              only if jpn0.pgf is there
 *   <file>     a .ttf/.otf/.ttc in the fonts folder next to the exe, with the
 *              same fallbacks for missing characters
 * Glyphs are sized to the PSP font's (about 14 pixels for a capital, an 18
 * pixel line). Text the game already drew keeps its old font. */
#ifndef PSP2I_FONT_WIN_H
#define PSP2I_FONT_WIN_H

/* Scan the choices: fonts_dir (the folder of extra fonts) and pgf_dir (where
 * jpn0.pgf would be). Returns how many there are (at least 1, system). */
int         font_win_init(const char *fonts_dir, const char *pgf_dir);
int         font_win_count(void);
const char *font_win_name(int i);       /* for the menu: "SYSTEM", "PSP", "MYFONT" */
const char *font_win_key(int i);        /* for the ini: "system", "psp", "MyFont.ttf" */
int         font_win_find(const char *key);   /* index of an ini value; -1 if not there */

/* Draw the game's text with choice i from the next character. Returns the
 * choice in effect (system if i cannot be loaded). */
int         font_win_select(int i);

#endif /* PSP2I_FONT_WIN_H */
