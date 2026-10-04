/* PSP savedata encryption (ported from PPSSPP; GPL). See savecrypt.c. */
#ifndef PSP2I_SAVECRYPT_H
#define PSP2I_SAVECRYPT_H

#include <stdint.h>

/* mode: 1 (no key), 3 or 5 (with the game's 16-byte key). 0 on success. */
int savecrypt_decrypt(int mode, const uint8_t *file, uint32_t file_len, const uint8_t *key,
                      const uint8_t *expected_hash, uint8_t **out, uint32_t *out_len);
int savecrypt_encrypt(int mode, const uint8_t *plain, uint32_t len, const uint8_t *key,
                      uint8_t **out, uint32_t *out_len, uint8_t hash[16]);
/* Fill PARAM.SFO's SAVEDATA_PARAMS (128 bytes at params_off) with the save's
 * hashes. sfo must be zero-padded to a 16-byte multiple past sfo_size. */
int savecrypt_sfo_hash(uint8_t *sfo, uint32_t sfo_size, uint32_t params_off, int mode);

#endif
