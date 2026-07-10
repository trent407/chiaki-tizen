// chiaki-tizen spike: minimal mbedTLS + RFC 6455 WebSocket client.
// See ws_client.h for intent. Targets mbedTLS 2.28 (the version the module
// already links; Ubuntu 22.04 ships the same, so this builds/runs natively).

#include "ws_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>

static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

struct ws_client {
	mbedtls_net_context net;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_entropy_context entropy;
	mbedtls_x509_crt cacert;
	int have_ca;
	// small buffer to hold bytes read past the HTTP header terminator
	uint8_t stash[512];
	size_t stash_len;
};

const char *ws_strerror(ws_status s)
{
	switch(s)
	{
		case WS_OK: return "ok";
		case WS_ERR_ARG: return "bad argument";
		case WS_ERR_TCP: return "tcp connect failed";
		case WS_ERR_TLS: return "tls handshake failed";
		case WS_ERR_HTTP: return "websocket upgrade rejected (not 101)";
		case WS_ERR_ACCEPT: return "Sec-WebSocket-Accept mismatch";
		case WS_ERR_PROTO: return "malformed frame";
		case WS_ERR_IO: return "i/o error";
		case WS_ERR_CLOSED: return "peer closed";
		case WS_ERR_MEM: return "out of memory";
	}
	return "unknown";
}

// ---------------------------------------------------------------------------
// Pure helpers (unit-tested)
// ---------------------------------------------------------------------------

ws_status ws_compute_accept(const char *client_key, char *out, size_t out_size)
{
	if(!client_key || !out)
		return WS_ERR_ARG;
	char concat[128];
	int n = snprintf(concat, sizeof(concat), "%s%s", client_key, WS_GUID);
	if(n < 0 || (size_t)n >= sizeof(concat))
		return WS_ERR_ARG;

	uint8_t digest[20];
#if defined(MBEDTLS_VERSION_MAJOR) && MBEDTLS_VERSION_MAJOR >= 3
	if(mbedtls_sha1((const unsigned char *)concat, (size_t)n, digest) != 0)
		return WS_ERR_PROTO;
#else
	if(mbedtls_sha1_ret((const unsigned char *)concat, (size_t)n, digest) != 0)
		return WS_ERR_PROTO;
#endif

	size_t olen = 0;
	if(mbedtls_base64_encode((unsigned char *)out, out_size, &olen, digest, sizeof(digest)) != 0)
		return WS_ERR_ARG;
	return WS_OK;
}

size_t ws_build_header(uint8_t *hdr, int opcode, size_t payload_len,
	const uint8_t mask[4])
{
	size_t i = 0;
	hdr[i++] = (uint8_t)(0x80 | (opcode & 0x0f)); // FIN + opcode
	if(payload_len < 126)
	{
		hdr[i++] = (uint8_t)(0x80 | payload_len); // MASK bit + len
	}
	else if(payload_len <= 0xffff)
	{
		hdr[i++] = 0x80 | 126;
		hdr[i++] = (uint8_t)((payload_len >> 8) & 0xff);
		hdr[i++] = (uint8_t)(payload_len & 0xff);
	}
	else
	{
		hdr[i++] = 0x80 | 127;
		for(int s = 56; s >= 0; s -= 8)
			hdr[i++] = (uint8_t)((payload_len >> s) & 0xff);
	}
	memcpy(hdr + i, mask, 4);
	i += 4;
	return i;
}

void ws_mask(uint8_t *data, size_t len, const uint8_t mask[4])
{
	for(size_t i = 0; i < len; i++)
		data[i] ^= mask[i & 3];
}

ws_status ws_verify_accept(const char *response, const char *sent_key)
{
	if(!response || !sent_key)
		return WS_ERR_ARG;
	if(strncmp(response, "HTTP/1.1 101", 12) != 0 && strncmp(response, "HTTP/1.0 101", 12) != 0)
		return WS_ERR_HTTP;

	char expect[32];
	if(ws_compute_accept(sent_key, expect, sizeof(expect)) != WS_OK)
		return WS_ERR_ACCEPT;

	// Find the header name case-insensitively, but read its base64 value from
	// the ORIGINAL response so case is preserved (base64 is case-sensitive).
	static const char needle[] = "sec-websocket-accept:";
	const size_t nlen = sizeof(needle) - 1;
	size_t n = strlen(response);
	for(size_t i = 0; i + nlen <= n; i++)
	{
		size_t j = 0;
		for(; j < nlen; j++)
		{
			char c = response[i + j];
			if(c >= 'A' && c <= 'Z')
				c = (char)(c + 32);
			if(c != needle[j])
				break;
		}
		if(j != nlen)
			continue;
		const char *val = response + i + nlen;
		while(*val == ' ' || *val == '\t')
			val++;
		return (strncmp(val, expect, strlen(expect)) == 0) ? WS_OK : WS_ERR_ACCEPT;
	}
	return WS_ERR_ACCEPT;
}

// ---------------------------------------------------------------------------
// TLS I/O
// ---------------------------------------------------------------------------

