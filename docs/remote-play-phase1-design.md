# Remote Play over the internet — Phase 1 design

Companion to `remote-play-over-internet-plan.md`. This is the concrete design
after reading chiaki-ng's `holepunch.c` and researching Tizen's networking
model. It supersedes the "do all signaling in JavaScript" framing of the
original plan — one hard constraint (below) moves the WebSocket into WASM.

## 0. The finding that shaped this design

chiaki-ng opens the PSN push channel as an authenticated WebSocket:

```
wss://<fqdn>/np/pushNotification
  Authorization: Bearer <oauth_token>
  Sec-WebSocket-Protocol: np-pushpacket
  X-PSN-APP-TYPE: REMOTE_PLAY   (+ 5 more X-PSN-* headers, User-Agent)
```

The browser/Tizen `WebSocket` API **cannot set custom request headers** — its
constructor takes only a URL and subprotocols. Sony requires `Authorization`
and the `X-PSN-*` headers on the upgrade, so **the push WebSocket cannot run in
pure JS.** It must run where we control the handshake: in WASM, over the Tizen
Sockets Extension, with TLS from mbedTLS (which is already linked into the
module). The REST calls have no such limit — `fetch()` sets arbitrary headers
fine — so those can stay in JS.

This is exactly the kind of blocker the spike existed to find before writing
code.

## 1. The protocol (extracted from holepunch.c)

Auth is `Authorization: Bearer <oauth_token>` on every call. Endpoints (all
Sony HTTPS):

| Purpose | Endpoint |
|---|---|
| List consoles | `web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/clients` |
| Push WS FQDN | `mobile-pushcl.np.communication.playstation.net/np/serveraddr` |
| Create session | `web.np.playstation.com/api/sessionManager/v1/remotePlaySessions` |
| Session message | `.../remotePlaySessions/{id}/sessionMessage` |
| Wake (PS4) | user-profile baseUrl + `/v1/users/{id}/remoteConsole/wakeUp` |
| Leave session | `.../remotePlaySessions/{id}/members/me` |
| Push channel | `wss://{fqdn}/np/pushNotification` |

Flow:

1. **Auth** — obtain a PSN OAuth Bearer token (see §4).
2. **Create session** — `POST remotePlaySessions` with a push-context id.
3. **Open push WS** — `GET serveraddr` for the FQDN, then open
   `wss://{fqdn}/np/pushNotification` with the headers above. Notifications
   arrive as JSON: `sessionMessage:created`, `members:created`, etc.
4. **Signaling** — the console and client exchange `sessionMessage`s carrying
   `{action: OFFER|ACCEPT|RESULT, reqId, connRequest}` where `connRequest` holds
   `{sid, peerSid, skey, natType, candidate[], localPeerAddr, localHashedId}`.
   Candidates are `{type: STATIC|LOCAL|STUN|DERIVED, addr, mappedAddr, port,
   mappedPort}`. Bodies are cleartext JSON wrapped as
   `"ver=1.0, type=text, body=<json>"` — no GCM on the signaling layer.
5. **STUN** — discover our public mapping (server list is pulled from a public
   always-online-stun GitHub list; we can hardcode a few instead).
6. **Punch** — blast UDP at the peer's candidates (`SELECT_CANDIDATE_TRIES=20`,
   0.5s each), pick the first that answers, hand the connected UDP socket to
   chiaki's session as `rudp_sock` / holepunch session.
7. **Session** — from here it's the same takion/ctrl/RUDP path as local play.
   RUDP is already compiled in (plan step 1, done).

## 2. Corrected architecture

```
┌──────────────── JavaScript (app/js/psn-signaling.js) ─────────────────┐
│  • OAuth token: store, refresh, paste-in UX                           │
│  • REST calls via fetch() (headers OK): create session, sessionMessage│
│  • JSON build/parse (native), candidate/offer/answer orchestration    │
│         │  ct_psn_* ccalls              ▲ events (ws notifications,    │
│         ▼                                │  punch result)             │
├──────────────── WASM (wasm/src/psn_transport.*) ──────────────────────┤
│  • WSS push client: mbedTLS (linked) + minimal WS framing over the    │
│    Tizen Sockets Extension — the ONE thing JS can't do                │
│  • STUN binding + UDP hole-punch over the socket extension            │
│  • Hands the punched chiaki_socket_t to the holepunch adapter         │
├──────────────── WASM (wasm/src/holepunch_bridge.c) ───────────────────┤
│  • Replaces holepunch_stub.c's 7 holepunch fns with a real adapter    │
│    backed by data the transport layer produced (ps_ip, ctrl_port,     │
│    punched sock) so session.c takes the remote path unmodified        │
└────────────────────────────────────────────────────────────────────────┘
```

RUDP (`chiaki_rudp_*`) is already real (plan §3a). Only the pieces above are new.

