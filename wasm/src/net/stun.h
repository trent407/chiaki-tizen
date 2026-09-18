// chiaki-tizen spike: minimal STUN (RFC 5389) client.
//
// Purpose: discover our public IP:port (server-reflexive mapping) so it can be
// offered as a candidate during PSN hole-punching, and — on Tizen — verify the
// Sockets Extension preserves the UDP source port across sends (hole punching
// depends on it). Pure UDP, no TLS, no dependencies.
//
// Transport-agnostic: the query uses POSIX UDP so it runs natively for
// verification; on Tizen the same message build/parse runs, with the socket
// created via the Tizen Sockets Extension.

#ifndef CHIAKI_TIZEN_STUN_H
#define CHIAKI_TIZEN_STUN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STUN_MAGIC_COOKIE 0x2112A442u
#define STUN_HEADER_SIZE 20
#define STUN_TXID_SIZE 12

typedef enum {
	STUN_OK = 0,
	STUN_ERR_ARG = -1,
	STUN_ERR_SOCKET = -2,
	STUN_ERR_IO = -3,
	STUN_ERR_TIMEOUT = -4,
	STUN_ERR_PARSE = -5,     // not a valid Binding Response for our txid
	STUN_ERR_NO_MAPPING = -6 // response had no (XOR-)MAPPED-ADDRESS
} stun_status;

// Build a 20-byte Binding Request into buf and fill txid with a fresh random id.
void stun_build_request(uint8_t buf[STUN_HEADER_SIZE], uint8_t txid[STUN_TXID_SIZE]);

// Parse a Binding Response: verify it matches txid, then extract the mapped
// IPv4 address (prefers XOR-MAPPED-ADDRESS, falls back to MAPPED-ADDRESS).
// ip_out receives a dotted-quad string. Returns STUN_OK on success.
stun_status stun_parse_response(const uint8_t *resp, size_t len,
	const uint8_t txid[STUN_TXID_SIZE],
	char *ip_out, size_t ip_size, uint16_t *port_out);

// Full round trip over UDP: resolve host:port, send a Binding Request, receive
// and parse the response. On success reports the public mapping (ip_out/
// port_out) and the local source port the OS assigned (local_port_out) — the
// latter is what hole punching relies on staying stable.
stun_status stun_query(const char *host, const char *port, int timeout_ms,
	char *ip_out, size_t ip_size, uint16_t *port_out, uint16_t *local_port_out);

// Same STUN round trip, but over a caller-owned UDP socket. The socket remains
// open and unconnected so it can be reused for candidate punching after the
// public mapping has been advertised.
stun_status stun_query_fd(int fd, const char *host, const char *port, int timeout_ms,
	char *ip_out, size_t ip_size, uint16_t *port_out, uint16_t *local_port_out);

const char *stun_strerror(stun_status s);

#ifdef __cplusplus
}
#endif
#endif // CHIAKI_TIZEN_STUN_H
