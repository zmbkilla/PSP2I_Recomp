#ifndef PSP2I_MODELX_H
#define PSP2I_MODELX_H
#include <stddef.h>
/* Model extraction (modelx.c): the package-loader hook that records file
 * names (at start), and the engine menu's EXTRACT MODELS: every loaded model
 * to <base_dir>/Data/<group>/<name>.glb, existing files kept. Returns the
 * number written; `summary` gets a short line for the menu. */
void modelx_init(void);
int  modelx_extract(const char *base_dir, char *summary, size_t cap);
#include <stdint.h>
/* LOAD MODELS: models in <base_dir>/Data replace the game's as they load
 * (when `load`); textures named after the game's replace those in the
 * renderer (modelx_texture for gl_ge_set_texture_source). */
void modelx_start(const char *base_dir, int load);
void modelx_set_load(int on);
void modelx_vblank(void);
const uint8_t *modelx_texture(uint32_t addr, int *w, int *h);
unsigned modelx_texture_gen(void);
#endif
