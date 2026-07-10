// chiaki-tizen: compatibility shim for the Samsung Tizen Sockets Extension.
// Force-included into every chiaki translation unit via `-include tizen_compat.h`.
//
// Why this exists (per Samsung's WASM docs & their Moonlight port notes):
//   1. fcntl() is not implemented by the Tizen Sockets Extension. Non-blocking
//      sockets must be created with SOCK_NONBLOCK at socket() time.
//   2. The runtime reports "would block" with the WASI errno value
//      (__WASI_ERRNO_AGAIN == 6), which may not match the EAGAIN your libc
//      headers define. Same story for EINPROGRESS on non-blocking connect().
//   3. poll()/select() must not mix socket and file descriptors. chiaki's
//      stop pipe is switched to a UDP loopback socket (the Switch codepath)
//      via -DCHIAKI_TIZEN_WASM, so all polled descriptors are sockets.
//   4. Socket APIs must not be called from the main browser thread; the
//      module is linked with -s PROXY_TO_PTHREAD so main() itself runs in a
//      worker, and all chiaki I/O already happens on chiaki's own pthreads.

#ifndef CHIAKI_TIZEN_COMPAT_H
#define CHIAKI_TIZEN_COMPAT_H

#if defined(__EMSCRIPTEN__) && defined(CHIAKI_TIZEN_WASM)

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// 1. socket(): always create non-blocking sockets.
//
// chiaki guards every blocking-style recv/send/connect with a stop-pipe
// select, and its EAGAIN/EWOULDBLOCK handling paths (http.c, regist.c,
// takion.c) tolerate spurious would-block results, so unconditionally
// non-blocking sockets are safe here — and they are the only way to get
// non-blocking behavior on this platform at all (see fcntl below).
// ---------------------------------------------------------------------------
static inline int chiaki_tizen_socket(int domain, int type, int protocol)
{
#ifdef SOCK_NONBLOCK
	type |= SOCK_NONBLOCK;
#endif
	// Note: `socket` is not yet redefined at this point in the file.
	return socket(domain, type, protocol);
}

// ---------------------------------------------------------------------------
// 2. fcntl(): the extension lacks it. chiaki only ever calls
//    fcntl(fd, F_GETFL/F_SETFL) to toggle O_NONBLOCK, which is already the
//    permanent state of every socket we create, so a no-op shim that reports
//    success preserves the semantics chiaki expects.
// ---------------------------------------------------------------------------
static inline int chiaki_tizen_fcntl(int fd, int cmd, long arg)
{
	(void)fd; (void)arg;
	if(cmd == F_GETFL)
		return O_NONBLOCK;
	if(cmd == F_SETFL)
		return 0;
	return 0;
}

// ---------------------------------------------------------------------------
// 3. setsockopt(): Samsung's socket runtime exposes Linux MTU-discovery
//    constants through the SDK headers, but the TV runtime returns
//    ENOPROTOOPT for IP_MTU_DISCOVER. Chiaki uses it only to toggle the UDP
//    Don't Fragment bit for MTU probing; lack of support should degrade MTU
//    accuracy, not abort the whole Takion stream connection.
// ---------------------------------------------------------------------------
static inline int chiaki_tizen_setsockopt(int sockfd, int level, int optname,
	const void *optval, socklen_t optlen)
{
#ifdef IP_MTU_DISCOVER
	if(level == IPPROTO_IP && optname == IP_MTU_DISCOVER)
	{
		(void)sockfd;
		(void)optval;
		(void)optlen;
		return 0;
	}
#endif
	return setsockopt(sockfd, level, optname, optval, optlen);
}

#ifdef __cplusplus
}
#endif

#define socket(domain, type, protocol) chiaki_tizen_socket((domain), (type), (protocol))
// chiaki calls fcntl with exactly three args in all cases we compile.
#define fcntl(fd, cmd, ...) chiaki_tizen_fcntl((fd), (cmd), (0, ##__VA_ARGS__))
#define setsockopt(sockfd, level, optname, optval, optlen) chiaki_tizen_setsockopt((sockfd), (level), (optname), (optval), (optlen))

// ---------------------------------------------------------------------------
// 4. errno remapping. Samsung's runtime sets WASI errno values on socket
//    calls. If your Samsung Emscripten SDK's <errno.h> already agrees with
//    these (newer SDKs do), this block is a no-op numerically. Verified
//    against the values used by Samsung's own Moonlight port.
//    Disable with -DCHIAKI_TIZEN_REMAP_ERRNO=0 if your SDK disagrees.
// ---------------------------------------------------------------------------
#ifndef CHIAKI_TIZEN_REMAP_ERRNO
#define CHIAKI_TIZEN_REMAP_ERRNO 1
#endif

#if CHIAKI_TIZEN_REMAP_ERRNO
#define CHIAKI_TIZEN_WASI_EAGAIN 6
#define CHIAKI_TIZEN_WASI_EINPROGRESS 26
#if defined(EAGAIN) && (EAGAIN != CHIAKI_TIZEN_WASI_EAGAIN)
#undef EAGAIN
#define EAGAIN CHIAKI_TIZEN_WASI_EAGAIN
#endif
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != CHIAKI_TIZEN_WASI_EAGAIN)
#undef EWOULDBLOCK
#define EWOULDBLOCK CHIAKI_TIZEN_WASI_EAGAIN
#endif
#if defined(EINPROGRESS) && (EINPROGRESS != CHIAKI_TIZEN_WASI_EINPROGRESS)
#undef EINPROGRESS
#define EINPROGRESS CHIAKI_TIZEN_WASI_EINPROGRESS
#endif
#endif // CHIAKI_TIZEN_REMAP_ERRNO

#endif // __EMSCRIPTEN__ && CHIAKI_TIZEN_WASM
#endif // CHIAKI_TIZEN_COMPAT_H
