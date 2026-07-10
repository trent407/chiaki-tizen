// chiaki-tizen: stubs for PSN holepunch (remote-over-internet play).
//
// The holepunch implementation depends on libcurl (with WebSocket support),
// libevent, miniupnpc and json-c — none of which are practical inside the
// Samsung Emscripten sandbox for a v1 port. Local-network Remote Play never
// takes these code paths: session.c / ctrl.c / regist.c only call them when
// connect_info.holepunch_session != NULL, which the Tizen bridge never sets.
// These stubs exist purely to satisfy the linker.
//
// NOTE: the RUDP transport (chiaki_rudp_*) is NO LONGER stubbed here — the
// real remote/rudp.c + rudpsendbuffer.c are compiled into the module (see
// wasm/CMakeLists.txt). RUDP has no heavy-library dependency; it is a plain
// reliable-UDP layer over sockets and is the first building block for the
// over-the-internet path. Only the PSN signaling / NAT-traversal orchestration
// in holepunch.c remains stubbed.
//
// Symbol list was derived mechanically:
//   nm(defined in remote/holepunch.c.o)
//   ∩ nm(undefined in {session,ctrl,regist,http,takion}.c.o)

#include <netinet/in.h>
#include <arpa/inet.h>

#include <chiaki/remote/holepunch.h>

#include <string.h>

#define STUB_LOG(...) do {} while(0)

CHIAKI_EXPORT ChiakiHolepunchRegistInfo chiaki_get_regist_info(ChiakiHolepunchSession session)
{
	(void)session;
	ChiakiHolepunchRegistInfo info;
	memset(&info, 0, sizeof(info));
	return info;
}

CHIAKI_EXPORT void chiaki_get_ps_selected_addr(ChiakiHolepunchSession session, char *ps_ip)
{
	(void)session;
	if(ps_ip)
		*ps_ip = '\0';
}

CHIAKI_EXPORT uint16_t chiaki_get_ps_ctrl_port(ChiakiHolepunchSession session)
{
	(void)session;
	return 0;
}

CHIAKI_EXPORT chiaki_socket_t *chiaki_get_holepunch_sock(ChiakiHolepunchSession session, ChiakiHolepunchPortType type)
{
	(void)session; (void)type;
	return NULL;
}

CHIAKI_EXPORT ChiakiErrorCode holepunch_session_create_offer(ChiakiHolepunchSession session)
{
	(void)session;
	return CHIAKI_ERR_UNKNOWN;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_holepunch_session_punch_hole(ChiakiHolepunchSession session, ChiakiHolepunchPortType port_type)
{
	(void)session; (void)port_type;
	return CHIAKI_ERR_UNKNOWN;
}

CHIAKI_EXPORT void chiaki_holepunch_session_fini(ChiakiHolepunchSession session)
{
	(void)session;
}
