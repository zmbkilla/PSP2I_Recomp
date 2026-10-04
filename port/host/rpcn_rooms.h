/* NP matching 2 rooms over RPCN: the game's request structures to RPCN
 * messages, RPCN replies and notifications back to the game. See
 * rpcn_rooms.c. Game thread only (it reads and writes guest memory). */
#ifndef PSP2I_RPCN_ROOMS_H
#define PSP2I_RPCN_ROOMS_H

#include <stdint.h>

/* Build the RPCN request for a PSP_M2_* room request whose parameters are
 * at guest address `param`: *cmd and a malloc'd payload (communication ID,
 * then the size-prefixed message). 0, or an SCE error to report. */
int  rooms_build(int kind, const char *com_id, uint32_t param, uint16_t *cmd, uint8_t **payload, uint32_t *len);
/* A reply (payload: RPCN error byte, then the message) to request req_id:
 * converted and handed to the game (psp_np2_request_done). */
void rooms_reply(int kind, uint32_t req_id, const uint8_t *payload, uint32_t len);
/* The request could not be sent / the connection was lost. */
void rooms_failed(int kind, uint32_t req_id);
/* An RPCN notification (room events and messages) for the game. */
void rooms_notification(uint16_t type, const uint8_t *payload, uint32_t len);
/* Our NP ID (the RPCN user name): which room member is "me". */
void rooms_set_self(const char *npid);

#endif
