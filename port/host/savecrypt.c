/* PSP savedata encryption (the firmware's chnnlsv service), so saves are
 * interchangeable with a real PSP and with PPSSPP.
 *
 * Ported from PPSSPP (Core/HLE/sceChnnlsv.cpp and the save routines in
 * Core/Dialog/SavedataParam.cpp, https://github.com/hrydgard/ppsspp), which is
 * licensed under the GNU GPL 2.0 or later; the key constants below come from
 * libkirk (ext/libkirk/kirk_engine.c, Draan et al., GPL 3.0 or later), as
 * published there. This file is therefore under the GNU GPL (3.0 or later).
 *
 * Only the PSP's AES-CBC commands with fixed keys are needed (KIRK commands
 * 4 and 7): the save modes a game can use without a console-unique key
 * (1, 3 and 5; PSP2i uses 5). The console-unique ("fuse") commands are not
 * available, exactly as in PPSSPP: hashes that need them come out as the
 * 0x01 fill PPSSPP writes. AES itself is Windows' (BCrypt); the IV seed
 * comes from the system random generator instead of KIRK command 14. */

#include "savecrypt.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#endif

/* ---- KIRK commands 4 / 7 ----------------------------------------------------------- */

/* The libkirk key slots the save modes use (KIRK key seeds). */
static const struct { int seed; uint8_t key[16]; } KEYS[] = {
    {   3, { 0x98, 0x02, 0xC4, 0xE6, 0xEC, 0x9E, 0x9E, 0x2F, 0xFC, 0x63, 0x4C, 0xE4, 0x2F, 0xBB, 0x46, 0x68 } },
    {   4, { 0x99, 0x24, 0x4C, 0xD2, 0x58, 0xF5, 0x1B, 0xCB, 0xB0, 0x61, 0x9C, 0xA7, 0x38, 0x30, 0x07, 0x5F } },
    {   5, { 0x02, 0x25, 0xD7, 0xBA, 0x63, 0xEC, 0xB9, 0x4A, 0x9D, 0x23, 0x76, 0x01, 0xB3, 0xF6, 0xAC, 0x17 } },
    {  12, { 0x84, 0x85, 0xC8, 0x48, 0x75, 0x08, 0x43, 0xBC, 0x9B, 0x9A, 0xEC, 0xA7, 0x9C, 0x7F, 0x60, 0x18 } },
    {  13, { 0xB5, 0xB1, 0x6E, 0xDE, 0x23, 0xA9, 0x7B, 0x0E, 0xA1, 0x7C, 0xDB, 0xA2, 0xDC, 0xDE, 0xC4, 0x6E } },
    {  14, { 0xC8, 0x71, 0xFD, 0xB3, 0xBC, 0xC5, 0xD2, 0xF2, 0xE2, 0xD7, 0x72, 0x9D, 0xDF, 0x82, 0x68, 0x82 } },
    {  16, { 0x32, 0x29, 0x5B, 0xD5, 0xEA, 0xF7, 0xA3, 0x42, 0x16, 0xC8, 0x8E, 0x48, 0xFF, 0x50, 0xD3, 0x71 } },
    {  17, { 0x46, 0xF2, 0x5E, 0x8E, 0x4D, 0x2A, 0xA5, 0x40, 0x73, 0x0B, 0xC4, 0x6E, 0x47, 0xEE, 0x6F, 0x0A } },
    {  18, { 0x5D, 0xC7, 0x11, 0x39, 0xD0, 0x19, 0x38, 0xBC, 0x02, 0x7F, 0xDD, 0xDC, 0xB0, 0x83, 0x7D, 0x9D } },
    {  83, { 0xAF, 0xFE, 0x8E, 0xB1, 0x3D, 0xD1, 0x7E, 0xD8, 0x0A, 0x61, 0x24, 0x1C, 0x95, 0x92, 0x56, 0xB6 } },
    {  87, { 0x1C, 0x9B, 0xC4, 0x90, 0xE3, 0x06, 0x64, 0x81, 0xFA, 0x59, 0xFD, 0xB6, 0x00, 0xBB, 0x28, 0x70 } },
    { 100, { 0x03, 0xB3, 0x02, 0xE8, 0x5F, 0xF3, 0x81, 0xB1, 0x3B, 0x8D, 0xAA, 0x2A, 0x90, 0xFF, 0x5E, 0x61 } },
};
#define NKEYS ((int)(sizeof KEYS / sizeof KEYS[0]))

