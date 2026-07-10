# Remote Play over the internet — implementation plan

Status: **proposal / not started.** This documents what it would take to add PSN
remote (over-the-internet) play to chiaki-tizen, which is currently local-network
only. It is scoped work, not a polish item — expect it to dwarf everything else
in the port so far.

## 1. Where we are today

Local play works because chiaki's session, ctrl, takion, regist and discovery
code only need raw UDP/TCP sockets, which the Tizen Sockets Extension provides.
The over-the-internet path is deliberately stubbed out in
`wasm/src/holepunch_stub.c`: every `chiaki_holepunch_*` and `chiaki_rudp_*`
symbol returns `CHIAKI_ERR_UNKNOWN` or `NULL`, and `SessionStartImpl` in
`bridge.cc` always sets `connect_info.holepunch_session = nullptr` and
`connect_info.rudp_sock = nullptr`, so chiaki never enters those code paths.

## 2. What "remote play over the internet" actually is

When the console and client are not on the same LAN, chiaki-ng does three
distinct things before the normal streaming session can start:

1. **PSN signaling.** Authenticate to Sony's PSN account servers and open a
   WebSocket to their signaling service. Through it, the client and console
   exchange connection metadata (an offer/answer, plus ICE-style candidate
   addresses). This is HTTPS REST + a WebSocket carrying JSON.
2. **NAT traversal (hole punching).** Using STUN to discover each side's
   public address/port, and optionally UPnP-IGD to open a port on the local
   router, the two peers punch a UDP hole so packets can flow directly. When
   direct traversal fails, chiaki falls back to port-guessing heuristics.
3. **RUDP control transport.** Over the punched socket, the early control
   handshake runs on chiaki's own reliable-UDP layer (`chiaki_rudp_*`) instead
   of the plain TCP ctrl channel used on a LAN. Once the stream is up, takion
   (the media transport) runs the same as local play.

Only after all three succeed does `connect_info` get a live
`holepunch_session` + `rudp_sock`, and the existing session code takes over.

## 3. The dependency problem — and why it splits in two

The stub header names the blockers: chiaki-ng's `holepunch.c` depends on
**libcurl (with WebSocket support), libevent, miniupnpc and json-c**, none of
which exist in the Samsung Emscripten sandbox. That framing is correct but
hides a useful distinction:

- **RUDP (`chiaki_rudp_*`) needs none of those libraries.** `rudp.c` and
  `rudpsendbuffer.c` are pure protocol code over a `chiaki_socket_t` and a
  stop pipe — exactly the primitives this port already made work for local
  play. They are stubbed today only because they were excluded from the WASM
  build alongside holepunch, not because they can't compile.
- **Only `holepunch.c` needs the heavy libraries**, because it is the piece
  that talks to Sony's servers (curl/websocket/json) and to the router
  (miniupnpc) and runs the STUN state machine (libevent).

So the work is really two efforts of very different size.

### 3a. RUDP — small — ✅ DONE

Un-stubbed: chiaki-ng's real `remote/rudp.c` and `remote/rudpsendbuffer.c` are
now compiled into the WASM module (`wasm/CMakeLists.txt`), and the
`chiaki_rudp_*` stubs were removed from `holepunch_stub.c` (holepunch stubs
kept). Verified: both sources compile clean against the vendored chiaki-ng with
the Tizen defines + compat shim; `rudp.c` defines all `chiaki_rudp_*` symbols
with no duplicate/missing symbols vs. the trimmed stub; and neither references
libcurl/libevent/json-c/holepunch. It is inert until a holepunch session
provides a socket, so local play is unaffected.

### 3b. PSN signaling + NAT traversal — large

This is the real project. There are two architectures.

**Option A — port the C libraries to Emscripten (keep `holepunch.c` as-is).**
Build libcurl (with the WebSocket option, which is recent and experimental),
libevent, json-c and miniupnpc for `wasm32-emscripten`, then compile
chiaki-ng's `holepunch.c` unmodified. Upside: no reimplementation, exercises
the same code the desktop client ships. Downsides are severe: libcurl's
WebSocket support is bleeding-edge and its TLS/DNS/event backends all assume
POSIX facilities that Emscripten only partially provides; libevent's core
(epoll/kqueue) has no clean WASM analogue; miniupnpc needs raw multicast. This
is a long, uncertain porting slog with a real chance of a dead end on any one
of the four.

**Option B — do the signaling in JavaScript, bridge results into WASM
(recommended).** A Tizen web app already has first-class `fetch()`, the
`WebSocket` API, and native JSON — i.e. everything `holepunch.c` uses libcurl +
json-c for, but built into the platform and far more reliable than porting them.
The plan:

