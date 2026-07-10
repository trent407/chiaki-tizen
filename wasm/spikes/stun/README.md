# STUN spike

Discovers our public IP:port (server-reflexive mapping) via a STUN Binding
Request — the candidate we offer the console during PSN hole-punching — and, on
Tizen, is where we'll verify the Sockets Extension keeps the UDP source port
stable (hole punching depends on it). Pure UDP, no dependencies.

## Files

- `stun.h` / `stun.c` — build a Binding Request, parse XOR-MAPPED-ADDRESS (and
  plain MAPPED-ADDRESS), and a full UDP `stun_query()` reporting the public
  mapping plus the local source port.
- `stun_test.c` — offline unit tests (RFC 5769 vector) + optional live query.
- `spike-stun.command` — double-click on macOS: compiles, runs the unit tests,
  then queries real STUN servers and prints your public mapping.

## Status

- ✅ Compiles clean; offline unit tests pass: Binding Request layout,
  XOR-MAPPED-ADDRESS decode (RFC 5769 → 192.0.2.1:32853), MAPPED-ADDRESS
  fallback, and txid-mismatch rejection.
- ⏳ Live query: run `spike-stun.command` on a networked machine (the build
  sandbox has no UDP egress).
- ⏳ On Tizen: create the UDP socket via the Sockets Extension and confirm the
  source port the STUN server reports matches the local bound port across
  repeated sends (design §7 step 3). This is the hole-punch prerequisite.

## Next

STUN gives one endpoint's mapping. Hole punching then blasts UDP at the peer's
candidates (learned via the WS signaling) until one answers, and hands the
connected socket to chiaki. That's the next module: the punch loop + the
holepunch adapter that replaces `holepunch_stub.c`.
