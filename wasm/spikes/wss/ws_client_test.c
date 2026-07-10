// Unit + integration tests for the WSS spike.
//
//   ./ws_client_test            -> run offline unit tests (no network)
//   ./ws_client_test live [host] [port] [path]
//                               -> also do a live TLS+WebSocket handshake
//                                  (default: echo.websocket.events / 443 / /)

#include "ws_client.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
	if(cond) { printf("  PASS: %s\n", msg); } \
	else { printf("  FAIL: %s\n", msg); failures++; } \
} while(0)

// RFC 6455 §1.3 handshake example
static void test_accept(void)
{
	printf("[test] Sec-WebSocket-Accept (RFC 6455 vector)\n");
	char out[32];
	ws_status s = ws_compute_accept("dGhlIHNhbXBsZSBub25jZQ==", out, sizeof(out));
	CHECK(s == WS_OK, "compute returns WS_OK");
	CHECK(strcmp(out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0, "accept matches expected");
}

// RFC 6455 §5.7 masked "Hello" example
static void test_frame(void)
{
	printf("[test] client frame build + mask (RFC 6455 vector)\n");
	uint8_t mask[4] = { 0x37, 0xfa, 0x21, 0x3d };
	uint8_t hdr[14];
	size_t hlen = ws_build_header(hdr, WS_OP_TEXT, 5, mask);
	CHECK(hlen == 6, "header length is 6 (2 + 4 mask) for short payload");
	CHECK(hdr[0] == 0x81, "byte0 = FIN|TEXT (0x81)");
	CHECK(hdr[1] == 0x85, "byte1 = MASK|len5 (0x85)");
	CHECK(memcmp(hdr + 2, mask, 4) == 0, "mask key follows header");

	uint8_t payload[5];
	memcpy(payload, "Hello", 5);
	ws_mask(payload, 5, mask);
	uint8_t expect[5] = { 0x7f, 0x9f, 0x4d, 0x51, 0x58 };
	CHECK(memcmp(payload, expect, 5) == 0, "masked 'Hello' matches RFC bytes");

	ws_mask(payload, 5, mask); // symmetric -> back to plaintext
	CHECK(memcmp(payload, "Hello", 5) == 0, "unmask restores plaintext");
}

// Extended length header encodings
static void test_ext_len(void)
{
	printf("[test] extended-length header encoding\n");
	uint8_t mask[4] = { 1, 2, 3, 4 };
	uint8_t hdr[14];

	size_t h1 = ws_build_header(hdr, WS_OP_BIN, 200, mask);
	CHECK(h1 == 8, "126<=len<=65535 -> 2 + 2 + 4 = 8 byte header");
	CHECK(hdr[1] == (0x80 | 126), "len marker 126");
	CHECK(hdr[2] == 0 && hdr[3] == 200, "16-bit length big-endian");

	size_t h2 = ws_build_header(hdr, WS_OP_BIN, 70000, mask);
	CHECK(h2 == 14, "len>65535 -> 2 + 8 + 4 = 14 byte header");
	CHECK(hdr[1] == (0x80 | 127), "len marker 127");
	// 70000 = 0x11170; 8-byte length occupies hdr[2..9], so the low 3 bytes
	// (01 11 70) land at hdr[7..9].
	CHECK(hdr[7] == 0x01 && hdr[8] == 0x11 && hdr[9] == 0x70, "64-bit length big-endian");
}

// Regression: parse a real upgrade response. Catches the case-folding bug that
// broke every live server (lowercasing the whole response destroyed the
// case-sensitive base64 Accept value).
static void test_verify_accept(void)
{
	printf("[test] upgrade-response Accept validation\n");
	const char *key = "dGhlIHNhbXBsZSBub25jZQ=="; // RFC key -> s3pPLMBiTxaQ9kYGzzhZRbK+xOo=

	const char *ok =
		"HTTP/1.1 101 Switching Protocols\r\n"
		"Upgrade: websocket\r\nConnection: Upgrade\r\n"
		"Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
	CHECK(ws_verify_accept(ok, key) == WS_OK, "accepts a valid 101 response (mixed-case base64)");

	// Header name in odd casing must still match (name is case-insensitive)
	const char *odd_case =
		"HTTP/1.1 101 Switching Protocols\r\n"
		"SEC-WEBSOCKET-ACCEPT: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
	CHECK(ws_verify_accept(odd_case, key) == WS_OK, "header name matched case-insensitively");

	const char *wrong =
		"HTTP/1.1 101 Switching Protocols\r\n"
		"Sec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n";
	CHECK(ws_verify_accept(wrong, key) == WS_ERR_ACCEPT, "rejects a wrong Accept value");

	const char *not101 =
		"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
	CHECK(ws_verify_accept(not101, key) == WS_ERR_HTTP, "rejects a non-101 status");
}

static int test_live(const char *host, const char *port, const char *path)
{
	printf("[live] TLS + WebSocket handshake to wss://%s:%s%s\n", host, port, path);
	const char *headers[] = {
		"User-Agent: chiaki-tizen-spike/0.1",
		// Sony would add: "Authorization: Bearer <token>",
		//                 "Sec-WebSocket-Protocol: np-pushpacket", X-PSN-* ...
	};
	ws_config cfg = {
		.host = host, .port = port, .path = path,
		.headers = headers, .n_headers = (int)(sizeof(headers)/sizeof(headers[0])),
		.verify_peer = 0, // SPIKE ONLY
		.ca_file = NULL,
		.timeout_ms = 8000,
	};
	ws_client *c = NULL;
	ws_status s = ws_connect(&cfg, &c);
	CHECK(s == WS_OK, "handshake completed (101 + valid Accept)");
	if(s != WS_OK)
	{
		printf("  -> %s\n", ws_strerror(s));
		return 1;
	}

	// Send a text frame and read a frame back (echo servers reply/greet).
	const char *msg = "chiaki-tizen ping";
	s = ws_send(c, WS_OP_TEXT, (const uint8_t *)msg, strlen(msg));
	CHECK(s == WS_OK, "sent a masked text frame");

	uint8_t buf[1024];
	int opcode = 0;
	size_t n = 0;
	s = ws_recv(c, buf, sizeof(buf), &opcode, &n);
	CHECK(s == WS_OK, "received a frame back");
	if(s == WS_OK)
		printf("  <- opcode=0x%x len=%zu: %.*s\n", opcode, n, (int)(n > 80 ? 80 : n), buf);

	ws_close(c);
	return 0;
}

int main(int argc, char **argv)
{
	printf("=== WSS spike tests ===\n");
	test_accept();
	test_frame();
	test_ext_len();
	test_verify_accept();

	if(argc > 1 && strcmp(argv[1], "live") == 0)
	{
		const char *host = argc > 2 ? argv[2] : "echo.websocket.events";
		const char *port = argc > 3 ? argv[3] : "443";
		const char *path = argc > 4 ? argv[4] : "/";
		test_live(host, port, path);
	}
	else
	{
		printf("[live] skipped (run with 'live' to test a real endpoint)\n");
	}

	printf("=== %s (%d failure%s) ===\n",
		failures ? "FAILURES" : "ALL PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
