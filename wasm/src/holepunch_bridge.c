// chiaki-tizen: holepunch adapter (replaces holepunch_stub.c). See the header.
//
// This provides the ChiakiHolepunchSession that session.c/ctrl.c drive on the
// remote-play path. The heavy PSN signaling (libcurl/libevent/json-c in
// chiaki-ng's holepunch.c) is intentionally NOT reproduced here — it runs in
// JavaScript + the WASM transport. This adapter just hands chiaki the sockets
// and console details that work already produced.

#include <netinet/in.h> // INET6_ADDRSTRLEN (used by ChiakiHolepunchRegistInfo)
#include <arpa/inet.h>

#include "holepunch_bridge.h"

#include <stdlib.h>
#include <string.h>

// Our concrete definition of the opaque ChiakiHolepunchSession handle.
struct session_t
{
	chiaki_socket_t ctrl_sock; // RUDP control socket (chiaki_rudp_init consumes)
	chiaki_socket_t data_sock; // takion data socket
	char ps_ip[INET6_ADDRSTRLEN];
	uint16_t ps_ctrl_port;
	ChiakiHolepunchRegistInfo regist_info;
};

ChiakiHolepunchSession ct_holepunch_bridge_new(
	chiaki_socket_t ctrl_sock, chiaki_socket_t data_sock,
	const char *ps_ip, uint16_t ps_ctrl_port,
	const ChiakiHolepunchRegistInfo *regist_info)
{
	struct session_t *s = calloc(1, sizeof(*s));
	if(!s)
		return NULL;
	s->ctrl_sock = ctrl_sock;
	s->data_sock = data_sock;
	s->ps_ctrl_port = ps_ctrl_port;
	if(ps_ip)
	{
		strncpy(s->ps_ip, ps_ip, sizeof(s->ps_ip) - 1);
		s->ps_ip[sizeof(s->ps_ip) - 1] = '\0';
	}
	if(regist_info)
		s->regist_info = *regist_info;
	return s;
}

// --- accessors chiaki calls on the remote path -----------------------------

CHIAKI_EXPORT ChiakiHolepunchRegistInfo chiaki_get_regist_info(ChiakiHolepunchSession session)
{
	if(session)
		return session->regist_info;
	ChiakiHolepunchRegistInfo empty;
	memset(&empty, 0, sizeof(empty));
	return empty;
}

CHIAKI_EXPORT void chiaki_get_ps_selected_addr(ChiakiHolepunchSession session, char *ps_ip)
{
	if(!ps_ip)
		return;
	if(session)
	{
		strncpy(ps_ip, session->ps_ip, INET6_ADDRSTRLEN - 1);
		ps_ip[INET6_ADDRSTRLEN - 1] = '\0';
	}
	else
		*ps_ip = '\0';
}

CHIAKI_EXPORT uint16_t chiaki_get_ps_ctrl_port(ChiakiHolepunchSession session)
{
	return session ? session->ps_ctrl_port : 0;
}

CHIAKI_EXPORT chiaki_socket_t *chiaki_get_holepunch_sock(ChiakiHolepunchSession session, ChiakiHolepunchPortType type)
{
	if(!session)
		return NULL;
	return (type == CHIAKI_HOLEPUNCH_PORT_TYPE_CTRL) ? &session->ctrl_sock : &session->data_sock;
}

CHIAKI_EXPORT ChiakiErrorCode holepunch_session_create_offer(ChiakiHolepunchSession session)
{
	// Offer/answer + candidate exchange happen in the JS signaling layer before
	// the session is created; nothing to do here.
	return session ? CHIAKI_ERR_SUCCESS : CHIAKI_ERR_UNKNOWN;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_holepunch_session_punch_hole(ChiakiHolepunchSession session, ChiakiHolepunchPortType port_type)
{
	// The hole was punched by the WASM transport (STUN + candidate check) before
	// the session started; just confirm the requested socket is live.
	if(!session)
		return CHIAKI_ERR_UNKNOWN;
	chiaki_socket_t s = (port_type == CHIAKI_HOLEPUNCH_PORT_TYPE_CTRL)
		? session->ctrl_sock : session->data_sock;
	return CHIAKI_SOCKET_IS_INVALID(s) ? CHIAKI_ERR_UNKNOWN : CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT void chiaki_holepunch_session_fini(ChiakiHolepunchSession session)
{
	if(!session)
		return;
	if(!CHIAKI_SOCKET_IS_INVALID(session->ctrl_sock))
		CHIAKI_SOCKET_CLOSE(session->ctrl_sock);
	if(!CHIAKI_SOCKET_IS_INVALID(session->data_sock))
		CHIAKI_SOCKET_CLOSE(session->data_sock);
	free(session);
}
