/* ATRAC3plus decoding through psp2i_atrac.dll (port/atrac_dll), loaded at run
 * time and handed to the runtime through psp_atrac_set_codec. See atrac_at3.c. */
#ifndef PSP2I_ATRAC_AT3_H
#define PSP2I_ATRAC_AT3_H

/* Load psp2i_atrac.dll from the exe's directory; 0 on success. On failure
 * ATRAC music is silent. */
int atrac_at3_init(const char *exedir);

#endif /* PSP2I_ATRAC_AT3_H */
