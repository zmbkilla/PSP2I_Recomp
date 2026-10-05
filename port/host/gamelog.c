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
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define FN_GAME_PRINTF 0x0886EA30u
#define FN_QUEST_TABLE 0x08D25714u   /* QuestDataManager vfunc 0x1C: table object by ID, or 0 */
#define FN_ARC_FIND    0x08D56918u   /* archive/registry: object by name */
#define FN_TABLE_SLOT  0x0880B6C4u   /* quest table entry -> slot (flag +0x1C bit 0, slot +0x1E), else -1 */
#define FN_TABLE_TASK  0x0888B62Cu   /* client: quest-table request task ctor (this, ?, table, slot, mode) */

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

static void logf_line(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    emit(line, 1);
    online_log("game: %s", line);
}

/* The session server asks its quest data for a table by ID (0x1710 ->
 * 0x1711): which ID, and whether it has it. */
static void hook_quest_table(void (*original)(void)) {
    const uint32_t id = psp_cpu.r[5];
    original();
    logf_line("[quest data] table %d requested -> %s (0x%08X)", (int32_t)id, psp_cpu.r[2] ? "found" : "NOT FOUND", psp_cpu.r[2]);
}

/* Archive lookups by name, for the quest-table names only; the ID list
 * (tableIdList.rel: u32 *ids at +0, count at +4) is printed when found. */
static void hook_arc_find(void (*original)(void)) {
    char name[64];
    psp_str(psp_cpu.r[5], name, sizeof name);
    original();
    const uint32_t obj = psp_cpu.r[2];
    if (strncmp(name, "table", 5) && strncmp(name, "filelist", 8) && !strstr(name, "Quest")) return;
    if (!strcmp(name, "tableIdList.rel") && obj) {
        const uint32_t arr = psp_read32(obj), n = psp_read32(obj + 4);
        char ids[700];
        size_t k = 0;
        ids[0] = '\0';
        for (uint32_t i = 0; i < n && i < 120 && k + 12 < sizeof ids; i++)
            k += (size_t)snprintf(ids + k, sizeof ids - k, "%s%d", i ? "," : "", (int)psp_read32(arr + 4 * i));
        static char last[700];
        if (strcmp(last, ids)) { snprintf(last, sizeof last, "%s", ids); logf_line("[quest data] tableIdList: %u tables: %s", n, ids); }
        return;
    }
    static char lastn[64];
    if (strcmp(lastn, name)) { snprintf(lastn, sizeof lastn, "%s", name); logf_line("[quest data] find \"%s\" -> %s", name, obj ? "found" : "NOT FOUND"); }
}

static void dump_bytes(const char *what, uint32_t a, uint32_t n) {
    if (!a) return;
    for (uint32_t o = 0; o < n; o += 32) {
        char hex[3 * 32 + 1];
        for (uint32_t i = 0; i < 32; i++) snprintf(hex + 3 * i, 4, "%02x ", psp_read8(a + o + i));
        logf_line("[quest data]   %s +%02X: %s", what, o, hex);
    }
}

static void hook_table_slot(void (*original)(void)) {
    const uint32_t e = psp_cpu.r[4];
    original();
    logf_line("[quest data] table entry 0x%08X: flags 0x%04X, slot %d -> %d%s", e, psp_read16(e + 0x1C), (int16_t)psp_read16(e + 0x1E),
              (int32_t)psp_cpu.r[2], (int32_t)psp_cpu.r[2] < 0 ? " (NOT USABLE)" : "");
    dump_bytes("entry", e, 0x60);
}

static void hook_table_task(void (*original)(void)) {
    logf_line("[quest data] client requests quest table %d (slot %d, mode %d)", (int32_t)psp_cpu.r[6], (int32_t)psp_cpu.r[7], (int32_t)psp_cpu.r[8]);
    original();
}

/* ---- the party: who the host's client thinks is in it ---- */

static void hook_zone_player(void (*original)(void)) {
    const uint32_t pkt = psp_cpu.r[4];
    logf_line("[party] session 0x0201: player id %u, session %u, flags +0x28=%d +0x29=%d", psp_read16(pkt + 36),
              psp_read32(pkt + 16), (int8_t)psp_read8(pkt + 40), (int8_t)psp_read8(pkt + 41));
    original();
}

static void hook_is_local(void (*original)(void)) {
    const uint32_t pl = psp_cpu.r[4];
    original();
    static uint32_t last_pl, last_r = 0xFFFFFFFF;
    if (pl != last_pl || psp_cpu.r[2] != last_r) {
        last_pl = pl; last_r = psp_cpu.r[2];
        logf_line("[party] player 0x%08X (+0x28 = %u) local? -> %d", pl, pl ? psp_read32(pl + 40) : 0, (int32_t)psp_cpu.r[2]);
    }
}