enum { KIRK_CMD_ENCRYPT_IV_0 = 4, KIRK_CMD_ENCRYPT_IV_FUSE = 5, KIRK_CMD_DECRYPT_IV_0 = 7, KIRK_CMD_DECRYPT_IV_FUSE = 8 };
enum { KIRK_MODE_ENCRYPT_CBC = 4, KIRK_MODE_DECRYPT_CBC = 5 };

#ifdef _WIN32
static BCRYPT_ALG_HANDLE g_alg;
static BCRYPT_KEY_HANDLE g_key[NKEYS];
static CRITICAL_SECTION g_cs;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_once(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_cs);
    if (BCryptOpenAlgorithmProvider(&g_alg, BCRYPT_AES_ALGORITHM, NULL, 0) != 0) { g_alg = NULL; return TRUE; }
    BCryptSetProperty(g_alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof BCRYPT_CHAIN_MODE_ECB, 0);
    for (int i = 0; i < NKEYS; i++)
        if (BCryptGenerateSymmetricKey(g_alg, &g_key[i], NULL, 0, (PUCHAR)KEYS[i].key, 16, 0) != 0) g_key[i] = NULL;
    return TRUE;
}

/* One AES block with key slot `k`. */
static int aes_block(int k, int enc, const uint8_t in[16], uint8_t out[16]) {
    ULONG got = 0;
    uint8_t t[16];
    memcpy(t, in, 16);
    NTSTATUS st = enc ? BCryptEncrypt(g_key[k], t, 16, NULL, NULL, 0, out, 16, &got, 0)
                      : BCryptDecrypt(g_key[k], t, 16, NULL, NULL, 0, out, 16, &got, 0);
    return st == 0 && got == 16 ? 0 : -1;
}

static void random_bytes(uint8_t *p, ULONG n) { BCryptGenRandom(NULL, p, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG); }
#endif

static int key_index(int seed) {
    for (int i = 0; i < NKEYS; i++) if (KEYS[i].seed == seed) return i;
    return -1;
}

static int32_t rd32(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, int32_t v) { memcpy(p, &v, 4); }

/* KIRK AES-CBC (zero IV) on a buffer with the 20-byte header in front:
 * mode, 0, 0, key seed, size. Command 4 encrypts the data in place (after
 * the header); command 7 decrypts it to the start of the buffer. The fuse
 * commands (5, 8) need the console's key and fail, as in PPSSPP. */
static int kirk_cbc(uint8_t *buf, int cmd) {
#ifdef _WIN32
    if (cmd != KIRK_CMD_ENCRYPT_IV_0 && cmd != KIRK_CMD_DECRYPT_IV_0) return -1;
    const int mode = rd32(buf), seed = rd32(buf + 12), size = rd32(buf + 16);
    if (cmd == KIRK_CMD_ENCRYPT_IV_0 && mode != KIRK_MODE_ENCRYPT_CBC) return -1;
    if (cmd == KIRK_CMD_DECRYPT_IV_0 && mode != KIRK_MODE_DECRYPT_CBC) return -1;
    if (size <= 0 || (size & 15)) return -1;
    InitOnceExecuteOnce(&g_once, init_once, NULL, NULL);
    const int k = key_index(seed);
    if (!g_alg || k < 0 || !g_key[k]) return -1;
    uint8_t iv[16] = { 0 }, blk[16];
    uint8_t *d = buf + 20;
    if (cmd == KIRK_CMD_ENCRYPT_IV_0) {
        for (int i = 0; i < size; i += 16) {
            for (int j = 0; j < 16; j++) blk[j] = d[i + j] ^ iv[j];
            if (aes_block(k, 1, blk, d + i)) return -1;
            memcpy(iv, d + i, 16);
        }
    } else {
        for (int i = 0; i < size; i += 16) {          /* output 20 bytes lower: safe front to back */
            uint8_t c[16];
            memcpy(c, d + i, 16);
            if (aes_block(k, 0, c, blk)) return -1;
            for (int j = 0; j < 16; j++) buf[i + j] = blk[j] ^ iv[j];
            memcpy(iv, c, 16);
        }
    }
    return 0;
#else
    (void)buf; (void)cmd;
    return -1;
#endif
}

/* ---- chnnlsv (sceSd*) ----------------------------------------------------------------- */

