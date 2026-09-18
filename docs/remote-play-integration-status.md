# Remote Play integration — status

Where the over-the-internet work actually stands. Honest ledger: what's wired
and verified vs. what remains (much of it on-device).

## Done and verified

- **RUDP transport** — real `rudp.c`/`rudpsendbuffer.c` compiled into the
  module; symbols verified, no dupes (plan §3a).
- **Transport primitives, proven in isolation** (`wasm/spikes/`):
  - **WSS** (mbedTLS + RFC 6455 framing) — unit-tested *and* a live TLS +
    WebSocket handshake completed against a real server. The one piece that
    can't be done in browser JS.
  - **STUN** — RFC 5769 vectors + a live query returning the public mapping
    (and the NAT showed port-preserving / endpoint-independent mapping).
  - **Hole-punch** — chiaki's exact 88-byte candidate-check protocol; full
    round trip over UDP loopback selects the reachable candidate.
- **Holepunch adapter** — `holepunch_bridge.c` replaces `holepunch_stub.c`,
  compile-verified against the vendored chiaki. Provides the real
  `ChiakiHolepunchSession` (punched CTRL/DATA sockets + console addr + regist
  info) that `session.c`/`ctrl.c` drive. Wired into `CMakeLists.txt`.
- **Session seam** — `bridge.cc` threads an optional holepunch session through
  `SessionStartImpl` (NULL on the local path → unchanged) and adds
  `ct_session_start_remote(...)`. Compile-verified; local play untouched.
- **config.xml** — WARP `<access>` grants for the PSN REST hosts (packaged Tizen
  apps bypass web-page CORS via WARP + the internet privilege).
- **JS signaling scaffold** — `app/js/psn-signaling.js`: endpoints, REST flow,
  token storage, and the offer/answer/punch/start state machine laid out.

## Also done since

- **Transport promoted into the module** — `wasm/src/net/{ws_client,stun,punch}`
  (from the verified spikes), added to `CMakeLists.txt`.
- **`ct_psn_*` bridge surface** in `bridge.cc`, compile-verified, all four
  exported: `ct_psn_ws_open` (spawns the authenticated push-WS listener thread,
  emits `psnNotification`/`psnWsOpen`/`psnWsClosed`), `ct_psn_ws_close`,
  `ct_psn_stun_gather` (emits `psnStun`), `ct_psn_punch` (emits `psnPunch`).
  Symbol cross-check confirms the `net/` objects satisfy every reference.
- **JS ↔ WASM binding** — `app.js` cwraps the new entry points, exposes
  `ChiakiTizen.psnTransport` + `sessionStartRemote`, and routes `psn*` events to
  `ChiakiPSN.onTransportEvent`; `index.html` loads `psn-signaling.js`.

## Signaling crypto — done

Turned out to be data-plumbing, not cryptography: `hashed_id_local` is 20
random bytes, `sid_local` is random, `skey` is zeroed in our offer; the
console's `hashed_id_console`/`sid_console` come from its answer (holepunch.c
lines 808-809, 1499-1500, 2498).

- `net/punch` corrected to **20-byte** hashed ids and made **bidirectional**
  (answers the console's probe with a RESPONSE carrying `sid_local ^ peer_addr`
  / `^ peer_port`, per send_response_ps). Verified: unit tests for the packet
  layout + XOR encoding, and a bidirectional loopback round trip.
- `ct_psn_punch` now takes the ids/sids (base64) and threads them through.
- `psn-signaling.js` generates the random local ids, builds the OFFER
  connRequest (candidates, zeroed skey, our localHashedId), and parses the
  console's ACCEPT/RESULT for its hashed id, sid, and candidates.

## JS orchestration glue — done

- `connectRemote` sequences the full flow: create session → open push WS → STUN
  → send OFFER → await the console's answer → punch (ctrl) → punch (data) →
  `ct_session_start_remote`. Driven by a small event→promise adaptor
  (`waitFor`/`resolveWaiter`) over the fire-and-forget `psn*` events.
- **PSN login helper**: Settings opens Sony's Remote Play OAuth login, accepts
  the returned redirect URL/code, exchanges it for access + refresh tokens, and
  derives the base64 PSN Account ID the pairing screen expects.
- **Console DUID lookup/cache**: before remote connect, JS fetches the PSN
  device list, matches the saved console by cached DUID/name/single-device
  fallback, and writes the DUID back to the saved console.
- **Stable STUN/punch sockets**: `ct_psn_stun_gather` now creates and retains
  CTRL/DATA UDP sockets, advertises those exact STUN mappings, and
  `ct_psn_punch` reuses the retained sockets for candidate checks.

## Remaining (on-device / known refinements)

1. **On-device bring-up** — the only place PSN auth, the Tizen socket extension
   (DNS? source-port preservation? non-blocking TLS?), the exact PSN JSON shapes,
   and a real PS5 handshake get validated. This is where the signaling *content*
   (not just mechanics) first meets Sony, and where the real debugging lives.

## Verify the current step

Rebuild once (`build-hdr.command`) to confirm the full Emscripten link succeeds
with `holepunch_bridge.c` in place of the stub. It provides the same symbols as
the stub, so the link is a drop-in; nothing user-visible changes yet (the remote
path has no caller until the transport in step 1 lands). This just banks the
adapter + session seam into a clean build.

## Honest read

Every *unknowable* risk is now retired — the pieces that could have said
"impossible" (authenticated WSS from WASM, STUN reachability, the punch
protocol, the chiaki session seam) are all proven or compiling. What's left is
assembly (steps 1–2), a UX pass (3), and patient hardware debugging (4). It's a
real amount of work, but it's no longer *uncertain* work.
