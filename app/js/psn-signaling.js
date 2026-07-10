/* chiaki-tizen — PSN remote-play signaling (JavaScript half).
 *
 * Architecture (see docs/remote-play-phase1-design.md):
 *   - JS (this file): PSN OAuth token, the REST calls (fetch — headers OK),
 *     JSON build/parse, and the offer/answer/candidate state machine.
 *   - WASM (psn_transport, TODO): the authenticated push WebSocket (mbedTLS +
 *     framing — the browser WebSocket API can't set the required headers),
 *     STUN, and the UDP hole-punch. The proven spikes for those live in
 *     wasm/spikes/{wss,stun,punch}/ and are being promoted into the module.
 *
 * STATUS: scaffold. The REST flow and state machine are laid out with the real
 * endpoints and payload templates from chiaki-ng's holepunch.c. Three seams are
 * not yet filled and are marked TODO:
 *   (1) window.ChiakiTizen.psnTransport.* — the ct_psn_* WASM bridge (WS/STUN/
 *       punch). Until it exists, openPushChannel()/runPunch() are stubs.
 *   (2) the signaling crypto: hashed_id derivation, skey, sid assignment
 *       (holepunch.c get_client_addr_* / the connreq builders).
 *   (3) in-app OAuth. For phase 1 the user pastes a token (like the account id).
 */
'use strict';

