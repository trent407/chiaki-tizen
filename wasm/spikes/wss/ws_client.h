// chiaki-tizen spike: minimal mbedTLS + RFC 6455 WebSocket client.
//
// Purpose: prove we can open Sony's authenticated push WebSocket
//   wss://<fqdn>/np/pushNotification
// with the custom headers (Authorization: Bearer, Sec-WebSocket-Protocol,
// X-PSN-*) that the browser/Tizen WebSocket API cannot set. This is the one
// component of the over-the-internet path that must live in WASM rather than
// JS (see docs/remote-play-phase1-design.md §0).
//
// This file is intentionally standalone and transport-agnostic: it uses
// mbedTLS' net layer for TCP so it builds and runs natively for verification.
// On the Tizen target the same TLS/WS logic runs, but the TCP BIO is swapped
// for the Tizen Sockets Extension (see ws_set_bio() note in ws_client.c).

#ifndef CHIAKI_TIZEN_WS_CLIENT_H
#define CHIAKI_TIZEN_WS_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// RFC 6455 opcodes
#define WS_OP_CONT  0x0
#define WS_OP_TEXT  0x1
#define WS_OP_BIN   0x2
#define WS_OP_CLOSE 0x8
#define WS_OP_PING  0x9
#define WS_OP_PONG  0xA

typedef enum {
	WS_OK = 0,
	WS_ERR_ARG = -1,
	WS_ERR_TCP = -2,
	WS_ERR_TLS = -3,
	WS_ERR_HTTP = -4,      // upgrade response was not 101
	WS_ERR_ACCEPT = -5,    // Sec-WebSocket-Accept mismatch
	WS_ERR_PROTO = -6,     // malformed frame
	WS_ERR_IO = -7,
	WS_ERR_CLOSED = -8,    // peer sent a CLOSE frame
	WS_ERR_MEM = -9,
} ws_status;

typedef struct ws_client ws_client;

typedef struct {
	const char *host;       // "mobile-pushcl.np.communication.playstation.net"
	const char *port;       // "443"
	const char *path;       // "/np/pushNotification"
	const char *const *headers; // extra request header lines, "Name: Value", no CRLF
	int n_headers;
	int verify_peer;        // 1 = require a valid server cert (needs ca_file)
	const char *ca_file;    // PEM CA bundle path, or NULL
	int timeout_ms;         // per-recv timeout (0 = block)
} ws_config;

// Connect TCP, do the TLS handshake, and perform the WebSocket upgrade with the
// given custom headers. On WS_OK, *out owns the connection.
ws_status ws_connect(const ws_config *cfg, ws_client **out);

// Send one frame (client frames are always masked, per RFC 6455).
ws_status ws_send(ws_client *c, int opcode, const uint8_t *data, size_t len);

// Receive one *application* frame. PING is answered with PONG and PONG is
// skipped internally; CLOSE returns WS_ERR_CLOSED. On WS_OK, *opcode and
// *out_len describe the payload written into buf.
ws_status ws_recv(ws_client *c, uint8_t *buf, size_t buf_size,
	int *opcode, size_t *out_len);

void ws_close(ws_client *c);

const char *ws_strerror(ws_status s);

// ---- pure helpers, exposed for unit testing --------------------------------

// Sec-WebSocket-Accept = base64(SHA1(client_key + RFC6455_GUID)).
// out must hold at least 29 bytes. Returns WS_OK on success.
ws_status ws_compute_accept(const char *client_key, char *out, size_t out_size);

// Build a client frame header (FIN=1, masked). Writes up to 14 bytes into hdr
// and returns the header length; the 4-byte mask is the last bytes written.
size_t ws_build_header(uint8_t *hdr, int opcode, size_t payload_len,
	const uint8_t mask[4]);

// Mask/unmask payload in place (XOR with the 4-byte key). Symmetric.
void ws_mask(uint8_t *data, size_t len, const uint8_t mask[4]);

// Validate a raw HTTP upgrade response against the key we sent: checks the 101
// status and that Sec-WebSocket-Accept matches base64(SHA1(key+GUID)). The
// header NAME is matched case-insensitively; the base64 VALUE is compared
// case-sensitively (base64 is case-significant). Returns WS_OK / WS_ERR_HTTP /
// WS_ERR_ACCEPT.
ws_status ws_verify_accept(const char *http_response, const char *sent_key);

#ifdef __cplusplus
}
#endif
#endif // CHIAKI_TIZEN_WS_CLIENT_H
