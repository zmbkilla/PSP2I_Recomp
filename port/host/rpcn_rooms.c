/* NP matching 2 rooms over RPCN (see rpcn_rooms.h).
 *
 * The game's request structures (PSP layouts as reconstructed by PPSSPP --
 * Komak57/ppsspp master, Core/HLE/Np2Types.h, checked against the requests
 * PSP2i sends) become RPCN messages (FlatBuffers, schema np2_structs.fbs of
 * RPCN protocol 27), and RPCN replies and notifications become the PSP
 * structures the game's callbacks read, written into guest memory from
 * psp_np2_alloc. The mapping follows PPSSPP's fb_helpers.cpp and
 * RPCNAgent.cpp.
 *
 * RPCN request payload: communication ID (12 bytes, "NPWR01446_00"), u32
 * message size, message. Reply payload: error byte, then per command (a
 * size-prefixed message, a room ID, or nothing). Notifications: per type. */

#include "rpcn_rooms.h"
#include "fbuf.h"
#include "online.h"

#include <psprecomp/hle.h>
#include <psprecomp/net.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RPCN CommandType (protocol 27) */
enum { CMD_CREATE_ROOM = 13, CMD_JOIN_ROOM = 14, CMD_LEAVE_ROOM = 15, CMD_SEARCH_ROOM = 16,
       CMD_GET_ROOM_DATA_EXTERNAL_LIST = 17, CMD_SET_ROOM_DATA_EXTERNAL = 18, CMD_SET_ROOM_DATA_INTERNAL = 20,
       CMD_SEND_ROOM_MESSAGE = 25 };
/* RPCN NotificationType */
enum { NT_USER_JOINED_ROOM = 0, NT_USER_LEFT_ROOM = 1, NT_ROOM_DESTROYED = 2, NT_UPDATED_ROOM_DATA_INTERNAL = 3,
       NT_UPDATED_ROOM_MEMBER_DATA_INTERNAL = 4, NT_ROOM_MESSAGE_RECEIVED = 9, NT_SIGNALING_HELPER = 12 };
/* RPCN ErrorType */
enum { E_NONE = 0, E_MALFORMED = 1, E_ROOM_MISSING = 14, E_ROOM_ALREADY_JOINED = 15, E_ROOM_FULL = 16,
       E_ROOM_PASSWORD_MISMATCH = 17 };

/* SCE codes */
#define M2_SERVER_ERROR_BAD_REQUEST          0x80550D01u
#define M2_SERVER_ERROR_SERVICE_UNAVAILABLE  0x80550D02u
#define M2_SERVER_ERROR_NO_SUCH_ROOM         0x80550D13u
#define M2_SERVER_ERROR_PASSWORD_MISMATCH    0x80550D17u
#define M2_SERVER_ERROR_ROOM_FULL            0x80550D19u
#define M2_SERVER_ERROR_ALREADY_JOINED       0x80550D30u
#define M2_ERROR_ROOM_NOT_FOUND              0x80550C35u
#define M2_ERROR_INVALID_ARGUMENT            0x80550C0Au
enum { ROOM_EVENT_MemberJoined = 0x1101, ROOM_EVENT_MemberLeft = 0x1102, ROOM_EVENT_RoomDestroyed = 0x1104,
       ROOM_EVENT_UpdatedRoomDataInternal = 0x1106, ROOM_EVENT_UpdatedRoomMemberDataInternal = 0x1107,
       ROOM_MSG_EVENT_Message = 0x2102 };
enum { CAST_BROADCAST = 1, CAST_UNICAST = 2, CAST_MULTICAST = 3, CAST_MULTICAST_TEAM = 4 };
/* bin attribute IDs: which list a room attribute belongs to */
enum { SEARCHABLE_BIN_EXT_1 = 0x54, BIN_EXT_1 = 0x55, BIN_EXT_2 = 0x56, BIN_INT_1 = 0x57, BIN_INT_2 = 0x58 };

static char g_self[20];
void rooms_set_self(const char *npid) { snprintf(g_self, sizeof g_self, "%s", npid ? npid : ""); }

#define LOG(...) online_log("rooms: " __VA_ARGS__)

static uint64_t rd64(uint32_t a) { return (uint64_t)psp_read32(a) | (uint64_t)psp_read32(a + 4) << 32; }
static void wr64(uint32_t a, uint64_t v) { psp_write32(a, (uint32_t)v); psp_write32(a + 4, (uint32_t)(v >> 32)); }

/* ==== the game's structures -> RPCN messages ============================================ */

/* guest bytes as a [ubyte] vector */
static fb_ref guest_bytes(fb_builder *b, uint32_t addr, uint32_t n) {
    if (!addr || !n || n > 0x10000) return fb_bytes(b, NULL, 0);
    uint8_t *tmp = (uint8_t *)malloc(n);
    if (!tmp) return 0;
    psp_mem_read_block(tmp, addr, n);
    const fb_ref r = fb_bytes(b, tmp, n);
    free(tmp);
    return r;
}

/* SceNpMatching2BinAttr {u16 id, pad, u8 *ptr, u32 size} -> BinAttr {id, data} */
static fb_ref bin_attr(fb_builder *b, uint32_t a) {
    const fb_ref data = guest_bytes(b, psp_read32(a + 4), psp_read32(a + 8));
    fb_table_start(b);
    fb_add_u16(b, 0, psp_read16(a));
    fb_add_ref(b, 1, data);
    return fb_table_end(b);
}

/* SceNpMatching2IntAttr {u16 id, pad, u32 num} -> IntAttr {id, num} */
static fb_ref int_attr(fb_builder *b, uint32_t a) {
    fb_table_start(b);
    fb_add_u16(b, 0, psp_read16(a));
    fb_add_u32(b, 1, psp_read32(a + 4));
    return fb_table_end(b);
}

static fb_ref bin_attr_list(fb_builder *b, uint32_t arr, uint32_t n) {
    if (!arr || !n || n > 64) return 0;
    fb_ref r[64];
    for (uint32_t i = 0; i < n; i++) r[i] = bin_attr(b, arr + 12 * i);
    return fb_vec_refs(b, r, n);
}

static fb_ref int_attr_list(fb_builder *b, uint32_t arr, uint32_t n) {
    if (!arr || !n || n > 64) return 0;
    fb_ref r[64];
    for (uint32_t i = 0; i < n; i++) r[i] = int_attr(b, arr + 8 * i);
    return fb_vec_refs(b, r, n);
}

/* Room bin attributes sorted by ID into internal / searchable external /
 * external, from any of the game's lists (as PPSSPP does: some games mix them). */