## 3. Dependency swap (vs. porting holepunch.c's stack)

We do **not** port libcurl/libevent/json-c/miniupnpc (infeasible on the 2019
fastcomp SDK). Instead:

| holepunch.c uses | We use | Notes |
|---|---|---|
| libcurl (HTTPS) | fetch() in JS **+** mbedTLS client in WASM for the WS | REST → JS; WSS → WASM |
| libcurl (WebSocket) | mbedTLS + ~200 lines of WS framing in WASM | the custom-header blocker |
| json-c | JS JSON (REST) + jsmn single-header parser in WASM (WS frames) | requests are already string templates in holepunch.c |
| libevent | chiaki's existing stop-pipe + select loop | already ported for local play |
| miniupnpc | **dropped for v1** | STUN + port-guessing covers most NATs |

Net new C: a small mbedTLS-backed HTTPS/WSS client, a WS framer, jsmn, STUN,
punch, and the holepunch adapter. Substantial but bounded — and it reuses the
socket extension and mbedTLS that already work.

## 4. Auth UX — phase 1 keeps it dumb on purpose

Full in-app PSN OAuth (webview login, redirect capture, token refresh) is a UX
project of its own and is **not** required to validate the hard parts. For
phase 1: the user obtains a PSN OAuth token out-of-band (chiaki-ng desktop, or
the documented Sony login-URL → paste-redirect flow) and pastes it into the app
once, exactly like the PSN Account ID field works today. Token storage +
in-app login is a later phase. This lets us test signaling + punch immediately.

## 5. JS ↔ WASM interface (new ct_* surface)

- `ct_psn_ws_open(token, fqdn)` → open the authenticated push WS in WASM;
  emits `psnNotification` events (JSON) up to JS.
- `ct_psn_ws_close()`.
- `ct_psn_stun_gather(server_json)` → returns local/STUN candidates to JS.
- `ct_psn_punch(candidates_json, skey)` → attempts the hole punch; emits
  `psnPunchResult` with the chosen candidate; retains the connected socket.
- `ct_session_start_remote(...)` → like `ct_session_start` but wires the
  punched socket + holepunch adapter into `ChiakiConnectInfo`.

JS owns the REST calls and the state machine; WASM owns the WS + UDP.

## 6. config.xml changes

Add WARP access + navigation for the PSN domains (Tizen packaged apps bypass
web-page CORS via WARP + the internet privilege — the mechanism Samsung's own
Moonlight port relies on):

```xml
<access origin="https://web.np.playstation.com" subdomains="true"/>
<access origin="https://mobile-pushcl.np.communication.playstation.net" subdomains="true"/>
<access origin="https://asm.np.community.playstation.net" subdomains="true"/>
<access origin="https://auth.api.sonyentertainmentnetwork.com" subdomains="true"/>
```

(The WSS endpoint is opened from WASM/mbedTLS over a raw socket, so it isn't
governed by WARP — but keep the list for the fetch() REST calls.)

## 7. De-risking sequence — validate before building the whole thing

Each step gates the next; stop and rethink if one fails.

1. **REST reachability (JS).** From the installed app, `fetch()` one PSN
   endpoint (e.g. `serveraddr`) with a valid Bearer token and confirm a real
   JSON response, not a CORS/TLS failure. Proves WARP + token work. ~½ day.
2. **WSS from WASM.** Open `wss://{fqdn}/np/pushNotification` from a throwaway
   mbedTLS+WS client in WASM with the full header set; confirm the upgrade
   succeeds and a keep-alive frame arrives. This is the single riskiest new
   component — prove it in isolation. ~1 week.
3. **Socket-extension punch viability.** Confirm the Tizen UDP socket preserves
   source port across sends (hole punching depends on it) with a STUN binding
   round-trip. ~2–3 days.
4. Only then build the full signaling state machine, holepunch adapter, and
   `ct_session_start_remote`.

## 8. Open questions / risks

- **WSS-in-WASM is the crux.** mbedTLS handshake + WS framing over the socket
  extension is unproven here; step 2 above must clear before committing.
- **STUN/punch on Tizen** depends on source-port preservation (step 3).
- **Token lifetime.** Pasted tokens expire; phase 1 tolerates re-paste,
  refresh comes later.
- **Protocol drift.** This tracks Sony's undocumented API; chiaki-ng upstream
  is the reference to re-sync against if it breaks.
- **Effort.** Roughly 6–9 focused weeks to a first remote session with the
  paste-token shortcut, most of it in steps 2 + the signaling state machine.

## 9. Recommendation

Proceed, but gate on the §7 sequence — the WSS-from-WASM spike (step 2) is the
make-or-break and should be built as a standalone throwaway before any of the
signaling logic. RUDP is already in place; the transport client is the next
real build.
