// Unit + integration tests for the STUN spike.
//
//   ./stun_test              -> offline unit tests (RFC vectors)
//   ./stun_test live [host] [port]
//                            -> live query (default stun.l.google.com:19302)

#include "stun.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
	if(cond) printf("  PASS: %s\n", msg); \
	else { printf("  FAIL: %s\n", msg); failures++; } \
} while(0)

static void test_build(void)
{
	printf("[test] Binding Request construction\n");
	uint8_t req[STUN_HEADER_SIZE], txid[STUN_TXID_SIZE];
	stun_build_request(req, txid);
	CHECK(req[0] == 0x00 && req[1] == 0x01, "message type = Binding Request (0x0001)");
	CHECK(req[2] == 0x00 && req[3] == 0x00, "length = 0 (no attributes)");
	CHECK(req[4] == 0x21 && req[5] == 0x12 && req[6] == 0xA4 && req[7] == 0x42,
		"magic cookie 0x2112A442");
	CHECK(memcmp(req + 8, txid, STUN_TXID_SIZE) == 0, "txid copied into header");
}

// RFC 5769 §2.1: XOR-MAPPED-ADDRESS decodes to 192.0.2.1 : 32853
static void test_xor_mapped(void)
{
	printf("[test] XOR-MAPPED-ADDRESS parse (RFC 5769 vector)\n");
	const uint8_t txid[STUN_TXID_SIZE] = {
		0xb7,0xe7,0xa7,0x01,0xbc,0x34,0xd6,0x86,0xfa,0x87,0xdf,0xae };
	uint8_t resp[STUN_HEADER_SIZE + 12];
	// header
	resp[0]=0x01; resp[1]=0x01;           // Binding Response
	resp[2]=0x00; resp[3]=0x0c;           // length = 12
	resp[4]=0x21; resp[5]=0x12; resp[6]=0xA4; resp[7]=0x42;
	memcpy(resp + 8, txid, STUN_TXID_SIZE);
	// XOR-MAPPED-ADDRESS attribute, IPv4, x-port A147, x-addr E112A643
	uint8_t attr[12] = { 0x00,0x20, 0x00,0x08, 0x00,0x01, 0xA1,0x47, 0xE1,0x12,0xA6,0x43 };
	memcpy(resp + STUN_HEADER_SIZE, attr, 12);

	char ip[32]; uint16_t port = 0;
	stun_status s = stun_parse_response(resp, sizeof(resp), txid, ip, sizeof(ip), &port);
	CHECK(s == STUN_OK, "parse returns STUN_OK");
	CHECK(strcmp(ip, "192.0.2.1") == 0, "address decodes to 192.0.2.1");
	CHECK(port == 32853, "port decodes to 32853");
}

// Plain MAPPED-ADDRESS (no XOR) fallback
static void test_mapped_fallback(void)
{
	printf("[test] MAPPED-ADDRESS (non-XOR) fallback\n");
	const uint8_t txid[STUN_TXID_SIZE] = {1,2,3,4,5,6,7,8,9,10,11,12};
	uint8_t resp[STUN_HEADER_SIZE + 12];
	resp[0]=0x01; resp[1]=0x01; resp[2]=0x00; resp[3]=0x0c;
	resp[4]=0x21; resp[5]=0x12; resp[6]=0xA4; resp[7]=0x42;
	memcpy(resp + 8, txid, STUN_TXID_SIZE);
	// MAPPED-ADDRESS 0x0001, IPv4, port 0x04D2 (1234), addr 203.0.113.5 = CB007105
	uint8_t attr[12] = { 0x00,0x01, 0x00,0x08, 0x00,0x01, 0x04,0xD2, 0xCB,0x00,0x71,0x05 };
	memcpy(resp + STUN_HEADER_SIZE, attr, 12);

	char ip[32]; uint16_t port = 0;
	stun_status s = stun_parse_response(resp, sizeof(resp), txid, ip, sizeof(ip), &port);
	CHECK(s == STUN_OK, "parse returns STUN_OK");
	CHECK(strcmp(ip, "203.0.113.5") == 0, "address 203.0.113.5");
	CHECK(port == 1234, "port 1234");
}

static void test_txid_mismatch(void)
{
	printf("[test] rejects a response for a different txid\n");
	const uint8_t txid[STUN_TXID_SIZE] = {1,2,3,4,5,6,7,8,9,10,11,12};
	const uint8_t other[STUN_TXID_SIZE] = {9,9,9,9,9,9,9,9,9,9,9,9};
	uint8_t resp[STUN_HEADER_SIZE + 12];
	resp[0]=0x01; resp[1]=0x01; resp[2]=0x00; resp[3]=0x0c;
	resp[4]=0x21; resp[5]=0x12; resp[6]=0xA4; resp[7]=0x42;
	memcpy(resp + 8, other, STUN_TXID_SIZE);
	uint8_t attr[12] = { 0x00,0x20, 0x00,0x08, 0x00,0x01, 0xA1,0x47, 0xE1,0x12,0xA6,0x43 };
	memcpy(resp + STUN_HEADER_SIZE, attr, 12);

	char ip[32]; uint16_t port = 0;
	CHECK(stun_parse_response(resp, sizeof(resp), txid, ip, sizeof(ip), &port) == STUN_ERR_PARSE,
		"txid mismatch -> STUN_ERR_PARSE");
}

static void test_live(const char *host, const char *port)
{
	printf("[live] STUN query to %s:%s\n", host, port);
	char ip[64] = {0};
	uint16_t pub_port = 0, local_port = 0;
	stun_status s = stun_query(host, port, 4000, ip, sizeof(ip), &pub_port, &local_port);
	CHECK(s == STUN_OK, "query returned a public mapping");
	if(s == STUN_OK)
		printf("  -> public %s:%u  (local source port %u)\n", ip, pub_port, local_port);
	else
		printf("  -> %s\n", stun_strerror(s));
}

int main(int argc, char **argv)
{
	printf("=== STUN spike tests ===\n");
	test_build();
	test_xor_mapped();
	test_mapped_fallback();
	test_txid_mismatch();

	if(argc > 1 && strcmp(argv[1], "live") == 0)
	{
		const char *host = argc > 2 ? argv[2] : "stun.l.google.com";
		const char *port = argc > 3 ? argv[3] : "19302";
		test_live(host, port);
	}
	else
	{
		printf("[live] skipped (run with 'live' to query a real STUN server)\n");
	}

	printf("=== %s (%d failure%s) ===\n",
		failures ? "FAILURES" : "ALL PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