typedef struct { fb_ref internal[8], s_external[8], external[8]; uint32_t ni, ns, ne; } bin_sort;
static void sort_bins(fb_builder *b, bin_sort *s, uint32_t arr, uint32_t n) {
    for (uint32_t i = 0; arr && i < n && i < 32; i++) {
        const uint32_t a = arr + 12 * i;
        const uint16_t id = psp_read16(a);
        const fb_ref r = bin_attr(b, a);
        if ((id == BIN_INT_1 || id == BIN_INT_2) && s->ni < 8) s->internal[s->ni++] = r;
        else if ((id == BIN_EXT_1 || id == BIN_EXT_2) && s->ne < 8) s->external[s->ne++] = r;
        else if (id == SEARCHABLE_BIN_EXT_1 && s->ns < 8) s->s_external[s->ns++] = r;
        else LOG("unexpected room bin attribute id 0x%X: left out", id);
    }
}

/* SceNpMatching2PresenceOptionData {u8 data[16]; u32 length} -> PresenceOptionData {data, len} */
static fb_ref opt_data(fb_builder *b, uint32_t a) {
    const fb_ref d = guest_bytes(b, a, 16);
    fb_table_start(b);
    fb_add_ref(b, 0, d);
    fb_add_u32(b, 1, psp_read32(a + 16));
    return fb_table_end(b);
}

