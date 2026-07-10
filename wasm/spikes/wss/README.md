# WSS-from-WASM spike

Proves the one over-the-internet component that cannot live in JavaScript: the
authenticated PSN push WebSocket. The Tizen/browser `WebSocket` API can't set
custom request headers (`Authorization: Bearer`, `Sec-WebSocket-Protocol`,
`X-PSN-*`), which Sony requires on the upgrade — so this runs in WASM with
mbedTLS (already linked) and hand-rolled RFC 6455 framing. See
`docs/remote-play-phase1-design.md` §0.

## Files

- `ws_client.h` / `ws_client.c` — the client: TCP → TLS (mbedTLS) → WebSocket
  upgrade with arbitrary headers → masked frame send / recv (handles
  ping/pong/close). Transport-agnostic; the TCP BIO is swappable.
- `ws_client_test.c` — offline unit tests (RFC 6455 vectors) + an optional live
  handshake to any wss server.
- `spike-wss.command` — double-click on macOS: compiles against the mbedTLS 2.28
  fetched by `./build.sh wasm`, runs the unit tests, then a live handshake to a
  public echo server.

## Status

- ✅ Compiles against mbedTLS 2.28 (exact version the module links).
- ✅ Unit tests pass: `Sec-WebSocket-Accept` RFC vector, masked-"Hello" RFC
  vector, and 7/16/64-bit length encodings.
- ⏳ Live handshake: run `spike-wss.command` on a networked machine (the build
  sandbox is offline). Validates TLS + upgrade + Accept + frame round-trip on a
  real server.
- ⏳ On Tizen: swap `mbedtls_net_connect` + `mbedtls_ssl_set_bio` for a BIO over
  the Tizen Sockets Extension fd (the TLS/WS logic is unchanged), then point it
  at Sony's push FQDN with a real Bearer token. This is design §7 step 2.

## Not production-ready

`verify_peer=0` (no cert check) is set for the spike. Production MUST bundle
Sony's CA roots and set `verify_peer=1` — otherwise the TLS is MITM-able.