/* 0x08CB611C = "this room is an infra lobby" (room size == lobby size, or the
 * lobby controller exists). Forcing it in a session turns the session into a
 * lobby (tested 2026-10-05), so it is correctly false there. */
static void hook_may_charainfo(void (*original)(void)) {
    const uint32_t m = psp_cpu.r[4];
    original();
    static int32_t last[3] = {-99, -99, -99};
    const int32_t now[3] = {(int32_t)psp_read32(m + 0x8FC), (int32_t)psp_read32(m + 0xB68), (int32_t)psp_cpu.r[2]};
    if (now[0] == last[0] && now[1] == last[1] && now[2] == last[2]) return;
    last[0] = now[0]; last[1] = now[1]; last[2] = now[2];
    logf_line("[party] chara-info allowed? +0x8FC=%d +0xB68=%d -> %d", now[0], now[1], now[2]);
}

static void hook_charainfo_req(void (*original)(void)) {
    const uint32_t m = psp_cpu.r[4];
    logf_line("[party] sendRoomMessageCharaInfoReq (me member %u, owner member %u)", psp_read16(m + 2224), psp_read16(m + 2226));
    original();
}

static void hook_member_info(void (*original)(void)) {
    const uint32_t m = psp_cpu.r[4], member = psp_cpu.r[5] & 0xFFFF;
    original();
    logf_line("[party] updateGameOfRoomMemberInfo(member %u) -> %d", member, (int32_t)psp_cpu.r[2]);
}

static void hook_issue_netid(void (*original)(void)) {
    const uint32_t m = psp_cpu.r[4];
    original();
    logf_line("[party] issueNetId (me member %u, owner member %u) -> net id %d", psp_read16(m + 2224), psp_read16(m + 2226), (int32_t)psp_cpu.r[2]);
    for (int i = 0; i < 12; i++) {
        const uint32_t e = m + 0x2A0 + 136u * (uint32_t)i;
        if (psp_read16(e + 0x34)) logf_line("[party]   member slot %d: used 0x%04X, net id %d", i, psp_read16(e + 0x34), (int32_t)psp_read32(e));
    }
}

/* The quest counter's slot comes from the zone the player stands in
 * (vfunc +0x110 of the mode object): online, zone id +0x18AE picks it
 * (3001 -> by area type +0x18A4, 2004 -> 0, 4001 -> 3, 5001 -> 2), else -1. */
static void hook_counter_info(void (*original)(void)) {
    const uint32_t out = psp_cpu.r[4], z = psp_cpu.r[5];
    original();
    logf_line("[quest data] counter info: online %d, zone +189C=%d +18A0=%d area +18A4=%d +18A8=0x%X +18AC=%d zone id +18AE=%d -> slot %d, table %d",
              (int32_t)psp_read32(0x08ED8FE8u), (int32_t)psp_read32(z + 0x189C), (int32_t)psp_read32(z + 0x18A0), (int32_t)psp_read32(z + 0x18A4),
              psp_read32(z + 0x18A8), (int16_t)psp_read16(z + 0x18AC), (int16_t)psp_read16(z + 0x18AE), (int32_t)psp_read32(out),
              (int32_t)psp_read32(out + 16));
}

/* Member table: 12 x 136 bytes; game info at +0x250 (128 B), net id at
 * +0x2A0 (= game info +0x50), room member id at +0x2D4. */
static void dump_members(uint32_t m) {
    for (int i = 0; i < 12; i++) {
        const uint32_t e = m + 136u * (uint32_t)i;
        if (psp_read16(e + 0x2D4))
            logf_line("[party]   member table %d: member id %u, net id %d", i, psp_read16(e + 0x2D4), (int32_t)psp_read32(e + 0x2A0));
    }
}

static void hook_member_netid(void (*original)(void)) {
    const uint32_t m = psp_cpu.r[4], member = psp_cpu.r[5] & 0xFFFF;
    original();
    static uint32_t last_member = 0xFFFFFFFF, last_r = 0;
    if (member == last_member && psp_cpu.r[2] == last_r) return;
    last_member = member; last_r = psp_cpu.r[2];
    logf_line("[party] net id of member %u -> %d  (me %u, owner %u)  [game: %08X]", member, (int32_t)psp_cpu.r[2],
              psp_read16(m + 2224), psp_read16(m + 2226), psp_cpu.r[31] - 8);
    dump_members(m);
}

