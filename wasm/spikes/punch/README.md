# Hole-punch spike

Reproduces chiaki-ng's `check_candidates()` candidate-check protocol: the
88-byte UDP request/response the client and PS5 exchange to find a working
direct path, then adopt that socket for the stream. This is what turns the
peer's candidate IP:ports (learned over the WS signaling channel) plus our own
STUN mapping into a live UDP connection.

## Files

- `punch.h` / `punch.c` — 88-byte request/response build + parse (exact offsets
  from holepunch.c: type 0x00, hashed ids 0x04/0x24, sids 0x44/0x46, request id
  0x4b) and `punch_run()`: blast every candidate on one UDP socket, select-recv,
  match by request id + source address, select the winner, send confirm.
- `punch_test.c` — packet unit tests + a full punch round trip over UDP
  loopback against an in-process fake console.
- `spike-punch.command` — double-click on macOS to compile and run.

## Status

- ✅ Compiles clean; all tests pass, including the **loopback punch**: with a
  dead decoy candidate plus a reachable one, `punch_run` selects the candidate
  that actually answered. Validates the wire format and the
  send/select/match/confirm mechanics over real UDP.
- ⏳ On device: the real PS5 answers only when the packet carries valid
  `hashed_id_local/console` + `sid_local/console` derived during signaling, and
  the socket must be created via the Tizen Sockets Extension. That's the
  end-to-end on-hardware test.

## Notes / simplifications vs. full holepunch.c

- One request id per punch (chiaki's `CHECK_CANDIDATES_REQUEST_NUMBER = 1`).
- No STUN "random allocation" port-guessing fan-out (many sockets) yet — that's
  a NAT-hard-case optimization; your network already showed endpoint-independent
  mapping (STUN spike), so the base punch should suffice for v1.
- IPv4 only.
