// Unit + loopback-integration tests for the protocol-accurate hole-punch.
//   ./punch_test  -> packet unit tests + a bidirectional punch round trip over
//                    UDP loopback against a "fake console" thread.

#include "punch.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
	if(cond) printf("  PASS: %s\n", msg); \
	else { printf("  FAIL: %s\n", msg); failures++; } \
} while(0)

static uint32_t rd_u32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static punch_ids make_ids(void)
{
	punch_ids ids;
	memset(&ids, 0, sizeof(ids));
	memset(ids.hashed_id_local, 0x11, PUNCH_HASH_SIZE);
	memset(ids.hashed_id_console, 0x22, PUNCH_HASH_SIZE);
	ids.sid_local = 0x1234;
	ids.sid_console = 0x5678;
	return ids;
}

static void test_packet(void)
{
	printf("[test] 88-byte packet layout (20-byte hashed ids)\n");
	punch_ids ids = make_ids();
	uint8_t id[5] = { 0x10, 0x11, 0x12, 0x13, 0x14 };

	uint8_t req[PUNCH_PKT_SIZE];
	punch_build_request(req, &ids, id);
	CHECK(rd_u32(req + PUNCH_OFF_TYPE) == PUNCH_MSG_REQ, "type = MSG_REQ");
	CHECK(req[PUNCH_OFF_HASH_LOCAL] == 0x11 && req[PUNCH_OFF_HASH_LOCAL + 19] == 0x11
		&& req[PUNCH_OFF_HASH_LOCAL + 20] == 0x00, "hashed_id_local is 20 bytes, zero-padded");
	CHECK(req[PUNCH_OFF_HASH_CONSOLE] == 0x22, "hashed_id_console at 0x24");
	CHECK(req[PUNCH_OFF_SID_LOCAL] == 0x12 && req[PUNCH_OFF_SID_LOCAL + 1] == 0x34, "sid_local BE at 0x44");
	CHECK(memcmp(req + PUNCH_OFF_REQID, id, 5) == 0, "request id at 0x4b");

	uint8_t resp[PUNCH_PKT_SIZE];
	punch_build_response(resp, &ids, id, "1.2.3.4", 0x0506);
	CHECK(rd_u32(resp + PUNCH_OFF_TYPE) == PUNCH_MSG_RESP, "response type = MSG_RESP");
	CHECK(punch_is_response_for(resp, sizeof(resp), id), "response echoes the request id");
	// 0x50 = sid_local XOR addr : 0x1234 seed on first 2 bytes, then XOR 1.2.3.4
	// byte0 = 0x12 ^ 0x01, byte1 = 0x34 ^ 0x02
	CHECK(resp[PUNCH_OFF_XOR_ADDR] == (0x12 ^ 0x01) && resp[PUNCH_OFF_XOR_ADDR + 1] == (0x34 ^ 0x02),
		"XOR addr field encodes sid_local ^ peer addr");
}

// ---- loopback fake console (bidirectional) ----
struct console_ctx { int fd; int saw_our_resp; };

static void raw_pkt(uint8_t *buf, uint32_t type, const uint8_t reqid[5])
{
	memset(buf, 0, PUNCH_PKT_SIZE);
	buf[0] = (uint8_t)(type >> 24); buf[1] = (uint8_t)(type >> 16);
	buf[2] = (uint8_t)(type >> 8);  buf[3] = (uint8_t)type;
	memcpy(buf + PUNCH_OFF_REQID, reqid, 5);
}

static void *fake_console(void *arg)
{
	struct console_ctx *ctx = arg;
	uint8_t buf[256];
	struct sockaddr_in from;
	socklen_t fl = sizeof(from);
	// 1. receive our REQUEST
	long r = recvfrom(ctx->fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
	if(r < PUNCH_PKT_SIZE || rd_u32(buf + PUNCH_OFF_TYPE) != PUNCH_MSG_REQ)
		return NULL;
	// 2. probe US with a REQUEST (we must answer with a RESPONSE)
	uint8_t creq[PUNCH_PKT_SIZE];
	uint8_t cid[5] = { 0xB0, 0xB1, 0xB2, 0xB3, 0xB4 };
	raw_pkt(creq, PUNCH_MSG_REQ, cid);
	sendto(ctx->fd, creq, sizeof(creq), 0, (struct sockaddr *)&from, fl);
	// 3. answer OUR request (punch_run uses deterministic id 0xA0..0xA4)
	uint8_t our_id[5] = { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4 };
	uint8_t cresp[PUNCH_PKT_SIZE];
	raw_pkt(cresp, PUNCH_MSG_RESP, our_id);
	sendto(ctx->fd, cresp, sizeof(cresp), 0, (struct sockaddr *)&from, fl);
	// 4. confirm we answered its probe
	struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
	setsockopt(ctx->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	long r2 = recvfrom(ctx->fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
	if(r2 >= PUNCH_PKT_SIZE && rd_u32(buf + PUNCH_OFF_TYPE) == PUNCH_MSG_RESP
		&& memcmp(buf + PUNCH_OFF_REQID, cid, 5) == 0)
		ctx->saw_our_resp = 1;
	return NULL;
}

static int bind_loopback(uint16_t *port_out)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if(fd < 0) return -1;
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = 0;
	if(bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { close(fd); return -1; }
	socklen_t l = sizeof(a);
	getsockname(fd, (struct sockaddr *)&a, &l);
	*port_out = ntohs(a.sin_port);
	return fd;
}

static void test_loopback_punch(void)
{
	printf("[test] bidirectional punch round trip over UDP loopback\n");
	uint16_t cport = 0, pport = 0;
	int cfd = bind_loopback(&cport);
	int pfd = bind_loopback(&pport);
	CHECK(cfd >= 0 && pfd >= 0, "bound console + punch sockets");
	if(cfd < 0 || pfd < 0) return;

	struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
	setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	struct console_ctx ctx = { .fd = cfd, .saw_our_resp = 0 };
	pthread_t th;
	pthread_create(&th, NULL, fake_console, &ctx);

	punch_ids ids = make_ids();
	punch_candidate cands[2] = {
		{ "203.0.113.9", 9999 },  // dead decoy
		{ "127.0.0.1", cport },   // reachable console
	};
	int selected = -1;
	punch_status s = punch_run(pfd, cands, 2, &ids, 300, 15, &selected);
	CHECK(s == PUNCH_OK, "punch confirmed a candidate");
	CHECK(selected == 1, "selected the reachable candidate (index 1)");

	pthread_join(th, NULL);
	CHECK(ctx.saw_our_resp, "we answered the console's inbound probe with a RESPONSE");
	if(s == PUNCH_OK)
		printf("  -> selected %d (%s:%u)\n", selected, cands[selected].ip, cands[selected].port);

	close(cfd); close(pfd);
}

int main(void)
{
	printf("=== hole-punch spike tests (protocol-accurate) ===\n");
	test_packet();
	test_loopback_punch();
	printf("=== %s (%d failure%s) ===\n",
		failures ? "FAILURES" : "ALL PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