static ws_status tls_write_all(ws_client *c, const uint8_t *buf, size_t len)
{
	size_t off = 0;
	while(off < len)
	{
		int r = mbedtls_ssl_write(&c->ssl, buf + off, len - off);
		if(r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if(r <= 0)
			return WS_ERR_IO;
		off += (size_t)r;
	}
	return WS_OK;
}

// Read exactly len bytes, draining the stash first.
static ws_status tls_read_all(ws_client *c, uint8_t *buf, size_t len)
{
	size_t off = 0;
	if(c->stash_len)
	{
		size_t take = c->stash_len < len ? c->stash_len : len;
		memcpy(buf, c->stash, take);
		memmove(c->stash, c->stash + take, c->stash_len - take);
		c->stash_len -= take;
		off = take;
	}
	while(off < len)
	{
		int r = mbedtls_ssl_read(&c->ssl, buf + off, len - off);
		if(r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if(r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
			return WS_ERR_CLOSED;
		if(r < 0)
			return WS_ERR_IO;
		off += (size_t)r;
	}
	return WS_OK;
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

static ws_status do_ws_upgrade(ws_client *c, const ws_config *cfg)
{
	// 16 random bytes -> base64 Sec-WebSocket-Key
	uint8_t nonce[16];
	if(mbedtls_ctr_drbg_random(&c->drbg, nonce, sizeof(nonce)) != 0)
		return WS_ERR_TLS;
	char key[32];
	size_t klen = 0;
	if(mbedtls_base64_encode((unsigned char *)key, sizeof(key), &klen, nonce, sizeof(nonce)) != 0)
		return WS_ERR_MEM;

	// Build request
	char req[2048];
	int n = snprintf(req, sizeof(req),
		"GET %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Version: 13\r\n"
		"Sec-WebSocket-Key: %s\r\n",
		cfg->path, cfg->host, key);
	if(n < 0 || (size_t)n >= sizeof(req))
		return WS_ERR_ARG;
	for(int i = 0; i < cfg->n_headers; i++)
	{
		int m = snprintf(req + n, sizeof(req) - n, "%s\r\n", cfg->headers[i]);
		if(m < 0 || (size_t)(n + m) >= sizeof(req))
			return WS_ERR_ARG;
		n += m;
	}
	if((size_t)n + 3 >= sizeof(req))
		return WS_ERR_ARG;
	memcpy(req + n, "\r\n", 3); // include NUL room; only 2 bytes sent below
	n += 2;

	ws_status s = tls_write_all(c, (const uint8_t *)req, (size_t)n);
	if(s != WS_OK)
		return s;

	// Read response headers up to and including the blank line. Anything read
	// past the terminator is stashed for the frame reader.
	char resp[4096];
	size_t rlen = 0;
	int terminated = 0;
	while(rlen < sizeof(resp) - 1)
	{
		uint8_t ch;
		int r = mbedtls_ssl_read(&c->ssl, &ch, 1);
		if(r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if(r <= 0)
			return WS_ERR_IO;
		resp[rlen++] = (char)ch;
		if(rlen >= 4 && resp[rlen-4] == '\r' && resp[rlen-3] == '\n'
			&& resp[rlen-2] == '\r' && resp[rlen-1] == '\n')
		{
			terminated = 1;
			break;
		}
	}
	if(!terminated)
		return WS_ERR_HTTP;
	resp[rlen] = '\0';

	// Status line 101 + Sec-WebSocket-Accept validation (base64 value compared
	// case-sensitively — see ws_verify_accept).
	return ws_verify_accept(resp, key);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ws_status ws_connect(const ws_config *cfg, ws_client **out)
{
	if(!cfg || !cfg->host || !cfg->port || !cfg->path || !out)
		return WS_ERR_ARG;

	ws_client *c = calloc(1, sizeof(*c));
	if(!c)
		return WS_ERR_MEM;

	mbedtls_net_init(&c->net);
	mbedtls_ssl_init(&c->ssl);
	mbedtls_ssl_config_init(&c->conf);
	mbedtls_ctr_drbg_init(&c->drbg);
	mbedtls_entropy_init(&c->entropy);
	mbedtls_x509_crt_init(&c->cacert);

	ws_status status = WS_ERR_TLS;

	static const char pers[] = "chiaki-tizen-ws";
	if(mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy,
		(const unsigned char *)pers, sizeof(pers) - 1) != 0)
		goto fail;

	// NOTE (Tizen): mbedtls_net_connect uses POSIX sockets. On the Tizen target
	// the raw TCP is created via the Tizen Sockets Extension instead; swap this
	// call + mbedtls_ssl_set_bio for a BIO backed by that fd. The TLS/WS logic
	// below is unchanged. Kept as net layer here so the spike runs natively.
	if(mbedtls_net_connect(&c->net, cfg->host, cfg->port, MBEDTLS_NET_PROTO_TCP) != 0)
	{
		status = WS_ERR_TCP;
		goto fail;
	}

	if(mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
		MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0)
		goto fail;

	if(cfg->verify_peer && cfg->ca_file)
	{
		if(mbedtls_x509_crt_parse_file(&c->cacert, cfg->ca_file) != 0)
			goto fail;
		mbedtls_ssl_conf_ca_chain(&c->conf, &c->cacert, NULL);
		mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
		c->have_ca = 1;
	}
	else
	{
		// SPIKE ONLY: no cert verification. Production MUST bundle Sony's CA
		// roots and set verify_peer=1 to avoid MITM.
		mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
	}

	mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);

	if(mbedtls_ssl_setup(&c->ssl, &c->conf) != 0)
		goto fail;
	if(mbedtls_ssl_set_hostname(&c->ssl, cfg->host) != 0) // SNI + cert CN check
		goto fail;
	mbedtls_ssl_set_bio(&c->ssl, &c->net, mbedtls_net_send, mbedtls_net_recv, NULL);

	int hs;
	while((hs = mbedtls_ssl_handshake(&c->ssl)) != 0)
	{
		if(hs != MBEDTLS_ERR_SSL_WANT_READ && hs != MBEDTLS_ERR_SSL_WANT_WRITE)
		{
			status = WS_ERR_TLS;
			goto fail;
		}
	}

	status = do_ws_upgrade(c, cfg);
	if(status != WS_OK)
		goto fail;

	*out = c;
	return WS_OK;

fail:
	ws_close(c);
	return status;
}

ws_status ws_send(ws_client *c, int opcode, const uint8_t *data, size_t len)
{
	if(!c)
		return WS_ERR_ARG;
	uint8_t mask[4];
	if(mbedtls_ctr_drbg_random(&c->drbg, mask, sizeof(mask)) != 0)
		return WS_ERR_IO;

	uint8_t hdr[14];
	size_t hlen = ws_build_header(hdr, opcode, len, mask);
	ws_status s = tls_write_all(c, hdr, hlen);
	if(s != WS_OK)
		return s;
	if(len)
	{
		uint8_t *tmp = malloc(len);
		if(!tmp)
			return WS_ERR_MEM;
		memcpy(tmp, data, len);
		ws_mask(tmp, len, mask);
		s = tls_write_all(c, tmp, len);
		free(tmp);
	}
	return s;
}

ws_status ws_recv(ws_client *c, uint8_t *buf, size_t buf_size,
	int *opcode, size_t *out_len)
{
	if(!c || !buf || !opcode || !out_len)
		return WS_ERR_ARG;

	for(;;)
	{
		uint8_t h2[2];
		ws_status s = tls_read_all(c, h2, 2);
		if(s != WS_OK)
			return s;
		int op = h2[0] & 0x0f;
		int server_masked = (h2[1] & 0x80) != 0;
		uint64_t plen = h2[1] & 0x7f;
		if(plen == 126)
		{
			uint8_t ext[2];
			if((s = tls_read_all(c, ext, 2)) != WS_OK) return s;
			plen = ((uint64_t)ext[0] << 8) | ext[1];
		}
		else if(plen == 127)
		{
			uint8_t ext[8];
			if((s = tls_read_all(c, ext, 8)) != WS_OK) return s;
			plen = 0;
			for(int i = 0; i < 8; i++)
				plen = (plen << 8) | ext[i];
		}
		uint8_t smask[4] = {0};
		if(server_masked)
		{
			if((s = tls_read_all(c, smask, 4)) != WS_OK) return s;
		}

		// Read payload into a scratch buffer (control frames are small).
		uint8_t *payload = NULL;
		if(plen)
		{
			payload = malloc((size_t)plen);
			if(!payload)
				return WS_ERR_MEM;
			if((s = tls_read_all(c, payload, (size_t)plen)) != WS_OK)
			{
				free(payload);
				return s;
			}
			if(server_masked)
				ws_mask(payload, (size_t)plen, smask);
		}

		if(op == WS_OP_PING)
		{
			ws_send(c, WS_OP_PONG, payload, (size_t)plen);
			free(payload);
			continue; // keep waiting for an application frame
		}
		if(op == WS_OP_PONG)
		{
			free(payload);
			continue;
		}
		if(op == WS_OP_CLOSE)
		{
			free(payload);
			return WS_ERR_CLOSED;
		}

		// Application frame (TEXT/BIN/CONT)
		size_t copy = (size_t)plen < buf_size ? (size_t)plen : buf_size;
		if(payload && copy)
			memcpy(buf, payload, copy);
		free(payload);
		*opcode = op;
		*out_len = copy;
		return WS_OK;
	}
}

void ws_close(ws_client *c)
{
	if(!c)
		return;
	// best-effort close notify
	mbedtls_ssl_close_notify(&c->ssl);
	if(c->have_ca)
		mbedtls_x509_crt_free(&c->cacert);
	mbedtls_ssl_free(&c->ssl);
	mbedtls_ssl_config_free(&c->conf);
	mbedtls_ctr_drbg_free(&c->drbg);
	mbedtls_entropy_free(&c->entropy);
	mbedtls_net_free(&c->net);
	free(c);
}
