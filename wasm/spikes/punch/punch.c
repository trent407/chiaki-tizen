// chiaki-tizen: PSN candidate-check hole-punch (protocol-accurate). See punch.h.
// Mirrors holepunch.c check_candidates() + send_response_ps().

#include "punch.h"

#include <stdio.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static void put_u32_be(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t get_u32_be(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put_u16_be(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static void xor_bytes(uint8_t *dst, const uint8_t *src, size_t n)
{
	for(size_t i = 0; i < n; i++)
		dst[i] ^= src[i];
}

const char *punch_strerror(punch_status s)
{
	switch(s)
	{
		case PUNCH_OK: return "ok";
		case PUNCH_ERR_ARG: return "bad argument";
		case PUNCH_ERR_SOCKET: return "socket error";
		case PUNCH_ERR_TIMEOUT: return "no candidate answered";
		case PUNCH_ERR_IO: return "i/o error";
	}
	return "unknown";
}

static void fill_common(uint8_t *buf, const punch_ids *ids)
{
	memset(buf, 0, PUNCH_PKT_SIZE);
	memcpy(buf + PUNCH_OFF_HASH_LOCAL, ids->hashed_id_local, PUNCH_HASH_SIZE);
	memcpy(buf + PUNCH_OFF_HASH_CONSOLE, ids->hashed_id_console, PUNCH_HASH_SIZE);
	put_u16_be(buf + PUNCH_OFF_SID_LOCAL, ids->sid_local);
	put_u16_be(buf + PUNCH_OFF_SID_CONSOLE, ids->sid_console);
}

void punch_build_request(uint8_t buf[PUNCH_PKT_SIZE], const punch_ids *ids,
	const uint8_t req_id[PUNCH_REQID_SIZE])
{
	fill_common(buf, ids);
	put_u32_be(buf + PUNCH_OFF_TYPE, PUNCH_MSG_REQ);
	memcpy(buf + PUNCH_OFF_REQID, req_id, PUNCH_REQID_SIZE);
}

void punch_build_response(uint8_t buf[PUNCH_PKT_SIZE], const punch_ids *ids,
	const uint8_t their_req_id[PUNCH_REQID_SIZE],
	const char *peer_ipv4, uint16_t peer_port)
{
	fill_common(buf, ids);
	put_u32_be(buf + PUNCH_OFF_TYPE, PUNCH_MSG_RESP);
	memcpy(buf + PUNCH_OFF_REQID, their_req_id, PUNCH_REQID_SIZE);

	// 0x50 = sid_local XOR peer_ipv4 ; 0x54 = sid_local XOR peer_port
	put_u16_be(buf + PUNCH_OFF_XOR_ADDR, ids->sid_local);
	put_u16_be(buf + PUNCH_OFF_XOR_ADDR + 2, ids->sid_local); // 4-byte field seeded with sid
	put_u16_be(buf + PUNCH_OFF_XOR_PORT, ids->sid_local);
	uint8_t addr[4] = {0};
	if(peer_ipv4)
		inet_pton(AF_INET, peer_ipv4, addr);
	uint8_t port[2];
	put_u16_be(port, peer_port);
	xor_bytes(buf + PUNCH_OFF_XOR_ADDR, addr, 4);
	xor_bytes(buf + PUNCH_OFF_XOR_PORT, port, 2);
}

int punch_is_response_for(const uint8_t *buf, size_t len, const uint8_t req_id[PUNCH_REQID_SIZE])
{
	if(!buf || len < PUNCH_PKT_SIZE)
		return 0;
	if(get_u32_be(buf + PUNCH_OFF_TYPE) != PUNCH_MSG_RESP)
		return 0;
	return memcmp(buf + PUNCH_OFF_REQID, req_id, PUNCH_REQID_SIZE) == 0;
}

int punch_is_request(const uint8_t *buf, size_t len)
{
	if(!buf || len < PUNCH_PKT_SIZE)
		return 0;
	return get_u32_be(buf + PUNCH_OFF_TYPE) == PUNCH_MSG_REQ;
}

static int make_addr(const punch_candidate *c, struct sockaddr_in *out)
{
	memset(out, 0, sizeof(*out));
	out->sin_family = AF_INET;
	out->sin_port = htons(c->port);
	return inet_pton(AF_INET, c->ip, &out->sin_addr) == 1 ? 0 : -1;
}

punch_status punch_run(int fd, const punch_candidate *candidates, size_t n,
	const punch_ids *ids, int timeout_ms, int tries, int *selected)
{
	if(fd < 0 || !candidates || !n || !ids || !selected)
		return PUNCH_ERR_ARG;

	uint8_t req_id[PUNCH_REQID_SIZE];
	for(int i = 0; i < PUNCH_REQID_SIZE; i++)
		req_id[i] = (uint8_t)(0xA0 + i); // deterministic for the spike/test

	uint8_t req[PUNCH_PKT_SIZE];
	punch_build_request(req, ids, req_id);

	struct sockaddr_in addrs[64];
	if(n > 64)
		n = 64;
	for(size_t i = 0; i < n; i++)
		if(make_addr(&candidates[i], &addrs[i]) != 0)
			return PUNCH_ERR_ARG;

	for(int round = 0; round < tries; round++)
	{
		for(size_t i = 0; i < n; i++)
			sendto(fd, req, sizeof(req), 0, (struct sockaddr *)&addrs[i], sizeof(addrs[i]));

		struct timeval tv;
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
		if(sel < 0)
			return PUNCH_ERR_IO;
		if(sel == 0)
			continue;

		// Drain everything currently readable this round.
		for(;;)
		{
			uint8_t pkt[256];
			struct sockaddr_in from;
			socklen_t fromlen = sizeof(from);
			long r = recvfrom(fd, pkt, sizeof(pkt), MSG_DONTWAIT,
				(struct sockaddr *)&from, &fromlen);
			if(r <= 0)
				break;

			// Which candidate did this come from?
			int idx = -1;
			for(size_t i = 0; i < n; i++)
				if(from.sin_addr.s_addr == addrs[i].sin_addr.s_addr
					&& from.sin_port == addrs[i].sin_port)
				{ idx = (int)i; break; }

			if(punch_is_request(pkt, (size_t)r))
			{
				// The console is probing us: answer with a RESPONSE encoding the
				// peer address/port XOR'd with sid_local.
				const char *peer_ip = idx >= 0 ? candidates[idx].ip : NULL;
				uint16_t peer_port = idx >= 0 ? candidates[idx].port : ntohs(from.sin_port);
				uint8_t resp[PUNCH_PKT_SIZE];
				punch_build_response(resp, ids, pkt + PUNCH_OFF_REQID, peer_ip, peer_port);
				sendto(fd, resp, sizeof(resp), 0, (struct sockaddr *)&from, fromlen);
				continue;
			}

			if(punch_is_response_for(pkt, (size_t)r, req_id))
			{
				*selected = idx >= 0 ? idx : 0;
				return PUNCH_OK;
			}
		}
	}
	return PUNCH_ERR_TIMEOUT;
}