typedef struct { int mode; uint8_t result[16], key[16]; int keyLength; } mac_ctx;     /* pspChnnlsvContext1 */
typedef struct { int mode, unkn; uint8_t cryptedData[0x92]; } cipher_ctx;              /* pspChnnlsvContext2 */

static uint8_t dataBuf[2048 + 20];
static uint8_t *const dataBuf2 = dataBuf + 20;

static const uint8_t hash198C[16] = {0xFA, 0xAA, 0x50, 0xEC, 0x2F, 0xDE, 0x54, 0x93, 0xAD, 0x14, 0xB2, 0xCE, 0xA5, 0x30, 0x05, 0xDF};
static const uint8_t hash19BC[16] = {0xCB, 0x15, 0xF4, 0x07, 0xF9, 0x6A, 0x52, 0x3C, 0x04, 0xB9, 0xB2, 0xEE, 0x5C, 0x53, 0xFA, 0x86};
static const uint8_t key19CC[16]  = {0x70, 0x44, 0xA3, 0xAE, 0xEF, 0x5D, 0xA5, 0xF2, 0x85, 0x7F, 0xF2, 0xD6, 0x94, 0xF5, 0x36, 0x3B};
static const uint8_t key19DC[16]  = {0xEC, 0x6D, 0x29, 0x59, 0x26, 0x35, 0xA5, 0x7F, 0x97, 0x2A, 0x0D, 0xBC, 0xA3, 0x26, 0x33, 0x00};
static const uint8_t key199C[16]  = {0x36, 0xA5, 0x3E, 0xAC, 0xC5, 0x26, 0x9E, 0xA3, 0x83, 0xD9, 0xEC, 0x25, 0x6C, 0x48, 0x48, 0x72};
static const uint8_t key19AC[16]  = {0xD8, 0xC0, 0xB0, 0xF3, 0x3E, 0x6B, 0x76, 0x85, 0xFD, 0xFB, 0x4D, 0x7D, 0x45, 0x1E, 0x92, 0x03};

static void memxor(uint8_t *d, const uint8_t *s, size_t n) { while (n--) *d++ ^= *s++; }

static int numFromMode(int mode) {
    switch (mode) { case 1: return 3; case 2: return 5; case 3: return 12; case 4: return 13; case 6: return 17; default: return 16; }
}
static int numFromMode2(int mode) { return mode == 1 ? 4 : mode == 3 ? 14 : 18; }
static int typeFromMode(int mode) { return (mode == 1 || mode == 2) ? 83 : (mode == 3 || mode == 4) ? 87 : 100; }

static int kirkSendCmd(uint8_t *data, int length, int num, int encrypt) {
    wr32(data + 0, encrypt ? KIRK_MODE_ENCRYPT_CBC : KIRK_MODE_DECRYPT_CBC);
    wr32(data + 4, 0);
    wr32(data + 8, 0);
    wr32(data + 12, num);
    wr32(data + 16, length);
    return kirk_cbc(data, encrypt ? KIRK_CMD_ENCRYPT_IV_0 : KIRK_CMD_DECRYPT_IV_0) ? -257 : 0;
}

static int kirkSendFuseCmd(uint8_t *data, int length, int encrypt) {
    wr32(data + 0, encrypt ? KIRK_MODE_ENCRYPT_CBC : KIRK_MODE_DECRYPT_CBC);
    wr32(data + 4, 0);
    wr32(data + 8, 0);
    wr32(data + 12, 256);
    wr32(data + 16, length);
    return kirk_cbc(data, encrypt ? KIRK_CMD_ENCRYPT_IV_FUSE : KIRK_CMD_DECRYPT_IV_FUSE) ? -258 : 0;
}

static int sub_15B0(uint8_t *data, int alignedLen, uint8_t *buf, int val) {
    uint8_t sp0[16];
    memcpy(sp0, data + alignedLen + 4, 16);
    int res = kirkSendCmd(data, alignedLen, val, 0);
    if (res) return res;
    memxor(data, buf, 16);
    memcpy(buf, sp0, 16);
    return 0;
}

