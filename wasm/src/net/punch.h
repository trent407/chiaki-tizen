// chiaki-tizen: PSN candidate-check hole-punch (protocol-accurate).
//
// Mirrors chiaki-ng holepunch.c check_candidates() + send_response_ps(). The
// punch is BIDIRECTIONAL: we send 88-byte REQUESTs to the console's candidates
// AND answer the console's REQUESTs with RESPONSEs, until a candidate's path is
// confirmed. Packet layout (88 bytes):
//   0x00  uint32  MSG type (REQ 0x06000000 / RESP 0x07000000), big-endian
//   0x04  20B     hashed_id_local  (random per session; zero-padded to 0x24)
//   0x24  20B     hashed_id_console (from the console's answer message)
//   0x44  uint16  sid_local  (big-endian)
//   0x46  uint16  sid_console (big-endian)
//   0x4b  5B      request id (echoed)
//   RESPONSE only:
//   0x50  4B      sid_local XOR peer_ipv4
//   0x54  2B      sid_local XOR peer_port

#ifndef CHIAKI_TIZEN_PUNCH_H
#define CHIAKI_TIZEN_PUNCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUNCH_PKT_SIZE     88
#define PUNCH_MSG_REQ      0x06000000u
#define PUNCH_MSG_RESP     0x07000000u
#define PUNCH_HASH_SIZE    20   // chiaki hashed_id_local/console are 20 bytes
#define PUNCH_REQID_SIZE   5

#define PUNCH_OFF_TYPE         0x00
#define PUNCH_OFF_HASH_LOCAL   0x04
#define PUNCH_OFF_HASH_CONSOLE 0x24
#define PUNCH_OFF_SID_LOCAL    0x44
#define PUNCH_OFF_SID_CONSOLE  0x46
#define PUNCH_OFF_REQID        0x4b
#define PUNCH_OFF_XOR_ADDR     0x50
#define PUNCH_OFF_XOR_PORT     0x54

typedef enum {
	PUNCH_OK = 0,
	PUNCH_ERR_ARG = -1,
	PUNCH_ERR_SOCKET = -2,
	PUNCH_ERR_TIMEOUT = -3,
	PUNCH_ERR_IO = -4,
} punch_status;

typedef struct {
	const char *ip;     // dotted IPv4
	uint16_t port;
} punch_candidate;

// Session values carried in every packet. hashed ids are 20 bytes.
typedef struct {
	uint8_t hashed_id_local[PUNCH_HASH_SIZE];
	uint8_t hashed_id_console[PUNCH_HASH_SIZE];
	uint16_t sid_local;
	uint16_t sid_console;
} punch_ids;

// Build an 88-byte REQUEST with the given request id.
void punch_build_request(uint8_t buf[PUNCH_PKT_SIZE], const punch_ids *ids,
	const uint8_t req_id[PUNCH_REQID_SIZE]);

// Build the 88-byte RESPONSE to a console request: echoes their req id and
// encodes the peer (candidate) address/port XOR'd with sid_local.
void punch_build_response(uint8_t buf[PUNCH_PKT_SIZE], const punch_ids *ids,
	const uint8_t their_req_id[PUNCH_REQID_SIZE],
	const char *peer_ipv4, uint16_t peer_port);

// True if buf is a RESPONSE echoing our req_id.
int punch_is_response_for(const uint8_t *buf, size_t len, const uint8_t req_id[PUNCH_REQID_SIZE]);
// True if buf is a REQUEST (the console probing us).
int punch_is_request(const uint8_t *buf, size_t len);

// Run the bidirectional punch on an (unconnected) UDP socket: send our REQUEST
// to every candidate, answer any inbound console REQUEST with a RESPONSE, and
// finish when a candidate returns a RESPONSE to our request. On PUNCH_OK,
// *selected is the winning candidate index.
punch_status punch_run(int fd, const punch_candidate *candidates, size_t n,
	const punch_ids *ids, int timeout_ms, int tries, int *selected);

const char *punch_strerror(punch_status s);

#ifdef __cplusplus
}
#endif
#endif // CHIAKI_TIZEN_PUNCH_H
