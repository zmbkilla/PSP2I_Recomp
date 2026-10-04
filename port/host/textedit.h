/* A one-line text editor for the settings menu (the SEGA server address).
 * Keyboard: type, Backspace deletes, Enter saves, Escape cancels.
 * Controller: an on-screen keyboard -- D-pad picks, Circle types, Square
 * deletes, Start saves, Cross cancels. (Circle accepts, Cross goes back.) */
#ifndef PSP2I_TEXTEDIT_H
#define PSP2I_TEXTEDIT_H

#include <stdint.h>
#include "menu.h"

#define TEXTEDIT_MAX 96
#define TE_COLS 10
#define TE_ROWS 5

enum { TE_NONE = 0, TE_SAVE, TE_CANCEL };

typedef struct {
    int      open;
    char     title[48];
    char     hint[96];
    char     text[TEXTEDIT_MAX + 1];
    int      kx, ky;                   /* on-screen keyboard cursor */
    uint32_t prev, prev_psp;
} textedit;

void textedit_open(textedit *e, const char *title, const char *hint, const char *initial);
/* held: MI_* (menu_inputs_from_psp + navigation keys); psp: PSP buttons.
 * Returns TE_SAVE / TE_CANCEL when finished (then e->open is 0). */
int  textedit_update(textedit *e, uint32_t held, uint32_t psp);
void textedit_type(textedit *e, char c);
void textedit_backspace(textedit *e);
void textedit_draw(const textedit *e, menu_image *img);
char textedit_key(int x, int y);

#endif /* PSP2I_TEXTEDIT_H */
