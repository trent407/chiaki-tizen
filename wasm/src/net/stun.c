// chiaki-tizen spike: minimal STUN (RFC 5389) client. See stun.h.

#include "stun.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <sys/time.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#endif

#define STUN_BINDING_REQUEST   0x0001
#define STUN_BINDING_RESPONSE  0x0101
#define STUN_ATTR_MAPPED_ADDR      0x0001
#define STUN_ATTR_XOR_MAPPED_ADDR  0x0020
#define STUN_FAMILY_IPV4 0x01

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get_u32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

const char *stun_strerror(stun_status s)
{
	switch(s)
	{
		case STUN_OK: return "ok";
		case STUN_ERR_ARG: return "bad argument";
		case STUN_ERR_SOCKET: return "socket/resolve failed";
		case STUN_ERR_IO: return "i/o error";
		case STUN_ERR_TIMEOUT: return "timed out waiting for response";
		case STUN_ERR_PARSE: return "response not a binding response for our txid";
		case STUN_ERR_NO_MAPPING: return "no mapped address in response";
	}
	return "unknown";
}

static void fill_random(uint8_t *buf, size_t len)
{
#if !defined(_WIN32)
	FILE *f = fopen("/dev/urandom", "rb");
	if(f)
	{
		size_t got = fread(buf, 1, len, f);
		fclose(f);
		if(got == len)
			return;
	}
#endif
	// Fallback (spike): not cryptographic, but a STUN txid only needs to be
	// unique enough to match request/response.
	static uint32_t seed = 0x12345678u;
	for(size_t i = 0; i < len; i++)
	{
		seed = seed * 1103515245u + 12345u;
		buf[i] = (uint8_t)(seed >> 16);
	}
}

void stun_build_request(uint8_t buf[STUN_HEADER_SIZE], uint8_t txid[STUN_TXID_SIZE])
{
	fill_random(txid, STUN_TXID_SIZE);
	put_u16(buf + 0, STUN_BINDING_REQUEST);
	put_u16(buf + 2, 0); // no attributes
	buf[4] = 0x21; buf[5] = 0x12; buf[6] = 0xA4; buf[7] = 0x42; // magic cookie
	memcpy(buf + 8, txid, STUN_TXID_SIZE);
}

stun_status stun_parse_response(const uint8_t *resp, size_t len,
	const uint8_t txid[STUN_TXID_SIZE],
	char *ip_out, size_t ip_size, uint16_t *port_out)
{
	if(!resp || !ip_out || !port_out || len < STUN_HEADER_SIZE)
		return STUN_ERR_ARG;
	if(get_u16(resp + 0) != STUN_BINDING_RESPONSE)
		return STUN_ERR_PARSE;
	if(get_u32(resp + 4) != STUN_MAGIC_COOKIE)
		return STUN_ERR_PARSE;
	if(memcmp(resp + 8, txid, STUN_TXID_SIZE) != 0)
		return STUN_ERR_PARSE;

	uint16_t msg_len = get_u16(resp + 2);
	if((size_t)msg_len + STUN_HEADER_SIZE > len)
		return STUN_ERR_PARSE;

	const uint8_t *p = resp + STUN_HEADER_SIZE;
	const uint8_t *end = p + msg_len;
	while(p + 4 <= end)
	{
		uint16_t atype = get_u16(p);
		uint16_t alen = get_u16(p + 2);
		const uint8_t *val = p + 4;
		if(val + alen > end)
			break;

		if((atype == STUN_ATTR_XOR_MAPPED_ADDR || atype == STUN_ATTR_MAPPED_ADDR) && alen >= 8)
		{
			uint8_t family = val[1];
			if(family == STUN_FAMILY_IPV4)
			{
				uint16_t port = get_u16(val + 2);
				uint32_t addr = get_u32(val + 4);
				if(atype == STUN_ATTR_XOR_MAPPED_ADDR)
				{
					port ^= (uint16_t)(STUN_MAGIC_COOKIE >> 16);
					addr ^= STUN_MAGIC_COOKIE;
				}
				int n = snprintf(ip_out, ip_size, "%u.%u.%u.%u",
					(addr >> 24) & 0xff, (addr >> 16) & 0xff,
					(addr >> 8) & 0xff, addr & 0xff);
				if(n < 0 || (size_t)n >= ip_size)
					return STUN_ERR_ARG;
				*port_out = port;
				return STUN_OK;
			}
		}
		// attributes are padded to 4-byte boundaries
		uint16_t padded = (uint16_t)((alen + 3) & ~3u);
		p = val + padded;
	}
	return STUN_ERR_NO_MAPPING;
}

stun_status stun_query(const char *host, const char *port, int timeout_ms,
	char *ip_out, size_t ip_size, uint16_t *port_out, uint16_t *local_port_out)
{
	if(!host || !port || !ip_out || !port_out)
		return STUN_ERR_ARG;

	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;      // IPv4 for the spike
	hints.ai_socktype = SOCK_DGRAM;
	if(getaddrinfo(host, port, &hints, &res) != 0 || !res)
		return STUN_ERR_SOCKET;

	int fd = (int)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if(fd < 0)
	{
		freeaddrinfo(res);
		return STUN_ERR_SOCKET;
	}

	stun_status status = stun_query_fd(fd, host, port, timeout_ms,
		ip_out, ip_size, port_out, local_port_out);

#if defined(_WIN32)
	closesocket(fd);
#else
	close(fd);
#endif
	freeaddrinfo(res);
	return status;
}

stun_status stun_query_fd(int fd, const char *host, const char *port, int timeout_ms,
	char *ip_out, size_t ip_size, uint16_t *port_out, uint16_t *local_port_out)
{
	if(fd < 0 || !host || !port || !ip_out || !port_out)
		return STUN_ERR_ARG;

	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if(getaddrinfo(host, port, &hints, &res) != 0 || !res)
		return STUN_ERR_SOCKET;

	stun_status status = STUN_ERR_IO;
	uint8_t req[STUN_HEADER_SIZE];
	uint8_t txid[STUN_TXID_SIZE];
	stun_build_request(req, txid);
	if(sendto(fd, req, sizeof(req), 0, res->ai_addr, res->ai_addrlen) != (long)sizeof(req))
		goto done;

	if(local_port_out)
	{
		struct sockaddr_in local;
		socklen_t ll = sizeof(local);
		if(getsockname(fd, (struct sockaddr *)&local, &ll) == 0)
			*local_port_out = ntohs(local.sin_port);
	}

	struct timeval tv;
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
	if(sel < 0)
	{
		status = STUN_ERR_IO;
		goto done;
	}
	if(sel == 0)
	{
		status = STUN_ERR_TIMEOUT;
		goto done;
	}

	uint8_t resp[512];
	long r = recvfrom(fd, resp, sizeof(resp), 0, NULL, NULL);
	if(r <= 0)
	{
		status = (errno == EAGAIN || errno == EWOULDBLOCK) ? STUN_ERR_TIMEOUT : STUN_ERR_IO;
		goto done;
	}

	status = stun_parse_response(resp, (size_t)r, txid, ip_out, ip_size, port_out);

done:
	freeaddrinfo(res);
	return status;
}
