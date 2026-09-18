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
 * STATUS: experimental. The OAuth login/account-id helper and signaling state
 * machine mirror chiaki-ng's Remote Play path. Remaining known work is tracked
 * in docs/remote-play-integration-status.md, chiefly socket reuse for STUN +
 * punch, console DUID lookup/cache, and on-device PSN bring-up.
 */
'use strict';

(function() {

var PSN = {
  clientId: 'ba495a24-818c-472b-b12d-ff231c1b5745',
  clientSecret: 'mvaiZkRsAsI1IBkY',
  redirectUri: 'https://remoteplay.dl.playstation.net/remoteplay/redirect',
  scope: 'psn:clientapp referenceDataService:countryConfig.read pushNotification:webSocket.desktop.connect sessionManager:remotePlaySession.system.update',
  tokenUrl: 'https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token',
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
  token: null,        // PSN OAuth2 bearer token saved via Sony login
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

// --- PSN OAuth --------------------------------------------------------------
function formEncode(obj) {
  var parts = [];
  for (var k in obj)
    parts.push(encodeURIComponent(k) + '=' + encodeURIComponent(obj[k]));
  return parts.join('&');
}

function basicAuthHeader() {
  return 'Basic ' + btoa(PSN.clientId + ':' + PSN.clientSecret);
}

function loginUrl() {
  return 'https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize?'
    + formEncode({
      service_entity: 'urn:service-entity:psn',
      response_type: 'code',
      client_id: PSN.clientId,
      redirect_uri: PSN.redirectUri,
      scope: PSN.scope,
      request_locale: 'en_US',
      ui: 'pr',
      service_logo: 'ps',
      layout_type: 'popup',
      smcid: 'remoteplay',
      prompt: 'always',
      PlatformPrivacyWs1: 'minimal'
    }) + '&';
}

function parseRedirectCode(input) {
  input = String(input || '').trim();
  if (!input) throw new Error('missing redirect URL');
  var m = input.match(/[?&]code=([^&#]+)/);
  if (m && m[1]) return decodeURIComponent(m[1].replace(/\+/g, ' '));
  if (/^[A-Za-z0-9._~-]+$/.test(input)) return input;
  throw new Error('could not find code in redirect URL');
}

function saveTokenBundle(json) {
  if (!json || !json.access_token)
    throw new Error('PSN token response did not include an access token');
  saveToken(json.access_token);
  if (json.refresh_token)
    localStorage.setItem('psnRefreshToken', json.refresh_token);
  var expiresIn = parseInt(json.expires_in, 10);
  if (expiresIn > 0)
    localStorage.setItem('psnTokenExpiresAt', String(Date.now() + Math.max(0, expiresIn - 60) * 1000));
  return json.access_token;
}

function saveAccountIds(decimal) {
  decimal = String(decimal || '').replace(/\D/g, '');
  if (!decimal) return '';
  var b64 = decimalAccountIdToBase64(decimal);
  if (b64) localStorage.setItem('psnRemoteAccountId', b64);
  localStorage.setItem('psnRemoteAccountIdDecimal', decimal);
  state.accountId = decimal;
  return b64;
}

function tokenPost(body) {
  return fetch(PSN.tokenUrl, {
    method: 'POST',
    headers: {
      Authorization: basicAuthHeader(),
      'Content-Type': 'application/x-www-form-urlencoded'
    },
    body: formEncode(body)
  }).then(function(r) {
    if (!r.ok) throw new Error('PSN token request failed: ' + r.status);
    return r.json();
  }).then(saveTokenBundle);
}

function exchangeRedirect(input) {
  var code = parseRedirectCode(input);
  return tokenPost({
    grant_type: 'authorization_code',
    code: code,
    scope: PSN.scope,
    redirect_uri: PSN.redirectUri
  }).then(function(accessToken) {
    return fetchAccountId(accessToken).then(function(accountIdB64) {
      return { token: accessToken, accountIdB64: accountIdB64 };
    });
  });
}

function refreshToken() {
  var refresh = localStorage.getItem('psnRefreshToken') || '';
  if (!refresh) return Promise.reject(new Error('no PSN refresh token saved'));
  return tokenPost({
    grant_type: 'refresh_token',
    refresh_token: refresh,
    scope: PSN.scope,
    redirect_uri: PSN.redirectUri
  });
}

function ensureAccessToken() {
  var token = loadToken();
  if (!token) return Promise.reject(new Error('no PSN login saved'));
  var expiry = parseInt(localStorage.getItem('psnTokenExpiresAt') || '0', 10);
  if (expiry && Date.now() > expiry)
    return refreshToken();
  return Promise.resolve(token);
}

function fetchAccountId(accessToken) {
  return fetch(PSN.tokenUrl + '/' + encodeURIComponent(accessToken), {
    headers: {
      Authorization: basicAuthHeader(),
      Accept: 'application/json'
    }
  }).then(function(r) {
    if (!r.ok) throw new Error('PSN account request failed: ' + r.status);
    return r.json();
  }).then(function(json) {
    return saveAccountIds(json && json.user_id);
  });
}

function decimalAccountIdToBase64(decimal) {
  decimal = String(decimal || '').replace(/\D/g, '');
  if (!decimal) return '';
  var bytes = [];
  for (var i = 0; i < 8; i++) {
    var q = '';
    var rem = 0;
    for (var j = 0; j < decimal.length; j++) {
      var n = rem * 10 + (decimal.charCodeAt(j) - 48);
      var d = Math.floor(n / 256);
      rem = n % 256;
      if (q || d) q += String(d);
    }
    bytes.push(rem);
    decimal = q || '0';
  }
  return bytesToB64(new Uint8Array(bytes));
}

function accountIdBase64ToDecimal(b64) {
  var bin;
  try { bin = atob(b64 || ''); } catch (e) { return ''; }
  if (!bin) return '';
  var bytes = [];
  for (var i = 0; i < bin.length; i++) bytes.push(bin.charCodeAt(i) & 0xff);
  bytes.reverse();
  var dec = '0';
  for (var b = 0; b < bytes.length; b++) {
    var carry = bytes[b];
    var out = '';
    for (var j = dec.length - 1; j >= 0; j--) {
      var n = (dec.charCodeAt(j) - 48) * 256 + carry;
      out = String(n % 10) + out;
      carry = Math.floor(n / 10);
    }
    while (carry) {
      out = String(carry % 10) + out;
      carry = Math.floor(carry / 10);
    }
    dec = out.replace(/^0+/, '') || '0';
  }
  return dec === '0' ? '' : dec;
}

function loadToken() {
  state.token = localStorage.getItem('psnOAuthToken') || null;
  state.accountId = localStorage.getItem('psnRemoteAccountIdDecimal')
    || accountIdBase64ToDecimal(localStorage.getItem('psnRemoteAccountId'));
  return state.token;
}
function saveToken(t) {
  state.token = t;
  localStorage.setItem('psnOAuthToken', t);
}

function clearTokens() {
  state.token = null;
  state.accountId = null;
  localStorage.removeItem('psnOAuthToken');
  localStorage.removeItem('psnRefreshToken');
  localStorage.removeItem('psnTokenExpiresAt');
  localStorage.removeItem('psnRemoteAccountId');
  localStorage.removeItem('psnRemoteAccountIdDecimal');
  localStorage.removeItem('psnAccountId'); // legacy shared key from test builds
  localStorage.removeItem('psnAccountIdDecimal');
}

function authHeaders(extra) {
  var h = { 'Authorization': 'Bearer ' + state.token };
  if (extra) for (var k in extra) h[k] = extra[k];
  return h;
}

function normName(s) {
  return String(s || '').trim().toLowerCase();
}

function loadSavedConsoles() {
  try { return JSON.parse(localStorage.getItem('consoles') || '[]'); }
  catch (e) { return []; }
}

function saveConsoleDuid(consoleEntry, duid) {
  if (!consoleEntry || !duid) return;
  consoleEntry.duid = duid;
  var list = loadSavedConsoles();
  var changed = false;
  list.forEach(function(c) {
    var sameId = consoleEntry.id && c.id === consoleEntry.id;
    var sameHostAccount = c.host === consoleEntry.host
      && (c.accountB64 || '') === (consoleEntry.accountB64 || '');
    if (sameId || sameHostAccount) {
      c.duid = duid;
      changed = true;
    }
  });
  if (changed)
    localStorage.setItem('consoles', JSON.stringify(list));
}

function parsePsnDevices(json, platform) {
  var clients = json && Array.isArray(json.clients) ? json.clients : [];
  return clients.map(function(client) {
    var device = client.device || {};
    var features = Array.isArray(device.enabledFeatures) ? device.enabledFeatures : [];
    return {
      duid: String(client.duid || '').toLowerCase(),
      name: device.name || client.name || '',
      platform: platform,
      remoteplay: features.indexOf('remotePlay') >= 0
    };
  }).filter(function(d) { return d.duid && d.remoteplay; });
}

function fetchDeviceList(platform) {
  var url = PSN.deviceList + '?' + formEncode({
    platform: platform,
    includeFields: 'device',
    limit: 10,
    offset: 0
  });
  return getJson(url).then(function(json) {
    return parsePsnDevices(json, platform);
  });
}

function chooseDevice(devices, consoleEntry) {
  if (!devices.length) return null;
  var wantedDuid = String(consoleEntry.duid || '').toLowerCase();
  if (wantedDuid) {
    for (var i = 0; i < devices.length; i++)
      if (devices[i].duid === wantedDuid) return devices[i];
  }
  var names = [
    consoleEntry.nickname,
    consoleEntry.label,
    consoleEntry.discoveredName
  ].map(normName).filter(Boolean);
  for (var n = 0; n < names.length; n++) {
    for (var d = 0; d < devices.length; d++)
      if (normName(devices[d].name) === names[n]) return devices[d];
  }
  return devices.length === 1 ? devices[0] : null;
}

function ensureConsoleDuid(consoleEntry) {
  if (consoleEntry.duid) return Promise.resolve(consoleEntry.duid);
  var platform = consoleEntry.ps5 ? 'PS5' : 'PS4';
  return fetchDeviceList(platform).then(function(devices) {
    var device = chooseDevice(devices, consoleEntry);
    if (!device)
      throw new Error('could not match this console to a PSN Remote Play device');
    saveConsoleDuid(consoleEntry, device.duid);
    return device.duid;
  });
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
      port: c.localPort || c.port,
      mappedPort: c.mappedPort || c.port
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
  state.localCandidates = [];
  state.remoteCandidates = [];
  var T = window.ChiakiTizen.psnTransport;

  return ensureAccessToken()
    .then(function() { return ensureConsoleDuid(consoleEntry); })
    .then(function(duid) { state.console.duid = duid; })
    .then(createSession)
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
      if (stun && Array.isArray(stun.candidates))
        state.localCandidates = stun.candidates.map(function(c) {
          return {
            type: 'STATIC',
            ip: c.ip,
            port: c.port,
            mappedPort: c.port,
            localPort: c.localPort || c.port
          };
        });
      else if (stun && stun.ip)
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
  clearTokens: clearTokens,
  loginUrl: loginUrl,
  exchangeRedirect: exchangeRedirect,
  refreshToken: refreshToken,
  ensureAccessToken: ensureAccessToken,
  connectRemote: connectRemote,
  onNotification: onNotification,
  onTransportEvent: onTransportEvent,
  state: state
};

})();