static int sub_0000(uint8_t *data_out, uint8_t *data, int alignedLen, const uint8_t *data2, int *data3, int mode) {
    memcpy(data_out + 20, data2, 16);
    const int type = typeFromMode(mode);
    int res;
    if (type == 87) memxor(data_out + 20, key19AC, 16);
    else if (type == 100) memxor(data_out + 20, key19DC, 16);
    switch (mode) {
    case 2: case 4: case 6: res = kirkSendFuseCmd(data_out, 16, 0); break;
    default:                res = kirkSendCmd(data_out, 16, numFromMode2(mode), 0); break;
    }
    if (type == 87) memxor(data_out, key199C, 16);
    else if (type == 100) memxor(data_out, key19CC, 16);
    if (res) return res;

    uint8_t sp0[16], sp16[16];
    memcpy(sp16, data_out, 16);
    if (*data3 == 1) memset(sp0, 0, 16);
    else { memcpy(sp0, sp16, 12); wr32(sp0 + 12, *data3 - 1); }
    for (int i = 20; i < alignedLen + 20; i += 16) {
        memcpy(data_out + i, sp16, 12);
        wr32(data_out + 12 + i, *data3);
        (*data3)++;
    }
    res = sub_15B0(data_out, alignedLen, sp0, type);
    if (res) return res;
    if (alignedLen > 0) memxor(data, data_out, (size_t)alignedLen);
    return 0;
}

static int sub_1510(uint8_t *data, int size, uint8_t *result, int num) {
    memxor(data + 20, result, 16);
    int res = kirkSendCmd(data, size, num, 1);
    if (res) return res;
    memcpy(result, data + size + 4, 16);
    return 0;
}

static int sceSdMacInit(mac_ctx *ctx, int value) {
    ctx->mode = value;
    memset(ctx->result, 0, 16);
    memset(ctx->key, 0, 16);
    ctx->keyLength = 0;
    return 0;
}

static int sceSdMacUpdate(mac_ctx *ctx, const uint8_t *data, int length) {
    if (ctx->keyLength >= 17) return -1026;
    if (ctx->keyLength + length < 17) {
        memcpy(ctx->key + ctx->keyLength, data, (size_t)length);
        ctx->keyLength += length;
        return 0;
    }
    const int num = numFromMode(ctx->mode);
    memset(dataBuf2, 0, 2048);
    memcpy(dataBuf2, ctx->key, (size_t)ctx->keyLength);
    int len = (ctx->keyLength + length) & 0xF;
    if (len == 0) len = 16;
    int newSize = ctx->keyLength;
    ctx->keyLength = len;
    const int diff = length - len;
    memcpy(ctx->key, data + diff, (size_t)len);
    for (int i = 0; i < diff; i++) {
        if (newSize == 2048) {
            int res = sub_1510(dataBuf, 2048, ctx->result, num);
            if (res) return res;
            newSize = 0;
        }
        dataBuf2[newSize++] = data[i];
    }
    if (newSize) sub_1510(dataBuf, newSize, ctx->result, num);
    return 0;                          /* as the firmware does (see PPSSPP) */
}

static void shift_left_xor(uint8_t d[16]) {
    const int t = (d[0] & 0x80) ? 135 : 0;
    for (int i = 0; i < 15; i++) d[i] = (uint8_t)((d[i] << 1) | (d[i + 1] >> 7));
    d[15] = (uint8_t)((d[15] << 1) ^ t);
}

static int sceSdMacFinal(mac_ctx *ctx, uint8_t *in_hash, const uint8_t *in_key) {
    if (ctx->keyLength >= 17) return -1026;
    const int num = numFromMode(ctx->mode);
    memset(dataBuf2, 0, 16);
    int res = kirkSendCmd(dataBuf, 16, num, 1);
    if (res) return res;
    uint8_t data1[16], data2[16];
    memcpy(data1, dataBuf2, 16);
    shift_left_xor(data1);
    if (ctx->keyLength < 16) {
        shift_left_xor(data1);
        const int old = ctx->keyLength;
        ctx->key[old] = 0x80;
        if (old + 1 < 16) memset(ctx->key + old + 1, 0, (size_t)(16 - (old + 1)));
    }
    memxor(ctx->key, data1, 16);
    memcpy(dataBuf2, ctx->key, 16);
    memcpy(data2, ctx->result, 16);
    res = sub_1510(dataBuf, 16, data2, num);
    if (res) return res;
    if (ctx->mode == 3 || ctx->mode == 4) memxor(data2, hash198C, 16);
    else if (ctx->mode == 5 || ctx->mode == 6) memxor(data2, hash19BC, 16);
    if (ctx->mode == 2 || ctx->mode == 4 || ctx->mode == 6) {
        memcpy(dataBuf2, data2, 16);
        res = kirkSendFuseCmd(dataBuf, 16, 1);
        if (res) return res;
        res = kirkSendCmd(dataBuf, 16, num, 1);
        if (res) return res;
        memcpy(data2, dataBuf2, 16);
    }
    if (in_key) {
        for (int i = 0; i < 16; i++) data2[i] ^= in_key[i];
        memcpy(dataBuf2, data2, 16);
        res = kirkSendCmd(dataBuf, 16, num, 1);
        if (res) return res;
        memcpy(data2, dataBuf2, 16);
    }
    memcpy(in_hash, data2, 16);
    sceSdMacInit(ctx, 0);
    return 0;
}