- Implement the PSN OAuth/account flow and the signaling WebSocket in a new
  `app/js/psn-signaling.js`, in plain JS. It performs the REST calls, opens the
  WebSocket, and exchanges the offer/answer + candidates.
- Do STUN binding and the actual UDP hole-punch **in WASM**, over the Tizen
  Sockets Extension, since that is where the raw UDP socket that the media
  session will use must live. STUN binding requests are a few dozen bytes of
  UDP; no library needed.
- Replace `holepunch_stub.c` with a real bridge that (a) receives the peer
  address/port candidates from JS via a small `ct_*` C ABI, (b) drives the
  STUN + punch over the WASM socket, and (c) hands the resulting
  `chiaki_socket_t` back to chiaki as `connect_info.rudp_sock` /
  `holepunch_session`.
- Treat UPnP (miniupnpc) as optional. STUN + port guessing (chiaki already has
  `chiaki_holepunch_session_force_port_guessing` and configurable guess ports)
  covers most NATs; UPnP is a later success-rate improvement, not a
  prerequisite.

Option B trades "reimplement Sony's signaling protocol in JS" for "port four
hostile C libraries into WASM." The signaling protocol is finite and
observable (it mirrors chiaki-ng's `holepunch.c`, which is the reference), the
JS platform APIs are solid, and it keeps the ugliest dependencies out of the
build entirely. It is the path most likely to actually ship.

## 4. Phased plan (Option B)

1. **RUDP un-stub (spike, ~1 day). ✅ DONE** — see 3a. Real `rudp.c` /
   `rudpsendbuffer.c` compiled in, stubs dropped, symbols verified. Confirm the
   full Emscripten link on the next `./build.sh all` (inert, no behavior change).
2. **PSN auth + account ID in JS (~1 week).** Implement the OAuth login and
   account-ID retrieval the app currently expects the user to paste in. This is
   independently useful — it removes the manual "PSN Account ID (base64)" step
   on the pairing screen even for local play.
3. **Signaling WebSocket in JS (~2–3 weeks).** Port `holepunch.c`'s REST +
   WebSocket message flow to `psn-signaling.js`: create session, list devices,
   exchange offer/answer and candidates. Reference the chiaki-ng source
   message-by-message. Deliverable: JS can complete a signaling handshake and
   surface the peer candidate list.
4. **STUN + punch in WASM (~2–3 weeks).** New bridge replacing the holepunch
   stub: STUN binding over the Tizen UDP socket, hole-punch against the
   candidates from step 3, expose the punched socket to chiaki. Wire
   `connect_info.holepunch_session` / `rudp_sock`.
5. **End-to-end bring-up + fallbacks (~2+ weeks, hardware-bound).** Connect a
   real remote session; add port-guessing fallback; handle the reconnect and
   teardown paths. This phase is gated entirely on testing against a real
   console over a real internet path (two different networks), which is slow.
6. **UPnP (optional, later).** Only if NAT-traversal success rate on real
   networks proves too low without it.

## 5. Testing strategy

None of this is verifiable from the native mock-header check, which only covers
the local API surface. Realistic verification needs: a PSN account with remote
play enabled; the console on one network and the TV on another (or a phone
hotspot to force a non-LAN path); and packet capture on both ends to compare
the signaling exchange against a known-good chiaki-ng desktop session. Budget
for the fact that hole punching fails silently in many ways and most debugging
is "why did no UDP packet arrive."

## 6. Risks and open questions

- **Sony can change the signaling protocol** at any time; this tracks an
  undocumented, moving target. chiaki-ng upstream is the canary.
- **CORS / TLS from a Tizen web app** to PSN endpoints may need the app's
  network privileges widened in `config.xml`, and some PSN endpoints may reject
  a browser-origin request — to be confirmed empirically in step 2/3.
- **STUN/punch reliability under Tizen's socket extension** is unproven; the
  extension's UDP semantics (source-port preservation across sends) are exactly
  what hole punching depends on and must be validated early.
- **Effort estimate is wide.** Roughly 8–12 focused weeks for Option B to a
  first working remote session, most of it in steps 3–5, and most of the
  schedule risk in hardware bring-up. Option A could be shorter if every library
  ports cleanly and much longer (or infeasible) if one doesn't.

## 7. Recommendation

Do step 1 (RUDP un-stub) soon regardless — it is cheap and clears half the
stub. Treat steps 2–5 as a distinct project with its own milestones, pursue
Option B (JS signaling + WASM STUN/punch), and keep the local-play build as the
shipping default until a remote session works end-to-end on hardware.
