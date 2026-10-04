/* Game controllers and joysticks through SDL3, mapped to PSP buttons.
 *
 * SDL3.dll is loaded at run time (the package next to the exe has no headers
 * or import library), so the port builds without SDL and runs keyboard-only
 * if the DLL is absent. See input_sdl.c for the mapping. */
#ifndef PSP2I_INPUT_SDL_H
#define PSP2I_INPUT_SDL_H

#include <stdint.h>

/* Load SDL3.dll and start the gamepad/joystick subsystems. 0 on success. */
int  input_sdl_init(void);
void input_sdl_shutdown(void);

/* Call once per vblank: updates devices (connects/disconnects included) and
 * returns the combined PSP button bits and analog stick (0..255, 128 =
 * centre) of every connected device. Returns 0 if SDL is not active. */
int  input_sdl_poll(uint32_t *buttons, uint8_t *ax, uint8_t *ay);

#endif /* PSP2I_INPUT_SDL_H */