(function() {

var PSN = {
  deviceList: 'https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/clients',
  wsFqdn:     'https://mobile-pushcl.np.communication.playstation.net/np/serveraddr?version=2.1&fields=keepAliveStatus&keepAliveStatusType=3',
  sessionCreate: 'https://web.np.playstation.com/api/sessionManager/v1/remotePlaySessions',
  sessionMessage: function(id) {
    return 'https://web.np.playstation.com/api/sessionManager/v1/remotePlaySessions/' + id + '/sessionMessage';
  },
  leave: function(id) {
    return 'https://web.np.playstation.com/api/sessionManager/v1/remotePlaySessions/' + id + '/members/me';
  }
};

var state = {
  token: null,        // PSN OAuth2 bearer token (phase 1: pasted)
  accountId: null,    // int64 as string
  sessionId: null,    // remote play session UUID
  wsFqdn: null,
  phase: 'idle',      // idle | creating | ws | offer | answer | punching | starting | error
  console: null,      // { host, ps5, registKeyB64, rpKeyB64, duid }
  localCandidates: [],
  remoteCandidates: [],
  // signaling ids (chiaki holepunch.c): hashed_id_local is 20 random bytes and
  // sid_local is random — NOT derived. skey is zeroed in our offer. The console
  // values arrive in its answer message.
  hashedLocalB64: null,
  sidLocal: 0,
  hashedConsoleB64: null,
  sidConsole: 0,
  reqId: 0
};

// 20 random bytes -> base64 ; random 16-bit sid. Generated once per session.
function generateLocalIds() {
  var id = new Uint8Array(20);
  (window.crypto || window.msCrypto).getRandomValues(id);
  state.hashedLocalB64 = bytesToB64(id);
  var s = new Uint16Array(1);
  (window.crypto || window.msCrypto).getRandomValues(s);
  state.sidLocal = s[0];
  state.reqId = Math.floor(Math.random() * 0x7fffffff);
}

function bytesToB64(u8) {
  var s = '';
  for (var i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
  return btoa(s);
}
var ZERO_SKEY_B64 = bytesToB64(new Uint8Array(16)); // 16 zero bytes

// --- token (phase 1: paste; phase 2: OAuth) --------------------------------
function loadToken() {
  state.token = localStorage.getItem('psnOAuthToken') || null;
  return state.token;
}
function saveToken(t) {
  state.token = t;
  localStorage.setItem('psnOAuthToken', t);
}

function authHeaders(extra) {
  var h = { 'Authorization': 'Bearer ' + state.token };
  if (extra) for (var k in extra) h[k] = extra[k];
  return h;
}

// --- REST -------------------------------------------------------------------
function getJson(url) {
  return fetch(url, { headers: authHeaders() }).then(function(r) {
    if (!r.ok) throw new Error('GET ' + url + ' -> ' + r.status);
    return r.json();
  });
}
function postJson(url, body) {
  return fetch(url, {
    method: 'POST',
    headers: authHeaders({ 'Content-Type': 'application/json' }),
    body: JSON.stringify(body)
  }).then(function(r) {
    if (!r.ok) throw new Error('POST ' + url + ' -> ' + r.status);
    return r.status === 204 ? {} : r.json();
  });
}

// 1. Create a remote play session and learn the push-WS FQDN.
function createSession() {
  state.phase = 'creating';
  var pushCtxId = cryptoRandomHex(8); // TODO: match the official app's format
  var body = { remotePlaySessions: [ { members: [ {
    accountId: 'me', deviceUniqueId: 'me', platform: 'me',
    pushContexts: [ { pushContextId: pushCtxId } ]
  } ] } ] };
  return postJson(PSN.sessionCreate, body).then(function(res) {
    // TODO: extract sessionId from res (shape per holepunch.c parsing)
    state.sessionId = res && res.remotePlaySessions && res.remotePlaySessions[0]
      ? res.remotePlaySessions[0].sessionId : null;
    return getJson(PSN.wsFqdn);
  }).then(function(fqdnRes) {
    state.wsFqdn = fqdnRes && (fqdnRes.fqdn || fqdnRes.data && fqdnRes.data.fqdn);
    return state.wsFqdn;
  });
}

// 2. Open the authenticated push WebSocket — IN WASM (JS can't set the headers).
function openPushChannel() {
  state.phase = 'ws';
  var T = window.ChiakiTizen && window.ChiakiTizen.psnTransport;
  if (!T || !T.wsOpen) {
    // TODO(2): implement ct_psn_ws_open in the WASM transport. Headers required:
    //   Authorization: Bearer <token>
    //   Sec-WebSocket-Protocol: np-pushpacket
    //   X-PSN-APP-TYPE: REMOTE_PLAY   (+ X-PSN-APP-VER / -OS-VER / -PROTOCOL-VERSION
    //                                    / -KEEP-ALIVE-STATUS-TYPE / -RECONNECTION)
    return Promise.reject(new Error('WASM push-WS transport not wired yet'));
  }
  return T.wsOpen(state.token, state.wsFqdn); // emits notifications -> onNotification
}

// Build the connRequest JSON object (mirrors session_connrequest_fmt).
function buildConnRequest() {
  var candidates = state.localCandidates.map(function(c) {
    return {
      type: c.type || 'STATIC',
      addr: c.ip, mappedAddr: c.ip,
      port: c.port, mappedPort: c.port
    };
  });
  return {
    sid: state.sidLocal,
    peerSid: state.sidConsole,      // 0 until the console answers
    skey: ZERO_SKEY_B64,            // chiaki zeroes skey in the offer
    natType: 2,
    candidate: candidates,
    defaultRouteMacAddr: '00:00:00:00:00:00',
    localPeerAddr: { accountId: String(state.accountId || 0), platform: 'REMOTE_PLAY' },
    localHashedId: state.hashedLocalB64
  };
}

// 3. Send our OFFER (candidates) as a sessionMessage; the console answers via a
//    push notification -> onNotification(). Mirrors session_message_fmt wrapped
//    in the "ver=1.0, type=text, body=..." envelope.
function sendOffer() {
  state.phase = 'offer';
  generateLocalIds();
  var body = JSON.stringify({
    action: 'OFFER', reqId: state.reqId, error: 0, connRequest: buildConnRequest()
  });
  var envelope = {
    channel: 'remote_play:1',
    payload: 'ver=1.0, type=text, body=' + body,
    to: [ { accountId: String(state.accountId || 0),
            deviceUniqueId: state.console.duid || '',
            platform: state.console.ps5 ? 'PS5' : 'PS4' } ]
  };
  return postJson(PSN.sessionMessage(state.sessionId), envelope);
}

// Push notifications arrive here (forwarded from the WASM WS via an event).
// The console's ACCEPT/RESULT carries its localHashedId, sid, and candidates.
function onNotification(json) {
  try {
    var body = extractBody(json);       // parse "ver=1.0,...,body=<json>"
    if (!body || !body.connRequest) return;
    var cr = body.connRequest;
    if (cr.localHashedId) state.hashedConsoleB64 = cr.localHashedId;
    if (typeof cr.sid === 'number') state.sidConsole = cr.sid;
    if (Array.isArray(cr.candidate)) {
      state.remoteCandidates = cr.candidate.map(function(c) {
        return { type: c.type, ip: c.addr, port: c.port,
                 mappedAddr: c.mappedAddr, mappedPort: c.mappedPort };
      });
    }
    if (body.action === 'ACCEPT' || body.action === 'RESULT') {
      state.phase = 'answer';
      resolveWaiter('answer', body);
    }
  } catch (e) {}
}

function extractBody(json) {
  // The push message embeds the session message; find the "body=" JSON.
  var s = typeof json === 'string' ? json : JSON.stringify(json);
  var i = s.indexOf('body=');
  if (i < 0) return null;
  var jstr = s.slice(i + 5);
  // trim a trailing quote/brace tail from the envelope if present
  try { return JSON.parse(jstr); } catch (e) {}
  var end = jstr.lastIndexOf('}');
  return end > 0 ? JSON.parse(jstr.slice(0, end + 1)) : null;
}

// 4. Punch to the console's candidates (candidate-check) IN WASM, passing the
//    signaling ids. portType: 0=ctrl, 1=data.
function runPunch(portType) {
  state.phase = 'punching';
  var T = window.ChiakiTizen && window.ChiakiTizen.psnTransport;
  if (!T || !T.punch) return Promise.reject(new Error('WASM punch transport not wired'));
  var csv = state.remoteCandidates.map(function(c) { return c.ip + ':' + c.port; }).join(',');
  T.punch(csv, portType | 0, state.hashedLocalB64, state.hashedConsoleB64,
    state.sidLocal, state.sidConsole);
  // Result returns asynchronously via the psnPunch event (onTransportEvent).
  return Promise.resolve();
}

// 5. Hand the punched sockets to chiaki and start the stream.
function startRemote(punchResult) {
  state.phase = 'starting';
  var c = state.console;
  var s = window.ChiakiTizen.loadStreamSettingsForRemote
    ? window.ChiakiTizen.loadStreamSettingsForRemote() : { resolution: 3, fps: 30, hdr: false };
  // ct_session_start_remote(ps5, registKey, rpKey, res, fps, hdr, ctrlFd, dataFd, psIp, psCtrlPort)
  return window.ChiakiTizen.sessionStartRemote(
    c.ps5 ? 1 : 0, c.registKeyB64, c.rpKeyB64,
    s.resolution, s.fps, s.hdr ? 1 : 0,
    punchResult.ctrlFd, punchResult.dataFd, punchResult.psIp, punchResult.psCtrlPort);
}

// --- event -> promise adaptor ----------------------------------------------
// The ct_psn_* calls are fire-and-forget; their results arrive as psn* events.
// waitFor() bridges a single pending event into the async flow.
var STUN_HOST = 'stun.l.google.com';
var STUN_PORT = '19302';
var waiters = {};
function waitFor(key, timeoutMs) {
  return new Promise(function(resolve, reject) {
    var to = setTimeout(function() {
      delete waiters[key];
      reject(new Error('timeout waiting for ' + key));
    }, timeoutMs || 15000);
    waiters[key] = function(val) { clearTimeout(to); delete waiters[key]; resolve(val); };
  });
}
function resolveWaiter(key, val) { if (waiters[key]) waiters[key](val); }

// Full flow orchestrator: session -> push WS -> STUN -> offer -> await answer
// -> punch (ctrl) -> punch (data) -> start.
function connectRemote(consoleEntry) {
  state.console = consoleEntry;
  if (!loadToken()) return Promise.reject(new Error('no PSN token — paste one in Settings first'));
  state.localCandidates = [];
  state.remoteCandidates = [];
  var T = window.ChiakiTizen.psnTransport;

  return createSession()
    .then(function() {
      if (!state.wsFqdn) throw new Error('no push-WS FQDN from PSN');
      T.wsOpen(state.token, state.wsFqdn);
      return waitFor('psnWsOpen', 15000);
    })
    .then(function() {
      T.stunGather(STUN_HOST, STUN_PORT);
      return waitFor('psnStun', 8000);
    })
    .then(function(stun) {
      if (stun && stun.ip)
        state.localCandidates.push({ type: 'STATIC', ip: stun.ip, port: stun.port });
      return sendOffer(); // REST; the console answers over the push WS
    })
    .then(function() { return waitFor('answer', 30000); })
    .then(function() {
      var csv = candidatesCsv();
      T.punch(csv, 0, state.hashedLocalB64, state.hashedConsoleB64, state.sidLocal, state.sidConsole);
      return waitFor('psnPunch', 15000);
    })
    .then(function(ctrl) {
      state._ctrl = ctrl;
      T.punch(candidatesCsv(), 1, state.hashedLocalB64, state.hashedConsoleB64,
        state.sidLocal, state.sidConsole);
      return waitFor('psnPunch', 15000);
    })
    .then(function(data) {
      var sel = (state._ctrl && typeof state._ctrl.selected === 'number') ? state._ctrl.selected : 0;
      var chosen = state.remoteCandidates[sel] || state.remoteCandidates[0] || {};
      return startRemote({
        ctrlFd: state._ctrl.fd, dataFd: data.fd,
        psIp: chosen.ip || '', psCtrlPort: chosen.port || 9295
      });
    })
    .catch(function(e) { state.phase = 'error'; throw e; });
}

function candidatesCsv() {
  return state.remoteCandidates.map(function(c) { return c.ip + ':' + c.port; }).join(',');
}

function cryptoRandomHex(nbytes) {
  var a = new Uint8Array(nbytes);
  (window.crypto || window.msCrypto).getRandomValues(a);
  return Array.prototype.map.call(a, function(b) {
    return ('0' + b.toString(16)).slice(-2);
  }).join('');
}

// Transport events from the WASM layer (routed by app.js _onEvent). Frame data
// arrives base64-encoded in ev.data.
function onTransportEvent(ev) {
  switch (ev.type) {
    case 'psnWsOpen': state.phase = 'ws'; resolveWaiter('psnWsOpen', ev); break;
    case 'psnWsClosed': if (state.phase !== 'starting') state.phase = 'idle'; break;
    case 'psnNotification':
      try { onNotification(JSON.parse(atobUtf8(ev.data))); } catch (e) {}
      break;
    case 'psnStun':
      if (ev.ip) resolveWaiter('psnStun', ev);
      break;
    case 'psnPunch':
      if (ev.error) resolveWaiter('psnPunch', ev); // let the flow surface the error
      else resolveWaiter('psnPunch', ev);          // { fd, selected, portType }
      break;
  }
}

function atobUtf8(b64) {
  var bin = atob(b64);
  try {
    return decodeURIComponent(bin.split('').map(function(c) {
      return '%' + ('00' + c.charCodeAt(0).toString(16)).slice(-2);
    }).join(''));
  } catch (e) { return bin; }
}

// Expose for the app UI + the WASM transport to call back into.
window.ChiakiPSN = {
  saveToken: saveToken,
  loadToken: loadToken,
  connectRemote: connectRemote,
  onNotification: onNotification,
  onTransportEvent: onTransportEvent,
  state: state
};

})();