/* Server: a client's party net id is taken from the matching member table by
 * the client's room member id (client+0x40) into client+0x60. */
static void hook_server_client_netid(void (*original)(void)) {
    const uint32_t c = psp_cpu.r[4];
    original();
    logf_line("[party] server client 0x%08X: member id %u -> net id %d (+0x20=%d +0x28=0x%08X)", c, psp_read16(c + 0x40),
              (int8_t)psp_read8(c + 0x60), (int32_t)psp_read32(c + 0x20), psp_read32(c + 0x28));
}

/* SetRoomDataInternal callback: room state +0x8 becomes 2 if +0x14 (set by
 * setRoomDataInternal's publish flag) is non-zero, else 1. */
static void hook_setdata_cb(void (*original)(void)) {
    const uint32_t m = psp_cpu.r[9];
    const int32_t err = (int32_t)psp_cpu.r[7];
    const uint32_t s4 = psp_read32(m + 4), s8 = psp_read32(m + 8), f14 = psp_read8(m + 0x14);
    original();
    logf_line("[party] SetRoomDataInternal callback (error 0x%08X): +0x14=%u, state +0x4 %u->%u, room state +0x8 %u->%u",
              (uint32_t)err, f14, s4, psp_read32(m + 4), s8, psp_read32(m + 8));
}

/* The character's items arrive in session packet 0x0A08; the client keeps
 * them in *(0x08ED8CEC)+0x58 -- and drops the packet if that object is
 * missing at the time (0x08ABB4D8). */
#define ITEM_OWNER 0x08ED8CECu
int game_stack(char *out, size_t cap, int max_frames);     /* main.c */
static void hook_items_packet(void (*original)(void)) {
    const uint32_t pkt = psp_cpu.r[4], o = psp_read32(ITEM_OWNER);
    char where[400] = "";
    game_stack(where, sizeof where, 16);
    logf_line("[party] items packet 0x0A08 (%u items) at vblank %llu: owner object 0x%08X%s  [game: %s]",
              psp_read8(pkt + 28), (unsigned long long)psp_sched_vblank_count(), o, o ? "" : " -- MISSING, packet dropped", where);
    original();
}
static void hook_items_owner_new(void (*original)(void)) {
    const uint32_t before = psp_read32(ITEM_OWNER);
    char where[400] = "";
    game_stack(where, sizeof where, 16);
    original();
    logf_line("[party] item owner object at vblank %llu: 0x%08X -> 0x%08X  [game: %s]",
              (unsigned long long)psp_sched_vblank_count(), before, psp_read32(ITEM_OWNER), where);
}
static void hook_items_owner_del(void (*original)(void)) {
    const uint32_t before = psp_read32(ITEM_OWNER);
    original();
    logf_line("[party] item owner object destroyed: 0x%08X -> 0x%08X  [game: %08X]", before, psp_read32(ITEM_OWNER), psp_cpu.r[31] - 8);
}

static void ms_line(const char *line) { logf_line("[file] %s", line); }

void gamelog_init(const char *exe_dir) {
    snprintf(g_dir, sizeof g_dir, "%s", exe_dir);
    psp_hook_set(FN_GAME_PRINTF, hook_printf);
    psp_hook_set(FN_QUEST_TABLE, hook_quest_table);
    psp_hook_set(FN_ARC_FIND, hook_arc_find);
    psp_hook_set(FN_TABLE_SLOT, hook_table_slot);
    psp_hook_set(FN_TABLE_TASK, hook_table_task);
    psp_hook_set(0x08B330B0u, hook_zone_player);
    psp_hook_set(0x08A3DE1Cu, hook_is_local);
    psp_hook_set(0x08CB611Cu, hook_may_charainfo);
    psp_hook_set(0x08CB3E40u, hook_setdata_cb);
    psp_hook_set(0x08ABB4A8u, hook_items_packet);
    psp_hook_set(0x08ABB3A4u, hook_items_owner_new);
    psp_hook_set(0x08ABBD6Cu, hook_items_owner_del);
    psp_hook_set(0x08CB25E0u, hook_charainfo_req);
    psp_hook_set(0x08CB2B5Cu, hook_member_info);
    psp_hook_set(0x08CB54E8u, hook_issue_netid);
    psp_hook_set(0x089489FCu, hook_counter_info);
    psp_hook_set(0x08CB1538u, hook_member_netid);
    psp_hook_set(0x08D10ACCu, hook_server_client_netid);
    psp_io_set_ms_log(ms_line);
}
