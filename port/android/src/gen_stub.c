/* Stands in for port/gen (the C allegrexrecomp generates from your own
 * decrypted EBOOT.BIN, which the repository cannot carry), so the app builds
 * without it. sdl_main.c is built with PSP2I_NO_GAME_CODE then and says what
 * is missing instead of starting. */
void psp_recomp_register(void) {}