static int sceSdCipherInit(cipher_ctx *ctx2, int mode, int uknw, uint8_t *data, const uint8_t *cryptkey) {
    ctx2->mode = mode;
    ctx2->unkn = 1;
    if (uknw == 2) {
        memcpy(ctx2->cryptedData, data, 16);
        if (cryptkey) memxor(ctx2->cryptedData, cryptkey, 16);
        return 0;
    }
    if (uknw == 1) {
        uint8_t kirkHeader[37];
        uint8_t *kirkData = kirkHeader + 20;
#ifdef _WIN32
        random_bytes(kirkHeader, 20);              /* KIRK command 14 (random) in the firmware */
#endif
        memcpy(kirkHeader + 20, kirkHeader, 16);
        memset(kirkHeader + 32, 0, 4);
        const int type = typeFromMode(mode);
        if (type == 87) memxor(kirkData, key199C, 16);
        else if (type == 100) memxor(kirkData, key19CC, 16);
        int res;
        switch (mode) {
        case 2: case 4: case 6: res = kirkSendFuseCmd(kirkHeader, 16, 1); break;
        default:                res = kirkSendCmd(kirkHeader, 16, numFromMode2(mode), 1); break;
        }
        if (type == 87) memxor(kirkData, key19AC, 16);
        else if (type == 100) memxor(kirkData, key19DC, 16);
        if (res) return res;
        memcpy(ctx2->cryptedData, kirkData, 16);
        memcpy(data, kirkData, 16);
        if (cryptkey) memxor(ctx2->cryptedData, cryptkey, 16);
    }
    return 0;
}

static int sceSdCipherUpdate(cipher_ctx *ctx, uint8_t *data, int alignedLen) {
    if (alignedLen == 0) return 0;
    if (alignedLen & 0xF) return -1025;
    static uint8_t kirkData[20 + 2048];
    int i = 0;
    while (alignedLen >= 2048) {
        int res = sub_0000(kirkData, data + i, 2048, ctx->cryptedData, &ctx->unkn, ctx->mode);
        alignedLen -= 2048;
        i += 2048;
        if (res) return res;
    }
    if (alignedLen == 0) return 0;
    return sub_0000(kirkData, data + i, alignedLen, ctx->cryptedData, &ctx->unkn, ctx->mode);
}

static int sceSdCipherFinal(cipher_ctx *ctx) {
    memset(ctx->cryptedData, 0, 16);
    ctx->unkn = 0;
    ctx->mode = 0;
    return 0;
}

/* ---- savedata (PPSSPP SavedataParam) -------------------------------------------------- */

static uint32_t align16(uint32_t n) { return (n + 15) & ~15u; }

static void lock(void) {
#ifdef _WIN32
    InitOnceExecuteOnce(&g_once, init_once, NULL, NULL);
    EnterCriticalSection(&g_cs);
#endif
}
static void unlock(void) {
#ifdef _WIN32
    LeaveCriticalSection(&g_cs);
#endif
}

/* DecryptData: data = the file (16-byte IV + cipher text), zero-padded to
 * a 16-byte multiple by the caller. */
static int decrypt_data(int mode, uint8_t *data, uint32_t *dataLen, uint32_t *alignedLen, const uint8_t *cryptkey, const uint8_t *expectedHash) {
    mac_ctx ctx1; cipher_ctx ctx2;
    memset(&ctx1, 0, sizeof ctx1); memset(&ctx2, 0, sizeof ctx2);
    if (*alignedLen <= 0x10) return -1;
    *dataLen -= 0x10;
    *alignedLen -= 0x10;
    if (sceSdMacInit(&ctx1, mode) < 0) return -2;
    if (sceSdCipherInit(&ctx2, mode, 2, data, cryptkey) < 0) return -3;
    if (sceSdMacUpdate(&ctx1, data, 0x10) < 0) return -4;
    if (sceSdMacUpdate(&ctx1, data + 0x10, (int)*alignedLen) < 0) return -5;
    if (sceSdCipherUpdate(&ctx2, data + 0x10, (int)*alignedLen) < 0) return -6;
    if (sceSdCipherFinal(&ctx2) < 0) return -7;
    if (expectedHash) {
        uint8_t hash[16];
        if (sceSdMacFinal(&ctx1, hash, cryptkey) < 0) return -7;
        if (memcmp(hash, expectedHash, 16) != 0) return -8;
    }
    memmove(data, data + 0x10, *dataLen);
    return 0;
}