static int valid_npid(const char *s) {
    const size_t n = strlen(s);
    if (n < 3 || n > 16) return 0;
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

/* SceNpId array (36 bytes each) -> [string] */
static fb_ref npid_list(fb_builder *b, uint32_t arr, uint32_t n) {
    if (!arr || !n) return 0;
    fb_ref r[32];
    uint32_t k = 0;
    for (uint32_t i = 0; i < n && i < 100 && k < 32; i++) {
        char id[17];
        psp_str(arr + 36 * i, id, sizeof id);
        if (valid_npid(id)) r[k++] = fb_string(b, id);     /* some games pass uninitialised entries */
    }
    return k ? fb_vec_refs(b, r, k) : 0;
}

static fb_ref build_search_room(fb_builder *b, uint32_t p) {
    const uint32_t ifl = psp_read32(p + 0x20), nifl = psp_read32(p + 0x24);
    const uint32_t bfl = psp_read32(p + 0x28), nbfl = psp_read32(p + 0x2C);
    const uint32_t aid = psp_read32(p + 0x30), naid = psp_read32(p + 0x34);
    fb_ref intf = 0, binf = 0, attrs = 0;
    if (ifl && nifl && nifl <= 32) {
        fb_ref r[32];
        for (uint32_t i = 0; i < nifl; i++) {                 /* {u8 op, pad[3], IntAttr} 12 bytes */
            const uint32_t e = ifl + 12 * i;
            const fb_ref at = int_attr(b, e + 4);
            fb_table_start(b);
            fb_add_u8(b, 0, psp_read8(e));
            fb_add_ref(b, 1, at);
            r[i] = fb_table_end(b);
        }
        intf = fb_vec_refs(b, r, nifl);
    }
    if (bfl && nbfl && nbfl <= 32) {
        fb_ref r[32];
        for (uint32_t i = 0; i < nbfl; i++) {                 /* {u8 op, pad[3], BinAttr} 16 bytes */
            const uint32_t e = bfl + 16 * i;
            const fb_ref at = bin_attr(b, e + 4);
            fb_table_start(b);
            fb_add_u8(b, 0, psp_read8(e));
            fb_add_ref(b, 1, at);
            r[i] = fb_table_end(b);
        }
        binf = fb_vec_refs(b, r, nbfl);
    }
    if (aid && naid && naid <= 64) {
        uint16_t v[64];
        for (uint32_t i = 0; i < naid; i++) v[i] = psp_read16(aid + 2 * i);
        attrs = fb_vec_u16(b, v, naid);
    }
    LOG("SearchRoom: world %u, from %u, max %u, flag filter 0x%08X / attr 0x%08X, %u int filters, %u bin filters, %u attribute ids",
        psp_read32(p + 4), psp_read32(p + 0x10), psp_read32(p + 0x14), psp_read32(p + 0x18), psp_read32(p + 0x1C), nifl, nbfl, naid);
    fb_table_start(b);
    fb_add_u32(b, 0, psp_read32(p + 0));          /* option */
    fb_add_u32(b, 1, psp_read32(p + 4));          /* worldId */
    fb_add_u64(b, 2, rd64(p + 8));                /* lobbyId */
    fb_add_u32(b, 3, psp_read32(p + 0x10));       /* rangeFilter_startIndex */
    fb_add_u32(b, 4, psp_read32(p + 0x14));       /* rangeFilter_max */
    fb_add_u32(b, 5, psp_read32(p + 0x18));       /* flagFilter */
    fb_add_u32(b, 6, psp_read32(p + 0x1C));       /* flagAttr */
    fb_add_ref(b, 7, intf);
    fb_add_ref(b, 8, binf);
    fb_add_ref(b, 9, attrs);
    return fb_table_end(b);
}

static fb_ref build_create_join_room(fb_builder *b, uint32_t p) {
    bin_sort s;
    memset(&s, 0, sizeof s);
    sort_bins(b, &s, psp_read32(p + 0x18), psp_read32(p + 0x1C));   /* roomBinAttrInternal */
    sort_bins(b, &s, psp_read32(p + 0x28), psp_read32(p + 0x2C));   /* roomSearchableBinAttrExternal */
    sort_bins(b, &s, psp_read32(p + 0x30), psp_read32(p + 0x34));   /* roomBinAttrExternal */
    const fb_ref bin_int = s.ni ? fb_vec_refs(b, s.internal, s.ni) : 0;
    const fb_ref bin_sext = s.ns ? fb_vec_refs(b, s.s_external, s.ns) : 0;
    const fb_ref bin_ext = s.ne ? fb_vec_refs(b, s.external, s.ne) : 0;
    const fb_ref int_sext = int_attr_list(b, psp_read32(p + 0x20), psp_read32(p + 0x24));
    const uint32_t pw = psp_read32(p + 0x38), mask_p = psp_read32(p + 0x44);
    fb_ref password = 0;
    if (mask_p) {
        static const uint8_t DEFAULT_PW[8] = { 0x50, 0x77, 0x4E, 0x61, 0x4E, 0, 0, 0 };   /* as PPSSPP */
        password = pw ? guest_bytes(b, pw, 8) : fb_bytes(b, DEFAULT_PW, 8);
    }
    fb_ref groups = 0;
    const uint32_t gc = psp_read32(p + 0x3C), ngc = psp_read32(p + 0x40);
    if (gc && ngc && ngc <= 16) {
        fb_ref r[16];
        for (uint32_t i = 0; i < ngc; i++) {          /* {u32 slotNum, u8 withLabel, label[8], u8 withPassword, pad[2]} */
            const uint32_t e = gc + 16 * i;
            const fb_ref label = psp_read8(e + 4) ? guest_bytes(b, e + 5, 8) : 0;
            fb_table_start(b);
            fb_add_u32(b, 0, psp_read32(e));
            fb_add_ref(b, 1, label);
            fb_add_u8(b, 2, psp_read8(e + 13) != 0);
            r[i] = fb_table_end(b);
        }
        groups = fb_vec_refs(b, r, ngc);
    }
    const fb_ref allowed = npid_list(b, psp_read32(p + 0x48), psp_read32(p + 0x4C));
    const fb_ref blocked = npid_list(b, psp_read32(p + 0x50), psp_read32(p + 0x54));
    const uint32_t gl = psp_read32(p + 0x58);
    const fb_ref label = gl ? guest_bytes(b, gl, 8) : 0;
    const fb_ref member_bins = bin_attr_list(b, psp_read32(p + 0x5C), psp_read32(p + 0x60));
    fb_ref sig = 0;
    const uint32_t so = psp_read32(p + 0x68);
    if (so) {                                         /* {u8 type, u8 flag, u16 hubMemberId, reserved[4]} */
        fb_table_start(b);
        fb_add_u8(b, 0, psp_read8(so));
        fb_add_u8(b, 1, psp_read8(so + 1));
        fb_add_u16(b, 2, psp_read16(so + 2));
        sig = fb_table_end(b);
    }
    LOG("CreateJoinRoom: world %u, max %u slots, flags 0x%08X, %u internal / %u searchable / %u external bin attrs, "
        "%u int attrs, password %s, %u member bin attrs",
        psp_read32(p), psp_read32(p + 0x10), psp_read32(p + 0x14), s.ni, s.ns, s.ne, psp_read32(p + 0x24),
        mask_p ? "set" : "none", psp_read32(p + 0x60));
    fb_table_start(b);
    fb_add_u32(b, 0, psp_read32(p));              /* worldId */
    fb_add_u64(b, 1, rd64(p + 8));                /* lobbyId */
    fb_add_u32(b, 2, psp_read32(p + 0x10));       /* maxSlot */
    fb_add_u32(b, 3, psp_read32(p + 0x14));       /* flagAttr */
    fb_add_ref(b, 4, bin_int);
    fb_add_ref(b, 5, int_sext);
    fb_add_ref(b, 6, bin_sext);
    fb_add_ref(b, 7, bin_ext);
    fb_add_ref(b, 8, password);
    fb_add_ref(b, 9, groups);
    fb_add_u64(b, 10, mask_p ? rd64(mask_p) : 0); /* passwordSlotMask */
    fb_add_ref(b, 11, allowed);
    fb_add_ref(b, 12, blocked);
    fb_add_ref(b, 13, label);
    fb_add_ref(b, 14, member_bins);
    fb_add_u8(b, 15, psp_read8(p + 0x64));        /* teamId */
    fb_add_ref(b, 16, sig);
    return fb_table_end(b);
}

static fb_ref build_join_room(fb_builder *b, uint32_t p) {
    const uint32_t pw = psp_read32(p + 8), gl = psp_read32(p + 0xC);
    const fb_ref password = pw ? guest_bytes(b, pw, 8) : 0;
    const fb_ref label = gl ? guest_bytes(b, gl, 8) : 0;
    const fb_ref bins = bin_attr_list(b, psp_read32(p + 0x10), psp_read32(p + 0x14));
    const fb_ref opt = opt_data(b, p + 0x18);
    LOG("JoinRoom: room 0x%016llX, password %s", (unsigned long long)rd64(p), pw ? "given" : "none");
    fb_table_start(b);
    fb_add_u64(b, 0, rd64(p));
    fb_add_ref(b, 1, password);
    fb_add_ref(b, 2, label);
    fb_add_ref(b, 3, bins);
    fb_add_ref(b, 4, opt);
    fb_add_u8(b, 5, psp_read8(p + 0x2C));
    return fb_table_end(b);
}

static fb_ref build_leave_room(fb_builder *b, uint32_t p) {
    const fb_ref opt = opt_data(b, p + 8);
    LOG("LeaveRoom: room 0x%016llX", (unsigned long long)rd64(p));
    fb_table_start(b);
    fb_add_u64(b, 0, rd64(p));
    fb_add_ref(b, 1, opt);
    return fb_table_end(b);
}

static fb_ref build_get_room_data_external_list(fb_builder *b, uint32_t p) {
    const uint32_t ids = psp_read32(p), nids = psp_read32(p + 4), aid = psp_read32(p + 8), naid = psp_read32(p + 0xC);
    uint64_t r[64];
    uint16_t a[64];
    uint32_t n = 0, m = 0;
    for (; ids && n < nids && n < 64; n++) r[n] = rd64(ids + 8 * n);
    for (; aid && m < naid && m < 64; m++) a[m] = psp_read16(aid + 2 * m);
    const fb_ref rv = fb_vec_u64(b, r, n), av = fb_vec_u16(b, a, m);
    LOG("GetRoomDataExternalList: %u rooms, %u attribute ids", n, m);
    fb_table_start(b);
    fb_add_ref(b, 0, rv);
    fb_add_ref(b, 1, av);
    return fb_table_end(b);
}

static fb_ref build_set_room_data_external(fb_builder *b, uint32_t p) {
    bin_sort s;
    memset(&s, 0, sizeof s);
    sort_bins(b, &s, psp_read32(p + 0x10), psp_read32(p + 0x14));
    sort_bins(b, &s, psp_read32(p + 0x18), psp_read32(p + 0x1C));
    const fb_ref ints = int_attr_list(b, psp_read32(p + 8), psp_read32(p + 0xC));
    const fb_ref sext = s.ns ? fb_vec_refs(b, s.s_external, s.ns) : 0;
    const fb_ref ext = s.ne ? fb_vec_refs(b, s.external, s.ne) : 0;
    LOG("SetRoomDataExternal: room 0x%016llX, %u int attrs, %u searchable / %u external bin attrs",
        (unsigned long long)rd64(p), psp_read32(p + 0xC), s.ns, s.ne);
    fb_table_start(b);
    fb_add_u64(b, 0, rd64(p));
    fb_add_ref(b, 1, ints);
    fb_add_ref(b, 2, sext);
    fb_add_ref(b, 3, ext);
    return fb_table_end(b);
}

static fb_ref build_set_room_data_internal(fb_builder *b, uint32_t p) {
    const fb_ref bins = bin_attr_list(b, psp_read32(p + 0x10), psp_read32(p + 0x14));
    fb_ref pwc = 0;
    const uint32_t pc = psp_read32(p + 0x18), npc = psp_read32(p + 0x1C);
    if (pc && npc && npc <= 16) {
        fb_ref r[16];
        for (uint32_t i = 0; i < npc; i++) {          /* {u8 groupId, u8 withPassword, pad} */
            fb_table_start(b);
            fb_add_u8(b, 0, psp_read8(pc + 3 * i));
            fb_add_u8(b, 1, psp_read8(pc + 3 * i + 1) != 0);
            r[i] = fb_table_end(b);
        }
        pwc = fb_vec_refs(b, r, npc);
    }
    const uint32_t mp = psp_read32(p + 0x20);
    uint64_t mask = mp ? rd64(mp) : 0;
    const fb_ref maskv = mp ? fb_vec_u64(b, &mask, 1) : 0;
    fb_ref ranks = 0;
    const uint32_t rp = psp_read32(p + 0x24), nr = psp_read32(p + 0x28);
    if (rp && nr && nr <= 64) {
        uint16_t v[64];
        for (uint32_t i = 0; i < nr; i++) v[i] = psp_read16(rp + 2 * i);
        ranks = fb_vec_u16(b, v, nr);
    }
    LOG("SetRoomDataInternal: room 0x%016llX, flags 0x%08X/0x%08X, %u bin attrs",
        (unsigned long long)rd64(p), psp_read32(p + 8), psp_read32(p + 0xC), psp_read32(p + 0x14));
    fb_table_start(b);
    fb_add_u64(b, 0, rd64(p));
    fb_add_u32(b, 1, psp_read32(p + 8));
    fb_add_u32(b, 2, psp_read32(p + 0xC));
    fb_add_ref(b, 3, bins);
    fb_add_ref(b, 4, pwc);
    fb_add_ref(b, 5, maskv);
    fb_add_ref(b, 6, ranks);
    return fb_table_end(b);
}

static fb_ref build_send_room_message(fb_builder *b, uint32_t p) {
    const uint8_t cast = psp_read8(p + 8);
    uint16_t dst[32];
    uint32_t nd = 0;
    switch (cast) {
    case CAST_UNICAST: dst[nd++] = psp_read16(p + 0xC); break;
    case CAST_MULTICAST: {
        const uint32_t ids = psp_read32(p + 0xC), n = psp_read32(p + 0x10);
        for (; ids && nd < n && nd < 32; nd++) dst[nd] = psp_read16(ids + 2 * nd);
        break;
    }
    case CAST_MULTICAST_TEAM: dst[nd++] = psp_read8(p + 0xC); break;
    default: break;
    }
    const fb_ref dv = fb_vec_u16(b, dst, nd);
    const fb_ref msg = guest_bytes(b, psp_read32(p + 0x14), psp_read32(p + 0x18));
    LOG("SendRoomMessage: room 0x%016llX, cast %u (%u targets), %u bytes", (unsigned long long)rd64(p), cast, nd, psp_read32(p + 0x18));
    fb_table_start(b);
    fb_add_u64(b, 0, rd64(p));
    fb_add_u8(b, 1, cast);
    fb_add_ref(b, 2, dv);
    fb_add_ref(b, 3, msg);
    fb_add_u8(b, 4, (uint8_t)psp_read32(p + 0x1C));
    return fb_table_end(b);
}

int rooms_build(int kind, const char *com_id, uint32_t p, uint16_t *cmd, uint8_t **payload, uint32_t *len) {
    fb_builder b;
    fb_init(&b);
    fb_ref root = 0;
    switch (kind) {
    case PSP_M2_SEARCH_ROOM:                 *cmd = CMD_SEARCH_ROOM; root = build_search_room(&b, p); break;
    case PSP_M2_CREATE_JOIN_ROOM:            *cmd = CMD_CREATE_ROOM; root = build_create_join_room(&b, p); break;
    case PSP_M2_JOIN_ROOM:                   *cmd = CMD_JOIN_ROOM; root = build_join_room(&b, p); break;
    case PSP_M2_LEAVE_ROOM:                  *cmd = CMD_LEAVE_ROOM; root = build_leave_room(&b, p); break;
    case PSP_M2_GET_ROOM_DATA_EXTERNAL_LIST: *cmd = CMD_GET_ROOM_DATA_EXTERNAL_LIST; root = build_get_room_data_external_list(&b, p); break;
    case PSP_M2_SET_ROOM_DATA_EXTERNAL:      *cmd = CMD_SET_ROOM_DATA_EXTERNAL; root = build_set_room_data_external(&b, p); break;
    case PSP_M2_SET_ROOM_DATA_INTERNAL:      *cmd = CMD_SET_ROOM_DATA_INTERNAL; root = build_set_room_data_internal(&b, p); break;
    case PSP_M2_SEND_ROOM_MESSAGE:           *cmd = CMD_SEND_ROOM_MESSAGE; root = build_send_room_message(&b, p); break;
    default: fb_free(&b); return (int)M2_ERROR_INVALID_ARGUMENT;
    }
    uint32_t n = 0;
    const uint8_t *msg = fb_finish(&b, root, &n);
    if (!msg) { fb_free(&b); return (int)M2_SERVER_ERROR_SERVICE_UNAVAILABLE; }
    *len = 12 + 4 + n;
    *payload = (uint8_t *)malloc(*len);
    if (!*payload) { fb_free(&b); return (int)M2_SERVER_ERROR_SERVICE_UNAVAILABLE; }
    memset(*payload, 0, 12);
    memcpy(*payload, com_id, strlen(com_id) < 12 ? strlen(com_id) : 12);
    for (int i = 0; i < 4; i++) (*payload)[12 + i] = (uint8_t)(n >> (8 * i));
    memcpy(*payload + 16, msg, n);
    fb_free(&b);
    return 0;
}

/* ==== RPCN messages -> the game's structures ============================================ */

/* A size-prefixed message at *off in the payload. */
static int get_message(const uint8_t *p, uint32_t len, uint32_t *off, fb_table *t) {
    if (*off + 4 > len) return -1;
    const uint32_t n = (uint32_t)p[*off] | (uint32_t)p[*off + 1] << 8 | (uint32_t)p[*off + 2] << 16 | (uint32_t)p[*off + 3] << 24;
    if (*off + 4 + n > len) return -1;
    *t = fb_root(p + *off + 4, n);
    *off += 4 + n;
    return t->pos ? 0 : -1;
}
static uint64_t get_u64(const uint8_t *p, uint32_t len, uint32_t *off) {
    uint64_t v = 0;
    for (int i = 0; i < 8 && *off + i < len; i++) v |= (uint64_t)p[*off + i] << (8 * i);
    *off += 8;
    return v;
}

/* [ubyte] vector -> freshly allocated guest bytes */
static uint32_t put_bytes(fb_vec v) {
    if (!v.pos || !v.n) return 0;
    const uint32_t a = psp_np2_alloc(v.n);
    if (a) for (uint32_t i = 0; i < v.n; i++) psp_write8(a + i, fb_vec_u8_at(v, i));
    return a;
}

/* BinAttr {id, data} -> SceNpMatching2BinAttr at a */
static void put_bin_attr(uint32_t a, fb_table t) {
    const fb_vec d = fb_vec_of(t, 1);
    psp_write16(a, fb_u16(t, 0));
    psp_write32(a + 4, put_bytes(d));
    psp_write32(a + 8, d.n);
}

/* UserInfo {npId, onlineName, avatarUrl} -> SceNpUserInfo2 {SceNpId npId (36), onlineName *, avatarUrl *} at a */
static void put_user_info(uint32_t a, fb_table u) {
    char s[130];
    if (fb_str(u, 0, s, 17) >= 0) for (int i = 0; i < 16 && s[i]; i++) psp_write8(a + (uint32_t)i, (uint8_t)s[i]);
    if (fb_str(u, 1, s, 49) >= 0) {                   /* SceNpOnlineName: data[49], pad[3] */
        const uint32_t n = psp_np2_alloc(52);
        if (n) { for (int i = 0; s[i]; i++) psp_write8(n + (uint32_t)i, (uint8_t)s[i]); psp_write32(a + 36, n); }
    }
    if (fb_str(u, 2, s, 128) >= 0) {                  /* SceNpAvatarUrl: data[127], term */
        const uint32_t n = psp_np2_alloc(128);
        if (n) { for (int i = 0; s[i]; i++) psp_write8(n + (uint32_t)i, (uint8_t)s[i]); psp_write32(a + 40, n); }
    }
}

/* RoomGroup -> SceNpMatching2RoomGroup (20 bytes) */
static void put_room_group(uint32_t a, fb_table g) {
    const fb_vec label = fb_vec_of(g, 2);
    psp_write8(a, fb_u8(g, 0));
    psp_write8(a + 1, fb_u8(g, 1));
    psp_write8(a + 2, label.pos ? 1 : 0);
    for (uint32_t i = 0; i < label.n && i < 8; i++) psp_write8(a + 4 + i, fb_vec_u8_at(label, i));
    psp_write32(a + 12, fb_u32(g, 3));
    psp_write32(a + 16, fb_u32(g, 4));
}

static uint32_t put_room_groups(fb_vec v) {
    if (!v.n) return 0;
    const uint32_t a = psp_np2_alloc(20 * v.n);
    for (uint32_t i = 0; a && i < v.n; i++) put_room_group(a + 20 * i, fb_vec_table_at(v, i));
    return a;
}

/* RoomMemberDataInternal -> SceNpMatching2RoomMemberDataInternal (80 bytes):
 * next, userInfo (44), joinDate, memberId, teamId, roomGroup *, natType,
 * flagAttr, roomMemberBinAttrInternal *, count. groups: the room's group
 * array, if the room is known (the member's group points into it). */
static void put_member(uint32_t a, fb_table m, uint32_t groups) {
    put_user_info(a + 4, fb_sub(m, 0));
    wr64(a + 48, fb_u64(m, 1));
    psp_write16(a + 56, fb_u16(m, 2));
    psp_write8(a + 58, fb_u8(m, 3));
    const fb_table g = fb_sub(m, 4);
    if (g.pos) {
        const uint8_t gid = fb_u8(g, 0);
        if (groups && gid) psp_write32(a + 60, groups + 20u * (gid - 1u));
        else { const uint32_t ga = psp_np2_alloc(20); if (ga) { put_room_group(ga, g); psp_write32(a + 60, ga); } }
    }
    psp_write8(a + 64, fb_u8(m, 5));
    psp_write32(a + 68, fb_u32(m, 6));
    const fb_vec bins = fb_vec_of(m, 7);
    if (bins.n) {                                     /* RoomMemberBinAttrInternal: u64 updateDate, BinAttr, pad[4] (24) */
        const uint32_t ba = psp_np2_alloc(24 * bins.n);
        for (uint32_t i = 0; ba && i < bins.n; i++) {
            const fb_table e = fb_vec_table_at(bins, i);
            wr64(ba + 24 * i, fb_u64(e, 0));
            put_bin_attr(ba + 24 * i + 8, fb_sub(e, 1));
        }
        psp_write32(a + 72, ba);
        psp_write32(a + 76, ba ? bins.n : 0);
    }
}

static int is_self(uint32_t member) {
    char id[17];
    psp_str(member + 4, id, sizeof id);
    return g_self[0] && !strcmp(id, g_self);
}

/* RoomDataInternal -> SceNpMatching2RoomDataInternal (72 bytes) at a. */
static void put_room_internal(uint32_t a, fb_table r) {
    psp_write16(a, fb_u16(r, 0));
    psp_write32(a + 4, fb_u32(r, 1));
    wr64(a + 8, fb_u64(r, 2));
    wr64(a + 16, fb_u64(r, 3));
    wr64(a + 24, fb_u64(r, 4));
    psp_write32(a + 32, fb_u32(r, 5));
    const uint32_t groups = put_room_groups(fb_vec_of(r, 8));
    const fb_vec gv = fb_vec_of(r, 8);
    psp_write32(a + 52, groups);
    psp_write32(a + 56, groups ? gv.n : 0);
    const fb_vec members = fb_vec_of(r, 6);
    const uint16_t owner_id = fb_u16(r, 7);
    if (members.n) {
        const uint32_t ma = psp_np2_alloc(80 * members.n);
        for (uint32_t i = 0; ma && i < members.n; i++) {
            const uint32_t m = ma + 80 * i;
            if (i + 1 < members.n) psp_write32(m, m + 80);
            put_member(m, fb_vec_table_at(members, i), groups);
            if (is_self(m)) psp_write32(a + 44, m);                     /* memberList.me */
            if (psp_read16(m + 56) == owner_id) psp_write32(a + 48, m);  /* memberList.owner */
        }
        psp_write32(a + 36, ma);
        psp_write32(a + 40, ma ? members.n : 0);
    }
    psp_write32(a + 60, fb_u32(r, 9));
    const fb_vec bins = fb_vec_of(r, 10);
    if (bins.n) {                                     /* RoomBinAttrInternal: u64 updateDate, u16 updateMemberId, pad, BinAttr (24) */
        const uint32_t ba = psp_np2_alloc(24 * bins.n);
        for (uint32_t i = 0; ba && i < bins.n; i++) {
            const fb_table e = fb_vec_table_at(bins, i);
            wr64(ba + 24 * i, fb_u64(e, 0));
            psp_write16(ba + 24 * i + 8, fb_u16(e, 1));
            put_bin_attr(ba + 24 * i + 12, fb_sub(e, 2));
        }
        psp_write32(a + 64, ba);
        psp_write32(a + 68, ba ? bins.n : 0);
    }
    char me[17] = "";
    if (psp_read32(a + 44)) psp_str(psp_read32(a + 44) + 4, me, sizeof me);
    LOG("  room 0x%016llX: server %u, world %u, %u/%u members, owner member %u, me %s, flags 0x%08X",
        (unsigned long long)fb_u64(r, 3), fb_u16(r, 0), fb_u32(r, 1), members.n, fb_u32(r, 5), owner_id,
        me[0] ? me : "(not found)", fb_u32(r, 9));
}

static uint32_t new_room_internal(fb_table r) {
    const uint32_t a = psp_np2_alloc(72);
    if (a) put_room_internal(a, r);
    return a;
}

/* RoomDataExternal -> SceNpMatching2RoomDataExternal (88 bytes) at a. */
static void put_room_external(uint32_t a, fb_table r) {
    psp_write16(a + 4, fb_u16(r, 0));
    psp_write32(a + 8, fb_u32(r, 1));
    psp_write16(a + 12, fb_u16(r, 2));
    psp_write16(a + 14, fb_u16(r, 3));
    wr64(a + 16, fb_u64(r, 4));
    wr64(a + 24, fb_u64(r, 5));
    psp_write32(a + 32, fb_u16(r, 7));                /* maxSlot */
    psp_write32(a + 36, fb_u16(r, 9));                /* curMemberNum */
    wr64(a + 40, fb_u64(r, 10));
    const fb_table owner = fb_sub(r, 11);
    if (owner.pos) {
        const uint32_t o = psp_np2_alloc(44);
        if (o) { put_user_info(o, owner); psp_write32(a + 48, o); }
    }
    const fb_vec gv = fb_vec_of(r, 12);
    const uint32_t groups = put_room_groups(gv);
    psp_write32(a + 52, groups);
    psp_write32(a + 56, groups ? gv.n : 0);
    psp_write32(a + 60, fb_u32(r, 13));
    const fb_vec iv = fb_vec_of(r, 14);
    if (iv.n) {
        const uint32_t ia = psp_np2_alloc(8 * iv.n);
        for (uint32_t i = 0; ia && i < iv.n; i++) {
            const fb_table e = fb_vec_table_at(iv, i);
            psp_write16(ia + 8 * i, fb_u16(e, 0));
            psp_write32(ia + 8 * i + 4, fb_u32(e, 1));
        }
        psp_write32(a + 64, ia);
        psp_write32(a + 68, ia ? iv.n : 0);
    }
    for (int k = 0; k < 2; k++) {                     /* searchable external, external */
        const fb_vec bv = fb_vec_of(r, 15 + k);
        if (!bv.n) continue;
        const uint32_t ba = psp_np2_alloc(12 * bv.n);
        for (uint32_t i = 0; ba && i < bv.n; i++) put_bin_attr(ba + 12 * i, fb_vec_table_at(bv, i));
        psp_write32(a + 72 + 8u * (uint32_t)k, ba);
        psp_write32(a + 76 + 8u * (uint32_t)k, ba ? bv.n : 0);
    }
    char on[17] = "";
    if (owner.pos) fb_str(owner, 0, on, sizeof on);
    LOG("  room 0x%016llX: world %u, %u/%u members, owner %s, flags 0x%08X",
        (unsigned long long)fb_u64(r, 5), fb_u32(r, 1), fb_u16(r, 9), fb_u16(r, 7), on, fb_u32(r, 13));
}

/* A list of external rooms, linked by next. */
static uint32_t put_room_externals(fb_vec v) {
    if (!v.n) return 0;
    const uint32_t a = psp_np2_alloc(88 * v.n);
    for (uint32_t i = 0; a && i < v.n; i++) {
        if (i + 1 < v.n) psp_write32(a + 88 * i, a + 88 * (i + 1));
        put_room_external(a + 88 * i, fb_vec_table_at(v, i));
    }
    return a;
}

/* PresenceOptionData -> SceNpMatching2PresenceOptionData at a */
static void put_opt_data(uint32_t a, fb_table o) {
    if (!o.pos) return;
    const fb_vec d = fb_vec_of(o, 0);
    for (uint32_t i = 0; i < d.n && i < 16; i++) psp_write8(a + i, fb_vec_u8_at(d, i));
    psp_write32(a + 16, d.n);
}

static uint32_t map_error(int kind, uint8_t e) {
    switch (e) {
    case E_MALFORMED: return M2_SERVER_ERROR_BAD_REQUEST;
    case E_ROOM_MISSING: return kind == PSP_M2_JOIN_ROOM ? M2_ERROR_ROOM_NOT_FOUND : M2_SERVER_ERROR_NO_SUCH_ROOM;
    case E_ROOM_ALREADY_JOINED: return M2_SERVER_ERROR_ALREADY_JOINED;
    case E_ROOM_FULL: return M2_SERVER_ERROR_ROOM_FULL;
    case E_ROOM_PASSWORD_MISMATCH: return M2_SERVER_ERROR_PASSWORD_MISMATCH;
    default: return M2_SERVER_ERROR_SERVICE_UNAVAILABLE;
    }
}

static const char *kind_name(int kind) {
    switch (kind) {
    case PSP_M2_SEARCH_ROOM: return "SearchRoom";
    case PSP_M2_CREATE_JOIN_ROOM: return "CreateJoinRoom";
    case PSP_M2_JOIN_ROOM: return "JoinRoom";
    case PSP_M2_LEAVE_ROOM: return "LeaveRoom";
    case PSP_M2_GET_ROOM_DATA_EXTERNAL_LIST: return "GetRoomDataExternalList";
    case PSP_M2_SET_ROOM_DATA_EXTERNAL: return "SetRoomDataExternal";
    case PSP_M2_SET_ROOM_DATA_INTERNAL: return "SetRoomDataInternal";
    case PSP_M2_SEND_ROOM_MESSAGE: return "SendRoomMessage";
    default: return "?";
    }
}

void rooms_failed(int kind, uint32_t req_id) {
    LOG("%s (request %u): not sent / connection lost -> service unavailable", kind_name(kind), req_id);
    psp_np2_request_done(req_id, M2_SERVER_ERROR_SERVICE_UNAVAILABLE, 0);
}

void rooms_reply(int kind, uint32_t req_id, const uint8_t *p, uint32_t len) {
    const uint8_t err = len ? p[0] : 0xFF;
    if (err != E_NONE) {
        const uint32_t e = len ? map_error(kind, err) : M2_SERVER_ERROR_SERVICE_UNAVAILABLE;
        LOG("%s (request %u): the server refused: RPCN error %u -> 0x%08X", kind_name(kind), req_id, err, e);
        psp_np2_request_done(req_id, e, 0);
        return;
    }
    uint32_t off = 1;
    fb_table t;
    switch (kind) {
    case PSP_M2_SEARCH_ROOM: {                        /* SearchRoomResponse {startIndex, total, rooms} */
        if (get_message(p, len, &off, &t)) break;
        const fb_vec rooms = fb_vec_of(t, 2);
        LOG("SearchRoom (request %u): %u rooms (total %u, from %u)", req_id, rooms.n, fb_u32(t, 1), fb_u32(t, 0));
        const uint32_t a = psp_np2_alloc(16);      /* {Range {startIndex, total, size}, RoomDataExternal *} */
        if (!a) break;
        psp_write32(a, fb_u32(t, 0));
        psp_write32(a + 4, fb_u32(t, 1));
        psp_write32(a + 8, rooms.n);
        psp_write32(a + 12, put_room_externals(rooms));
        psp_np2_request_done(req_id, 0, a);
        return;
    }
    case PSP_M2_CREATE_JOIN_ROOM: {                   /* RoomDataInternal */
        if (get_message(p, len, &off, &t)) break;
        LOG("CreateJoinRoom (request %u): room created", req_id);
        const uint32_t a = psp_np2_alloc(4);
        if (!a) break;
        psp_write32(a, new_room_internal(t));
        psp_np2_request_done(req_id, 0, a);
        return;
    }
    case PSP_M2_JOIN_ROOM: {                          /* JoinRoomResponse {room_data, signaling_data} */
        if (get_message(p, len, &off, &t)) break;
        LOG("JoinRoom (request %u): joined", req_id);
        const fb_vec sig = fb_vec_of(t, 1);
        if (sig.n) LOG("  %u members to connect to directly (player-to-player signaling is not implemented yet)", sig.n);
        const uint32_t a = psp_np2_alloc(4);
        if (!a) break;
        psp_write32(a, new_room_internal(fb_sub(t, 0)));
        psp_np2_request_done(req_id, 0, a);
        return;
    }
    case PSP_M2_LEAVE_ROOM:
        LOG("LeaveRoom (request %u): left room 0x%016llX", req_id, (unsigned long long)get_u64(p, len, &off));
        psp_np2_request_done(req_id, 0, 0);
        return;
    case PSP_M2_GET_ROOM_DATA_EXTERNAL_LIST: {        /* GetRoomDataExternalListResponse {rooms} */
        if (get_message(p, len, &off, &t)) break;
        const fb_vec rooms = fb_vec_of(t, 0);
        LOG("GetRoomDataExternalList (request %u): %u rooms", req_id, rooms.n);
        const uint32_t a = psp_np2_alloc(8);
        if (!a) break;
        psp_write32(a, put_room_externals(rooms));
        psp_write32(a + 4, rooms.n);
        psp_np2_request_done(req_id, 0, a);
        return;
    }
    default:                                          /* set data, send message: nothing back */
        LOG("%s (request %u): done", kind_name(kind), req_id);
        psp_np2_request_done(req_id, 0, 0);
        return;
    }
    LOG("%s (request %u): malformed reply (%u bytes) -> bad request", kind_name(kind), req_id, len);
    psp_np2_request_done(req_id, M2_SERVER_ERROR_BAD_REQUEST, 0);
}

/* RoomMemberUpdateInfo -> SceNpMatching2RoomMemberUpdateInfo (28): member *, eventCause, pad, optData */
static uint32_t new_member_update(fb_table u, uint16_t *member_id) {
    const uint32_t a = psp_np2_alloc(28);
    if (!a) return 0;
    const fb_table m = fb_sub(u, 0);
    if (m.pos) {
        const uint32_t ma = psp_np2_alloc(80);
        if (ma) { put_member(ma, m, 0); psp_write32(a, ma); *member_id = psp_read16(ma + 56); }
    }
    psp_write8(a + 4, fb_u8(u, 1));
    put_opt_data(a + 8, fb_sub(u, 2));
    return a;
}

void rooms_notification(uint16_t type, const uint8_t *p, uint32_t len) {
    uint32_t off = 0;
    fb_table t;
    uint16_t member = 0;
    switch (type) {
    case NT_USER_JOINED_ROOM: {                       /* NotificationUserJoinedRoom {room_id, update_info, signaling} */
        if (get_message(p, len, &off, &t)) break;
        const uint64_t room = fb_u64(t, 0);
        const uint32_t d = new_member_update(fb_sub(t, 1), &member);
        char who[17] = "";
        if (d && psp_read32(d)) psp_str(psp_read32(d) + 4, who, sizeof who);
        LOG("notification: %s (member %u) joined room 0x%016llX%s", who, member, (unsigned long long)room,
            fb_has(t, 2) ? " (player-to-player signaling is not implemented yet)" : "");
        psp_np2_room_event(room, member, ROOM_EVENT_MemberJoined, d);
        return;
    }
    case NT_USER_LEFT_ROOM: {                         /* u64 room, RoomMemberUpdateInfo */
        const uint64_t room = get_u64(p, len, &off);
        if (get_message(p, len, &off, &t)) break;
        const uint32_t d = new_member_update(t, &member);
        LOG("notification: member %u left room 0x%016llX", member, (unsigned long long)room);
        psp_np2_room_event(room, member, ROOM_EVENT_MemberLeft, d);
        return;
    }
    case NT_ROOM_DESTROYED: {                         /* u64 room, RoomUpdateInfo {eventCause, errorCode, optData} */
        const uint64_t room = get_u64(p, len, &off);
        if (get_message(p, len, &off, &t)) break;
        const uint32_t d = psp_np2_alloc(28);
        if (d) { psp_write8(d, fb_u8(t, 0)); psp_write32(d + 4, fb_u32(t, 1)); put_opt_data(d + 8, fb_sub(t, 2)); }
        LOG("notification: room 0x%016llX destroyed", (unsigned long long)room);
        psp_np2_room_event(room, 0, ROOM_EVENT_RoomDestroyed, d);
        return;
    }
    case NT_UPDATED_ROOM_DATA_INTERNAL: {             /* u64 room, RoomDataInternalUpdateInfo */
        const uint64_t room = get_u64(p, len, &off);
        if (get_message(p, len, &off, &t)) break;
        const uint32_t d = psp_np2_alloc(36);       /* new room *, newFlag *, prevFlag *, newMask *, prevMask *, groups *, n, bins **, n */
        if (!d) return;
        const uint32_t r = new_room_internal(fb_sub(t, 0));
        psp_write32(d, r);
        if (r && psp_read32(r + 60) != fb_u32(t, 1)) {
            const uint32_t pf = psp_np2_alloc(4);
            psp_write32(d + 4, r + 60);
            if (pf) { psp_write32(pf, fb_u32(t, 1)); psp_write32(d + 8, pf); }
        }
        if (r && rd64(r + 24) != fb_u64(t, 2)) {
            const uint32_t pm = psp_np2_alloc(8);
            psp_write32(d + 12, r + 24);
            if (pm) { wr64(pm, fb_u64(t, 2)); psp_write32(d + 16, pm); }
        }
        const fb_vec nb = fb_vec_of(t, 4);              /* IDs of the bin attributes that changed */
        if (r && nb.n) {
            const uint32_t arr = psp_np2_alloc(4 * nb.n), bins = psp_read32(r + 64), nbins = psp_read32(r + 68);
            for (uint32_t i = 0; arr && i < nb.n; i++)
                for (uint32_t j = 0; j < nbins; j++)
                    if (psp_read16(bins + 24 * j + 12) == fb_vec_u16_at(nb, i)) psp_write32(arr + 4 * i, bins + 24 * j);
            psp_write32(d + 28, arr);
            psp_write32(d + 32, arr ? nb.n : 0);
        }
        member = r && psp_read32(r + 48) ? psp_read16(psp_read32(r + 48) + 56) : 0;
        LOG("notification: room 0x%016llX data updated", (unsigned long long)room);
        psp_np2_room_event(room, member, ROOM_EVENT_UpdatedRoomDataInternal, d);
        return;
    }
    case NT_UPDATED_ROOM_MEMBER_DATA_INTERNAL: {      /* u64 room, RoomMemberDataInternalUpdateInfo */
        const uint64_t room = get_u64(p, len, &off);
        if (get_message(p, len, &off, &t)) break;
        const uint32_t d = psp_np2_alloc(24);       /* new member *, newFlag *, prevFlag *, newTeam *, bins **, n */
        if (!d) return;
        const uint32_t m = psp_np2_alloc(80);
        if (m) {
            put_member(m, fb_sub(t, 0), 0);
            psp_write32(d, m);
            member = psp_read16(m + 56);
            if (psp_read32(m + 68) != fb_u32(t, 1)) {
                const uint32_t pf = psp_np2_alloc(4);
                psp_write32(d + 4, m + 68);
                if (pf) { psp_write32(pf, fb_u32(t, 1)); psp_write32(d + 8, pf); }
            }
            if (psp_read8(m + 58) != fb_u8(t, 2)) psp_write32(d + 12, m + 58);
            const fb_vec nb = fb_vec_of(t, 3);
            if (nb.n) {
                const uint32_t arr = psp_np2_alloc(4 * nb.n), bins = psp_read32(m + 72), nbins = psp_read32(m + 76);
                for (uint32_t i = 0; arr && i < nb.n; i++)
                    for (uint32_t j = 0; j < nbins; j++)
                        if (psp_read16(bins + 24 * j + 8) == fb_vec_u16_at(nb, i)) psp_write32(arr + 4 * i, bins + 24 * j);
                psp_write32(d + 16, arr);
                psp_write32(d + 20, arr ? nb.n : 0);
            }
        }
        LOG("notification: member %u of room 0x%016llX updated", member, (unsigned long long)room);
        psp_np2_room_event(room, member, ROOM_EVENT_UpdatedRoomMemberDataInternal, d);
        return;
    }
    case NT_ROOM_MESSAGE_RECEIVED: {                  /* u64 room, u16 member, RoomMessageInfo */
        const uint64_t room = get_u64(p, len, &off);
        member = (uint16_t)(off + 2 <= len ? p[off] | p[off + 1] << 8 : 0);
        off += 2;
        if (get_message(p, len, &off, &t)) break;
        const uint32_t d = psp_np2_alloc(20);       /* filtered, castType, pad, dst *, srcMember *, msg *, msgLen */
        if (!d) return;
        const uint8_t cast = fb_u8(t, 1);
        const fb_vec dst = fb_vec_of(t, 2);
        psp_write8(d, fb_u8(t, 0));
        psp_write8(d + 1, cast);
        if (cast != CAST_BROADCAST) {
            const uint32_t da = psp_np2_alloc(8);
            if (da) {
                if (cast == CAST_MULTICAST) {
                    const uint32_t ids = psp_np2_alloc(2 * (dst.n ? dst.n : 1));
                    for (uint32_t i = 0; ids && i < dst.n; i++) psp_write16(ids + 2 * i, fb_vec_u16_at(dst, i));
                    psp_write32(da, ids);
                    psp_write32(da + 4, dst.n);
                } else if (cast == CAST_MULTICAST_TEAM) psp_write8(da, (uint8_t)fb_vec_u16_at(dst, 0));
                else psp_write16(da, fb_vec_u16_at(dst, 0));
                psp_write32(d + 4, da);
            }
        }
        const fb_table src = fb_sub(t, 3);
        if (src.pos) { const uint32_t s = psp_np2_alloc(44); if (s) { put_user_info(s, src); psp_write32(d + 8, s); } }
        const fb_vec msg = fb_vec_of(t, 4);
        psp_write32(d + 12, put_bytes(msg));
        psp_write32(d + 16, msg.n);
        LOG("notification: room message from member %u (%u bytes, cast %u)", member, msg.n, cast);
        psp_np2_room_message(room, member, ROOM_MSG_EVENT_Message, d);
        return;
    }
    case NT_SIGNALING_HELPER:
        LOG("notification: signaling helper (player-to-player signaling is not implemented yet)");
        return;
    default:
        LOG("notification type %u (%u bytes): not used here", type, len);
        return;
    }
    LOG("notification type %u: malformed (%u bytes)", type, len);
}
