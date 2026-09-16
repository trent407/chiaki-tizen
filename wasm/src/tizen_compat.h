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
// Debug instrumentation below (section 5) needs plain C stdio/string, not
// <cstdio>/<cstring>, since this header is force-included into chiaki-ng's
// .c translation units (takion.c, http.c, regist.c), not just C++ files.
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward-declared so item 1 (socket()) below can tune/log SO_RCVBUF on every
// socket chiaki-ng opens; full definitions are in section 5.
static inline void ct_debug_tune_socket(int fd, int domain, int type, int protocol);

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
	int fd = socket(domain, type, protocol);
	if(fd >= 0)
		ct_debug_tune_socket(fd, domain, type, protocol); // TEMPORARY: Tizen 9 debugging
	return fd;
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

// ---------------------------------------------------------------------------
// 5. TEMPORARY debug instrumentation — 2024/Tizen 9 FEC-failure regression.
//    Works on a 2021 Tizen 8 unit, corrupts video with FEC/missing-unit
//    errors on a 2024 Tizen 9 unit after its OS update. This section logs
//    the actual UDP receive path (takion.c's data socket, where those FEC
//    failures originate) to tell real network loss apart from a Tizen 9
//    socket-layer regression. Remove once diagnosed.
// ---------------------------------------------------------------------------

static inline int ct_debug_get_rcvbuf(int fd)
{
	int val = 0;
	socklen_t len = sizeof(val);
	if(getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &val, &len) == 0)
		return val;
	return -1;
}

// Tizen 9 reports from the field show contiguous chiaki FEC-unit loss after
// the stream is already connected. One plausible platform regression is a
// smaller or less forgiving UDP receive queue under bursty 1080p/60 video.
// Ask for headroom on UDP sockets at creation time, then log the effective
// value so user photos can tell whether the runtime honored it.
static inline void ct_debug_tune_socket(int fd, int domain, int type, int protocol)
{
	int clean_type = type;
#ifdef SOCK_NONBLOCK
	clean_type &= ~SOCK_NONBLOCK;
#endif
#ifdef SOCK_CLOEXEC
	clean_type &= ~SOCK_CLOEXEC;
#endif
	int before = ct_debug_get_rcvbuf(fd);
	if(domain == AF_INET && clean_type == SOCK_DGRAM)
	{
		int requested = 4 * 1024 * 1024;
		int r = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &requested, sizeof(requested));
		int e = r == 0 ? 0 : errno;
		int after = ct_debug_get_rcvbuf(fd);
		fprintf(stderr,
			"[ct_sock] fd=%d udp rcvbuf before=%d request=%d result=%d errno=%d after=%d proto=%d\n",
			fd, before, requested, r, e, after, protocol);
	}
	else
	{
		fprintf(stderr, "[ct_sock] fd=%d type=%d proto=%d SO_RCVBUF=%d bytes\n",
			fd, clean_type, protocol, before);
	}
}

// Logs every FAILED socket receive (return <= 0) with the raw errno, on every
// socket chiaki-ng reads from — including takion's video/audio data socket,
// where the FEC failures originate. Deliberately silent on success: at 60fps a
// single frame's UDP fragments mean dozens of successful reads a second, which
// would flood the 500-line #ct-debug-log ring buffer before a real failure
// ever showed up in it.
static inline ssize_t ct_debug_recv(int sockfd, void *buf, size_t len, int flags)
{
	ssize_t n = recv(sockfd, buf, len, flags);
	if(n <= 0)
		fprintf(stderr, "[ct_sock] recv fd=%d len=%zu -> %zd errno=%d (%s)\n",
			sockfd, len, n, errno, strerror(errno));
	return n;
}

static inline ssize_t ct_debug_recvfrom(int sockfd, void *buf, size_t len, int flags,
	struct sockaddr *src_addr, socklen_t *addrlen)
{
	ssize_t n = recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
	if(n <= 0)
		fprintf(stderr, "[ct_sock] recvfrom fd=%d len=%zu flags=0x%x -> %zd errno=%d (%s)\n",
			sockfd, len, flags, n, errno, strerror(errno));
	return n;
}

static inline ssize_t ct_debug_recvmsg(int sockfd, struct msghdr *msg, int flags)
{
	ssize_t n = recvmsg(sockfd, msg, flags);
	if(n <= 0)
		fprintf(stderr, "[ct_sock] recvmsg fd=%d flags=0x%x -> %zd errno=%d (%s)\n",
			sockfd, flags, n, errno, strerror(errno));
	return n;
}
#define recv(sockfd, buf, len, flags) ct_debug_recv((sockfd), (buf), (len), (flags))
#define recvfrom(sockfd, buf, len, flags, src_addr, addrlen) \
	ct_debug_recvfrom((sockfd), (buf), (len), (flags), (src_addr), (addrlen))
#define recvmsg(sockfd, msg, flags) ct_debug_recvmsg((sockfd), (msg), (flags))

#endif // __EMSCRIPTEN__ && CHIAKI_TIZEN_WASM
#endif // CHIAKI_TIZEN_COMPAT_H