int savecrypt_decrypt(int mode, const uint8_t *file, uint32_t file_len, const uint8_t *key,
                      const uint8_t *expected_hash, uint8_t **out, uint32_t *out_len) {
    *out = NULL; *out_len = 0;
    if (file_len <= 0x10) return -1;
    uint32_t aligned = align16(file_len), len = file_len;
    uint8_t *buf = (uint8_t *)calloc(1, aligned);
    if (!buf) return -1;
    memcpy(buf, file, file_len);
    lock();
    const int rc = decrypt_data(mode, buf, &len, &aligned, mode > 1 ? key : NULL, expected_hash);
    unlock();
    if (rc) { free(buf); return rc; }
    *out = buf;
    *out_len = len;
    return 0;
}

int savecrypt_encrypt(int mode, const uint8_t *plain, uint32_t len, const uint8_t *key,
                      uint8_t **out, uint32_t *out_len, uint8_t hash[16]) {
    *out = NULL; *out_len = 0;
    if (!len) return -1;
    const uint32_t aligned = align16(len);
    uint8_t *data = (uint8_t *)calloc(1, aligned + 0x10);
    if (!data) return -1;
    memcpy(data + 0x10, plain, len);           /* the IV goes in front */
    const uint8_t *ck = mode > 1 ? key : NULL;
    mac_ctx ctx1; cipher_ctx ctx2;
    memset(&ctx1, 0, sizeof ctx1); memset(&ctx2, 0, sizeof ctx2);
    memset(hash, 0, 16);
    int rc = 0;
    lock();
    if (sceSdCipherInit(&ctx2, mode, 1, data, ck) < 0) rc = -1;
    else if (sceSdMacInit(&ctx1, mode) < 0) rc = -2;
    else if (sceSdMacUpdate(&ctx1, data, 0x10) < 0) rc = -3;
    else if (sceSdCipherUpdate(&ctx2, data + 0x10, (int)aligned) < 0) rc = -4;
    else {
        memset(data + 0x10 + len, 0, aligned - len);
        if (sceSdMacUpdate(&ctx1, data + 0x10, (int)aligned) < 0) rc = -5;
        else if (sceSdCipherFinal(&ctx2) < 0) rc = -6;
        else if (sceSdMacFinal(&ctx1, hash, ck) < 0) rc = -7;
    }
    unlock();
    if (rc) { free(data); return rc; }
    *out = data;
    *out_len = len + 0x10;
    return 0;
}

/* BuildHash: 0x01 fill when the mode needs the console key (as PPSSPP). */
static void build_hash(uint8_t out[16], const uint8_t *data, uint32_t alignedLen, int mode) {
    mac_ctx ctx1;
    memset(&ctx1, 0, sizeof ctx1);
    memset(out, 0, 16);
    sceSdMacInit(&ctx1, mode & 0xFF);
    sceSdMacUpdate(&ctx1, data, (int)alignedLen);
    if (sceSdMacFinal(&ctx1, out, NULL) < 0) memset(out, 0x01, 16);
}

int savecrypt_sfo_hash(uint8_t *sfo, uint32_t sfo_size, uint32_t params_off, int mode) {
    const uint32_t aligned = align16(sfo_size);   /* the caller pads with zeroes */
    uint8_t *p = sfo + params_off, h[16];
    memset(p, 0, 128);
    int first = (mode & 2) ? 4 : 2, second = (mode & 2) ? 3 : 0;
    if (mode & 4) { first = 6; second = 5; }
    lock();
    build_hash(h, sfo, aligned, first);
    memcpy(p + 0x20, h, 16);
    p[0] |= 0x01;
    if (mode & 6) {
        p[0] |= (uint8_t)((mode & 6) << 4);
        build_hash(h, sfo, aligned, second);
        memcpy(p + 0x70, h, 16);
    }
    build_hash(h, sfo, aligned, 1);
    memcpy(p + 0x10, h, 16);
    unlock();
    return 0;
}
