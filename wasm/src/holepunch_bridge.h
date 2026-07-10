// chiaki-tizen: holepunch adapter (replaces holepunch_stub.c).
//
// Instead of porting chiaki-ng's libcurl/libevent/json-c holepunch stack, the
// PSN signaling runs in JavaScript and the STUN + hole-punch run in the WASM
// transport (see docs/remote-play-phase1-design.md). By the time a remote
// session starts, we already hold: a punched CTRL socket, a punched DATA
// socket, the console's chosen address + ctrl port, and the registration info
// from signaling. This adapter wraps those into the ChiakiHolepunchSession that
// chiaki's session.c drives via chiaki_get_holepunch_sock / _regist_info /
// _ps_selected_addr / _ps_ctrl_port and the create_offer / punch_hole calls.

#ifndef CHIAKI_TIZEN_HOLEPUNCH_BRIDGE_H
#define CHIAKI_TIZEN_HOLEPUNCH_BRIDGE_H

#include <chiaki/remote/holepunch.h>

#ifdef __cplusplus
extern "C" {
#endif

// Build a holepunch session from already-punched sockets and the
// signaling-derived console address / registration info. Ownership of the
// sockets transfers to the session (closed in chiaki_holepunch_session_fini).
// Returns NULL on allocation failure.
ChiakiHolepunchSession ct_holepunch_bridge_new(
	chiaki_socket_t ctrl_sock, chiaki_socket_t data_sock,
	const char *ps_ip, uint16_t ps_ctrl_port,
	const ChiakiHolepunchRegistInfo *regist_info);

#ifdef __cplusplus
}
#endif
#endif // CHIAKI_TIZEN_HOLEPUNCH_BRIDGE_H
