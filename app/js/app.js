/* chiaki-tizen — app logic.
 *
 * Talks to the WASM module through the ct_* C ABI (Module.ccall) and receives
 * events back as JSON via window.ChiakiTizen._onEvent(). All heavy lifting
 * (protocol, crypto, A/V) happens in WASM / the TV media pipeline; this file
 * is UI state, remote-control navigation and controller input.
 */
'use strict';

(function() {

// ---------------------------------------------------------------------------
// Chiaki controller button bitmask (must match chiaki/controller.h)
// ---------------------------------------------------------------------------
var BTN = {
  CROSS: 1 << 0, MOON: 1 << 1, BOX: 1 << 2, PYRAMID: 1 << 3,
  DPAD_LEFT: 1 << 4, DPAD_RIGHT: 1 << 5, DPAD_UP: 1 << 6, DPAD_DOWN: 1 << 7,
  L1: 1 << 8, R1: 1 << 9, L3: 1 << 10, R3: 1 << 11,
  OPTIONS: 1 << 12, SHARE: 1 << 13, TOUCHPAD: 1 << 14, PS: 1 << 15
};

// Tizen TV remote keycodes
var KEY = {
  LEFT: 37, UP: 38, RIGHT: 39, DOWN: 40, ENTER: 13, BACK: 10009,
  RED: 403, GREEN: 404, YELLOW: 405, BLUE: 406,
  PLAY: 415, PAUSE: 19, PLAYPAUSE: 10252, STOP: 413,
  REWIND: 412, FASTFORWARD: 417, EXIT: 10182,
  CHANNEL_UP: 427, CHANNEL_DOWN: 428 // TEMPORARY: scroll #ct-debug-log
};

var C = null; // ccall shorthands, filled when the module is ready
var REMOTE_PLAY_INTERNET_ENABLED = true;
var PSN_AUTH_ENABLED = true;
var PSN_RAW_TOKEN_FIELD_ENABLED = false;
var LOCAL_PAIRING_ACCOUNT_KEY = 'localPairingAccountId';

function samsungTvYearFromModel(text) {
  text = String(text || '').toUpperCase();
  // Common Samsung TV codes: T=2020, A=2021, B=2022, C=2023, D=2024,
  // F=2025. Examples: QN90C, S95D, LS03A, TU7000.
  var m = text.match(/\b(?:QN|QE|QA|UN|UE|UA|GU|GQ)?\d{2,3}(?:QN|Q|S|LST|LS)\d{2,4}(T|A|B|C|D|F)(?=\d|[A-Z]|$)/);
  if (!m)
    m = text.match(/\b(?:QN|QE|QA|UN|UE|UA|GU|GQ|KU|KD)?\d{2,3}[A-Z]*?(T|A|B|C|D|F)(?=\d|[A-Z]|$)/);
  if (!m)
    m = text.match(/\b(?:LST|LS|Q[A-Z]?|S)\d{2,3}(T|A|B|C|D|F)(?=\d|[A-Z]|$)/);
  if (!m)
    return null;
  var years = { T: 2020, A: 2021, B: 2022, C: 2023, D: 2024, F: 2025 };
  return years[m[1]] || null;
}

function detectTvModelYear() {
  var bits = [];
  if (window.tizen && tizen.systeminfo) {
    [
      'http://tizen.org/system/model_name',
      'http://tizen.org/system/model',
      'http://tizen.org/system/build.string'
    ].forEach(function(key) {
      try {
        var val = tizen.systeminfo.getCapability(key);
        if (val) bits.push(val);
      } catch (e) {}
    });
  }
  bits.push(navigator.userAgent || '');
  for (var i = 0; i < bits.length; i++) {
    var year = samsungTvYearFromModel(bits[i]);
    if (year) return year;
  }
  return null;
}

function applyAutoPerformanceUi() {
  var year = detectTvModelYear();
  state.tvModelYear = year;
  // Keep the fast UI for unknown/older sets. Only restore richer effects when
  // the model is confidently 2024+.
  if (year && year >= 2024)
    document.body.classList.remove('perf-ui');
  else
    document.body.classList.add('perf-ui');
  if (window.__ctReport)
    window.__ctReport('ui', 'performance mode ' +
      (document.body.classList.contains('perf-ui') ? 'on' : 'off') +
      (year ? ' tvYear=' + year : ' tvYear=unknown'));
}

var state = {
  moduleReady: false,
  moduleFailed: false,
  moduleStatus: 'Loading Chiaki core',
  screen: 'consoles',
  streaming: false,
  discovered: {},        // hostId -> discovery info
  consoleListSignature: '',
  consoleListRenderTimer: null,
  tvModelYear: null,
  registTargetHost: null,
  gamepadTimer: null,
  remoteButtons: 0       // buttons held via TV remote during stream
};

applyAutoPerformanceUi();

// ---------------------------------------------------------------------------
// Persistence (localStorage is fully supported in Tizen web apps)
// ---------------------------------------------------------------------------
function loadConsoles() {
  try { return JSON.parse(localStorage.getItem('consoles') || '[]'); }
  catch (e) { return []; }
}
function saveConsoles(list) {
  localStorage.setItem('consoles', JSON.stringify(list));
}
// A console's identity is host + PSN account, so the same console can be saved
// twice under two different accounts (each with its own registration).
function consoleId(host, accountB64) {
  return (host || '') + '|' + (accountB64 || ''); // '|' safe: not in IPv4/base64
}
function entryId(c) { return c.id || consoleId(c.host, c.accountB64); }

function upsertConsole(entry) {
  var list = loadConsoles();
  if (!entry.id) entry.id = consoleId(entry.host, entry.accountB64);
  var i = list.findIndex(function(c) { return entryId(c) === entry.id; });
  if (i >= 0) list[i] = Object.assign(list[i], entry);
  else list.push(entry);
  saveConsoles(list);
}
function removeConsoleById(id) {
  saveConsoles(loadConsoles().filter(function(c) { return entryId(c) !== id; }));
  state.consoleListSignature = '';
  renderConsoleList();
  if (typeof restartDiscovery === 'function') restartDiscovery();
}
function findConsoleById(id) {
  return loadConsoles().find(function(c) { return entryId(c) === id; });
}

function dedupeConsoles(list) {
  var out = [], seen = {};
  list.forEach(function(c) {
    var id = entryId(c);
    if (seen[id] == null) {
      seen[id] = out.length;
      out.push(c);
    } else {
      out[seen[id]] = Object.assign(out[seen[id]], c);
    }
  });
  return out;
}

// One-time prefill from a bundled prefill.json (for "personal" builds). Applies
// once per install — a flag guards it, so a console you later delete stays gone.
// An entry with registKeyB64 + rpKeyB64 becomes a ready-to-stream console; an
// entry with just host/account/type shows as unpaired, so you only enter the
// 8-digit link code once. Absent file (generic build) = no-op.
function applyPrefill() {
  if (localStorage.getItem('prefillApplied')) return;
  var done = function () { localStorage.setItem('prefillApplied', '1'); };
  fetch('prefill.json', { cache: 'no-store' })
    .then(function (r) { return (r && r.ok) ? r.json() : null; })
    .then(function (cfg) {
      if (!cfg) { done(); return; }
      var list = cfg.consoles || (cfg.host ? [cfg] : []);
      list.forEach(function (c) {
        if (!c.host) return;
        var entry = {
          host: String(c.host).trim(),
          ps5: !!c.ps5,
          accountB64: c.accountId || c.accountB64 || '',
          label: c.label || '',
          nickname: c.nickname || c.label || String(c.host).trim()
        };
        if (c.registKeyB64 && c.rpKeyB64) {
          entry.registKeyB64 = c.registKeyB64;
          entry.rpKeyB64 = c.rpKeyB64;
          if (c.target != null) entry.target = c.target;
        }
        upsertConsole(entry);
      });
      if (cfg.psnAccountId) localStorage.setItem(LOCAL_PAIRING_ACCOUNT_KEY, cfg.psnAccountId);
      if (cfg.psnToken && window.ChiakiPSN) window.ChiakiPSN.saveToken(cfg.psnToken);
      if (cfg.settings) {
        var s = loadStreamSettings();
        saveStreamSettings({
          resolution: cfg.settings.resolution != null ? cfg.settings.resolution : s.resolution,
          fps: cfg.settings.fps != null ? cfg.settings.fps : s.fps,
          hdr: cfg.settings.hdr != null ? !!cfg.settings.hdr : s.hdr
        });
      }
      done();
      renderConsoleList();
      if (typeof restartDiscovery === 'function') restartDiscovery();
    })
    .catch(function () { done(); });
}

// Decode chiaki's PSN Account ID (8-byte little-endian uint64, base64) to its
// decimal number. NOT the PSN username — that needs Sony's servers. No BigInt
// (older Tizen WebKit lacks it).
function accountIdDecimal(b64) {
  var bin;
  try { bin = atob(b64); } catch (e) { return null; }
  if (!bin || bin.length === 0) return null;
  var bytes = [];
  for (var i = 0; i < bin.length; i++) bytes.push(bin.charCodeAt(i) & 0xff);
  bytes.reverse(); // little-endian -> MSB first
  var dec = [0];
  for (var b = 0; b < bytes.length; b++) {
    var carry = bytes[b];
    for (var d = 0; d < dec.length; d++) {
      var val = dec[d] * 256 + carry;
      dec[d] = val % 10; carry = Math.floor(val / 10);
    }
    while (carry > 0) { dec.push(carry % 10); carry = Math.floor(carry / 10); }
  }
  return dec.reverse().join('');
}
function loadStreamSettings() {
  var res = parseInt(localStorage.getItem('streamResolution') || '3', 10);
  var fps = parseInt(localStorage.getItem('streamFps') || '30', 10);
  if (res !== 3 && res !== 4) res = 3;
  if (fps !== 30 && fps !== 60) fps = 30;
  var hdr = localStorage.getItem('streamHdr') === '1';
  return { resolution: res, fps: fps, hdr: hdr };
}
function saveStreamSettings(settings) {
  localStorage.setItem('streamResolution', String(settings.resolution));
  localStorage.setItem('streamFps', String(settings.fps));
  localStorage.setItem('streamHdr', settings.hdr ? '1' : '0');
}

function hideSplash() {
  var splash = document.getElementById('app-splash');
  if (splash) splash.classList.add('hidden');
}

function setModuleStatus(text, failed) {
  state.moduleStatus = text || state.moduleStatus;
  if (failed) state.moduleFailed = true;
  // Only surface something on the splash for a genuine failure. Routine boot
  // chatter (preflight checks, xhr/instantiate progress, pthread pool
  // counts, etc.) still lands in #ct-debug-log (Blue key) for diagnostics,
  // but shouldn't flash under the title on every normal, successful boot.
  if (failed) {
    var splash = document.getElementById('app-splash');
    if (splash) {
      var el = splash.querySelector('.module-status');
      if (!el) {
        el = document.createElement('p');
        el.className = 'module-status';
        splash.appendChild(el);
      }
      el.textContent = state.moduleStatus;
    }
  }
  var status = document.getElementById('regist-status');
  if (status && state.screen === 'regist' && !state.moduleReady) {
    status.textContent = state.moduleStatus;
    status.className = failed ? 'status err' : 'status';
  }
}

function moduleWaitMessage() {
  if (state.moduleFailed)
    return state.moduleStatus;
  return state.moduleStatus || 'Chiaki is still starting. Try again in a moment.';
}

function diagnosticTrail() {
  if (!window.__ctDiagnostics || !window.__ctDiagnostics.length)
    return '';
  return window.__ctDiagnostics.join(' | ');
}

// ---------------------------------------------------------------------------
// WASM bridge
// ---------------------------------------------------------------------------
window.ChiakiTizen = {
  _onModuleReady: function() {
    if (C) return;
    C = {
      init: Module.cwrap('ct_init', 'number', []),
      discoveryStart: Module.cwrap('ct_discovery_start', 'number', ['string', 'string']),
      discoveryStop: Module.cwrap('ct_discovery_stop', null, []),
      wakeup: Module.cwrap('ct_wakeup', 'number', ['string', 'string', 'number']),
      registStart: Module.cwrap('ct_regist_start', 'number', ['string', 'number', 'string', 'number']),
      registStop: Module.cwrap('ct_regist_stop', null, []),
      // HDR-aware 7-arg entry point. The last param (hdr) was added after the
      // wasm was last rebuilt on some machines, so a matching 6-arg wrapper is
      // kept as a fallback — see sessionStart() below.
      sessionStart: Module.cwrap('ct_session_start', 'number',
        ['string', 'number', 'string', 'string', 'number', 'number', 'number']),
      sessionStartNoHdr: Module.cwrap('ct_session_start', 'number',
        ['string', 'number', 'string', 'string', 'number', 'number']),
      sessionStop: Module.cwrap('ct_session_stop', null, []),
      setLoginPin: Module.cwrap('ct_session_set_login_pin', null, ['string']),
      controllerState: Module.cwrap('ct_controller_state', null,
        ['number', 'number', 'number', 'number', 'number', 'number', 'number']),
      // PSN over-the-internet transport (see app/js/psn-signaling.js)
      sessionStartRemote: Module.cwrap('ct_session_start_remote', 'number',
        ['number', 'string', 'string', 'number', 'number', 'number',
         'number', 'number', 'string', 'number']),
      psnWsOpen: Module.cwrap('ct_psn_ws_open', 'number', ['string', 'string']),
      psnWsClose: Module.cwrap('ct_psn_ws_close', null, []),
      psnStunGather: Module.cwrap('ct_psn_stun_gather', 'number', ['string', 'string']),
      psnPunch: Module.cwrap('ct_psn_punch', 'number',
        ['string', 'number', 'string', 'string', 'number', 'number'])
    };

    // Expose the transport to the PSN signaling module (fire-and-forget; the
    // results come back as psn* events routed in _onEvent below).
    window.ChiakiTizen.psnTransport = {
      wsOpen: function(token, fqdn) { return C.psnWsOpen(token, fqdn); },
      wsClose: function() { return C.psnWsClose(); },
      stunGather: function(host, port) { return C.psnStunGather(host, port || '3478'); },
      punch: function(candidatesCsv, portType, hashedLocalB64, hashedConsoleB64, sidLocal, sidConsole) {
        return C.psnPunch(candidatesCsv, portType | 0,
          hashedLocalB64 || '', hashedConsoleB64 || '', sidLocal | 0, sidConsole | 0);
      }
    };
    window.ChiakiTizen.sessionStartRemote = function(ps5, registKeyB64, rpKeyB64,
      res, fps, hdr, ctrlFd, dataFd, psIp, psCtrlPort) {
      return C.sessionStartRemote(ps5, registKeyB64, rpKeyB64, res, fps, hdr,
        ctrlFd, dataFd, psIp, psCtrlPort);
    };
    window.ChiakiTizen.loadStreamSettingsForRemote = function() {
      return loadStreamSettings();
    };
    // Deliberately NOT calling C.init() here: this fires from
    // Module.onRuntimeInitialized, which runs synchronously on the real
    // browser main thread, before PROXY_TO_PTHREAD has handed main() off to
    // a worker. Every ct_* entry point (see the threading-model comment atop
    // bridge.cc) is meant to run off the main thread; calling ct_init() from
    // here used to hang forever (likely inside mbedTLS's entropy/RNG
    // seeding in chiaki_lib_init(), which blocks in a way that's fine on a
    // real pthread worker but freezes Tizen's WebKit when run on the actual
    // main thread). wasm's own main() already calls ct_init() correctly on
    // the proxied worker and emits a "ready" event when it's done — see the
    // 'ready' case below.
  },

  _onEvent: function(jsonStr) {
    var ev;
    try { ev = JSON.parse(jsonStr); } catch (e) { return; }
    switch (ev.type) {
      case 'ready': onCoreReady(); break;
      case 'discoveryHosts': onDiscoveryHosts(ev.hosts || []); break;
      case 'registFinished': onRegistFinished(ev); break;
      case 'connected': onStreamConnected(); break;
      case 'loginPinRequest': onLoginPinRequest(ev); break;
      case 'nickname':
        if (state.streaming && state.currentHost)
          upsertConsole({ host: state.currentHost, nickname: ev.nickname });
        break;
      case 'quit': onStreamQuit(ev); break;
      case 'rumble': onRumble(ev.left, ev.right); break;
      case 'streamStats':
        state.lastStats = { kbps: ev.kbps, fps: ev.fps, width: ev.width,
                            height: ev.height, hdr: ev.hdr };
        if (state.hudOpen) populateHud();
        break;
      // PSN over-the-internet transport events -> the signaling module
      case 'psnWsOpen':
      case 'psnWsClosed':
      case 'psnNotification':
      case 'psnStun':
      case 'psnPunch':
        if (window.ChiakiPSN && window.ChiakiPSN.onTransportEvent)
          window.ChiakiPSN.onTransportEvent(ev);
        break;
    }
  },

  _onModuleLoadFailed: function() {
    setModuleStatus('Chiaki core script failed to load.', true);
    document.getElementById('console-list').innerHTML =
      '<p class="empty-hint">' + escapeHtml(state.moduleStatus) + '</p>';
    hideSplash();
  },

  _onModuleStatus: function(text) {
    setModuleStatus(text, /failed|abort|bad memory|exception|^\[error\]|^\[promise\]/i.test(String(text)));
  }
};

if (window.__ctModuleStatusQueue) {
  window.__ctModuleStatusQueue.forEach(function(t) { window.ChiakiTizen._onModuleStatus(t); });
  window.__ctModuleStatusQueue = [];
}

// Fired by the 'ready' event that wasm's main() emits once ct_init() has
// completed on the proxied pthread worker (see _onModuleReady above).
function onCoreReady() {
  if (state.moduleReady) return;
  state.moduleReady = true;
  state.moduleFailed = false;
  setModuleStatus('Chiaki core ready', false);
  // Discovery is a UDP broadcast; global 255.255.255.255 is often dropped on
  // TVs/APs, so prefer the TV's subnet-directed broadcast (x.y.z.255). Saved
  // consoles are also pinged directly (unicast) so their status works even if
  // broadcast is filtered.
  computeBroadcastAddr(function(bcast) {
    state.broadcastAddr = bcast;
    startDiscovery();
  });
  renderConsoleList();
  hideSplash();
}

function savedHostsCsv() {
  var seen = {}, ips = [];
  loadConsoles().forEach(function(c) {
    if (c.host && !seen[c.host]) { seen[c.host] = true; ips.push(c.host); }
  });
  return ips.join(',');
}
function startDiscovery() {
  if (!C) return;
  var bcast = state.broadcastAddr || '255.255.255.255';
  var hosts = savedHostsCsv();
  if (window.__ctReport)
    window.__ctReport('discovery', 'broadcast ' + bcast + ' + ' +
      (hosts ? hosts.split(',').length : 0) + ' unicast');
  C.discoveryStart(bcast, hosts);
}
// Re-run discovery after the saved list changes so new consoles get polled
// (and removed ones stop). Stop then start are queued in order on the worker.
function restartDiscovery() {
  if (!C || !state.moduleReady) return;
  C.discoveryStop();
  startDiscovery();
}

// Determine the subnet-directed broadcast from the TV's IPv4 (Wi-Fi or wired)
// via tizen.systeminfo; fall back to the global broadcast. Assumes a /24, which
// covers virtually all home networks. Async because systeminfo is callback-based.
function computeBroadcastAddr(cb) {
  var fallback = '255.255.255.255';
  if (!(window.tizen && tizen.systeminfo && tizen.systeminfo.getPropertyValue)) {
    cb(fallback); return;
  }
  var nets = ['WIFI_NETWORK', 'ETHERNET_NETWORK'];
  var i = 0;
  (function tryNext() {
    if (i >= nets.length) { cb(fallback); return; }
    var key = nets[i++];
    try {
      tizen.systeminfo.getPropertyValue(key, function(info) {
        var ip = info && info.ipAddress;
        if (ip && /^\d{1,3}(\.\d{1,3}){3}$/.test(ip) && ip !== '0.0.0.0') {
          var p = ip.split('.'); p[3] = '255';
          cb(p.join('.'));
        } else { tryNext(); }
      }, function() { tryNext(); });
    } catch (e) { tryNext(); }
  })();
}

// ---------------------------------------------------------------------------
// Screens & focus (D-pad navigation)
// ---------------------------------------------------------------------------
function show(screenId) {
  document.querySelectorAll('.screen').forEach(function(s) {
    s.classList.remove('active');
  });
  document.getElementById('screen-' + screenId).classList.add('active');
  state.screen = screenId;
  var first = document.querySelector('#screen-' + screenId + ' [data-nav], #screen-' + screenId + ' .console-card');
  if (first) first.focus();
}

function focusables() {
  return Array.prototype.slice.call(document.querySelectorAll(
    '#screen-' + state.screen + ' .active [data-nav], ' +
    '#screen-' + state.screen + ' [data-nav], ' +
    '#screen-' + state.screen + ' .console-card'
  )).filter(function(el) { return el.offsetParent !== null; });
}

function moveFocus(delta) {
  var els = focusables();
  if (!els.length) return;
  var i = els.indexOf(document.activeElement);
  var next = i < 0 ? 0 : Math.min(els.length - 1, Math.max(0, i + delta));
  els[next].focus();
}

var openTvSelectId = null;

function tvSelectButton(id) {
  return document.querySelector('[data-select-for="' + cssEscape(id) + '"]');
}

function tvSelectMenu(id) {
  return document.getElementById(id + '-menu');
}

function syncTvSelect(id) {
  var select = document.getElementById(id);
  var button = tvSelectButton(id);
  var menu = tvSelectMenu(id);
  if (!select || !button) return;
  var opt = select.options[select.selectedIndex];
  button.textContent = opt ? opt.text : '';
  if (menu) {
    Array.prototype.forEach.call(menu.querySelectorAll('.tv-select-option'), function(btn) {
      btn.classList.toggle('selected', btn.dataset.value === select.value);
    });
  }
}

function syncAllTvSelects() {
  Array.prototype.forEach.call(document.querySelectorAll('.native-select-source'), function(select) {
    syncTvSelect(select.id);
  });
}

function closeTvSelect(focusButton) {
  if (!openTvSelectId) return;
  var button = tvSelectButton(openTvSelectId);
  var menu = tvSelectMenu(openTvSelectId);
  if (button) button.classList.remove('open');
  if (menu) menu.classList.add('hidden');
  var id = openTvSelectId;
  openTvSelectId = null;
  if (focusButton && button)
    button.focus();
  syncTvSelect(id);
}

function openTvSelect(id) {
  closeTvSelect(false);
  var button = tvSelectButton(id);
  var menu = tvSelectMenu(id);
  if (!button || !menu) return;
  openTvSelectId = id;
  button.classList.add('open');
  menu.classList.remove('hidden');
  syncTvSelect(id);
  var selected = menu.querySelector('.tv-select-option.selected') || menu.querySelector('.tv-select-option');
  if (selected) selected.focus();
}

function toggleTvSelect(id) {
  if (openTvSelectId === id) closeTvSelect(true);
  else openTvSelect(id);
}

function initTvSelects() {
  Array.prototype.forEach.call(document.querySelectorAll('.native-select-source'), function(select) {
    var button = tvSelectButton(select.id);
    var menu = tvSelectMenu(select.id);
    if (!button || !menu) return;
    menu.innerHTML = '';
    Array.prototype.forEach.call(select.options, function(opt) {
      var item = document.createElement('button');
      item.type = 'button';
      item.className = 'tv-select-option';
      item.dataset.value = opt.value;
      item.setAttribute('data-nav', '');
      item.textContent = opt.text;
      item.addEventListener('click', function() {
        select.value = opt.value;
        syncTvSelect(select.id);
        closeTvSelect(true);
      });
      menu.appendChild(item);
    });
    button.addEventListener('click', function() {
      toggleTvSelect(select.id);
    });
    syncTvSelect(select.id);
  });
}

// The card currently in focus, whether the card itself or its delete button.
function focusedConsoleCard() {
  var el = document.activeElement;
  if (!el) return null;
  if (el.classList.contains('console-card')) return el;
  if (el.classList.contains('cc-del')) return el.closest('.console-card');
  return null;
}
// If a delete button is focused, move focus back to its card first (so
// up/down leaves the card row cleanly).
function focusCardIfOnDelete() {
  var el = document.activeElement;
  if (el && el.classList.contains('cc-del')) el.closest('.console-card').focus();
}

// ---------------------------------------------------------------------------
// Console list
// ---------------------------------------------------------------------------
function onDiscoveryHosts(hosts) {
  hosts.forEach(function(h) { state.discovered[h.hostAddr] = h; });
  reconcileSavedConsoleAddresses();
  scheduleConsoleListRender();
}

function scheduleConsoleListRender() {
  if (state.screen !== 'consoles') return;
  if (state.consoleListRenderTimer) return;
  state.consoleListRenderTimer = setTimeout(function() {
    state.consoleListRenderTimer = null;
    renderConsoleList();
  }, 150);
}

function discoveredConsoleType(d) {
  var text = ((d && d.hostType) || '') + ' ' + ((d && d.hostName) || '');
  if (/ps5/i.test(text)) return 'ps5';
  if (/ps4/i.test(text)) return 'ps4';
  return null;
}

function discoveredOnlineState(host) {
  var d = state.discovered[host];
  return d && (d.state === 'ready' || d.state === 'standby');
}

function updateSavedDiscoveryMetadata(list) {
  var changed = false;
  list.forEach(function(c) {
    var d = state.discovered[c.host];
    if (!d) return;
    if (d.hostId && c.hostId !== d.hostId) {
      c.hostId = d.hostId;
      changed = true;
    }
    if (d.hostName && !c.discoveredName) {
      c.discoveredName = d.hostName;
      changed = true;
    }
  });
  return changed;
}

function migrateSavedHost(list, oldHost, newHost, d) {
  var changed = false;
  list.forEach(function(c) {
    if (c.host !== oldHost) return;
    c.host = newHost;
    c.id = consoleId(newHost, c.accountB64);
    if (d && d.hostId) c.hostId = d.hostId;
    if (d && d.hostName && !c.discoveredName) c.discoveredName = d.hostName;
    changed = true;
  });
  return changed;
}

function reconcileSavedConsoleAddresses() {
  var saved = loadConsoles();
  if (!saved.length) return;

  var changed = updateSavedDiscoveryMetadata(saved);
  var savedHosts = {};
  saved.forEach(function(c) { if (c.host) savedHosts[c.host] = true; });

  // Strong match: once we have stored a discovery hostId, follow it across
  // DHCP changes. All profiles for the old address move together.
  Object.keys(state.discovered).forEach(function(host) {
    if (savedHosts[host]) return;
    var d = state.discovered[host];
    if (!d || !d.hostId || !discoveredOnlineState(host)) return;
    var oldHosts = {};
    saved.forEach(function(c) {
      if (c.hostId === d.hostId && c.host !== host)
        oldHosts[c.host] = true;
    });
    Object.keys(oldHosts).forEach(function(oldHost) {
      if (migrateSavedHost(saved, oldHost, host, d)) {
        changed = true;
        if (window.__ctReport)
          window.__ctReport('discovery', 'updated saved console address ' +
            oldHost + ' -> ' + host + ' by hostId');
      }
    });
  });

  savedHosts = {};
  saved.forEach(function(c) { if (c.host) savedHosts[c.host] = true; });

  // Fallback for existing installs that never stored hostId: if exactly one
  // unsaved online console of a type appears and exactly one offline paired
  // saved address of that same type exists, treat it as DHCP reassignment.
  ['ps5', 'ps4'].forEach(function(type) {
    var candidates = [];
    Object.keys(state.discovered).forEach(function(host) {
      if (savedHosts[host] || !discoveredOnlineState(host)) return;
      var d = state.discovered[host];
      if (discoveredConsoleType(d) === type)
        candidates.push({ host: host, discovery: d });
    });
    if (candidates.length !== 1) return;

    var oldHosts = {};
    saved.forEach(function(c) {
      if (!c.registKeyB64 || !c.rpKeyB64) return;
      if ((type === 'ps5') !== !!c.ps5) return;
      if (discoveredOnlineState(c.host)) return;
      oldHosts[c.host] = true;
    });
    var oldHostList = Object.keys(oldHosts);
    if (oldHostList.length !== 1) return;

    var candidate = candidates[0];
    if (migrateSavedHost(saved, oldHostList[0], candidate.host, candidate.discovery)) {
      changed = true;
      if (window.__ctReport)
        window.__ctReport('discovery', 'updated saved console address ' +
          oldHostList[0] + ' -> ' + candidate.host + ' by single-candidate match');
    }
  });

  if (changed)
    saveConsoles(dedupeConsoles(saved));
}

function renderConsoleList() {
  updatePsnStatusChip();
  var listEl = document.getElementById('console-list');
  var saved = loadConsoles();
  var savedHosts = {};
  var entries = [];

  saved.forEach(function(c) {
    var d = state.discovered[c.host];
    var psn = c.label || (c.accountB64 ? ('ID ' + accountIdDecimal(c.accountB64)) : '');
    entries.push({
      id: entryId(c), host: c.host, name: c.label || c.nickname || c.host,
      paired: !!c.registKeyB64, ps5: !!c.ps5,
      state: d ? d.state : 'unknown', app: d ? d.runningApp : '',
      psn: psn, saved: true
    });
    savedHosts[c.host] = true;
  });
  Object.keys(state.discovered).forEach(function(host) {
    if (savedHosts[host]) return; // already shown as a saved entry
    var d = state.discovered[host];
    entries.push({
      host: host, name: d.hostName || host, paired: false,
      ps5: /ps5/i.test(d.hostType || ''), state: d.state, app: d.runningApp,
      psn: '', saved: false
    });
  });

  var signature = JSON.stringify(entries.map(function(e) {
    return [
      e.id || '', e.host || '', e.name || '', e.paired ? 1 : 0,
      e.ps5 ? 1 : 0, e.state || '', e.app || '', e.psn || '', e.saved ? 1 : 0
    ];
  }));
  if (signature === state.consoleListSignature)
    return;
  state.consoleListSignature = signature;

  var focusedId = document.activeElement && document.activeElement.dataset
    ? document.activeElement.dataset.id : '';
  var focusedHost = document.activeElement && document.activeElement.dataset
    ? document.activeElement.dataset.host : '';
  var focusedDelete = document.activeElement &&
    document.activeElement.classList.contains('cc-del');

  listEl.innerHTML = '';

  if (!entries.length) {
    listEl.innerHTML = '<p class="empty-hint searching">Searching for consoles on your network&hellip; ' +
      'None found yet — you can add one by IP below.</p>';
    return;
  }

  var STATE_LABEL = { ready: 'Ready', standby: 'Standby', unknown: 'Offline' };
  entries.forEach(function(e) {
    var card = document.createElement('div');
    card.className = 'console-card';
    card.tabIndex = 0;
    card.dataset.host = e.host;
    card.dataset.ps5 = e.ps5 ? '1' : '0';
    card.dataset.paired = e.paired ? '1' : '0';
    if (e.id) card.dataset.id = e.id;
    var stateLabel = STATE_LABEL[e.state] || e.state;
    card.innerHTML =
      '<span class="cc-badge ' + (e.ps5 ? 'ps5' : 'ps4') + '">' +
        (e.ps5 ? 'PS5' : 'PS4') + '</span>' +
      '<span class="cc-main">' +
        '<span class="name">' + escapeHtml(e.name) + '</span>' +
        '<span class="addr">' + escapeHtml(e.host) +
          (e.app ? ' &middot; ' + escapeHtml(e.app) : '') + '</span>' +
        (e.psn ? '<span class="cc-psn">' + escapeHtml(e.psn) + '</span>' : '') +
      '</span>' +
      '<span class="cc-tags">' +
        (e.paired
          ? '<span class="cc-paired">Paired</span>'
          : '<span class="cc-pair-hint">Pair</span>') +
        '<span class="state ' + e.state + '">' + stateLabel + '</span>' +
        (e.saved
          ? '<button class="cc-del" tabindex="0" data-del="' + escapeHtml(e.id) +
            '" aria-label="Remove this console">&#10005;</button>'
          : '') +
      '</span>';
    listEl.appendChild(card);
  });

  if (focusedId || focusedHost) {
    var selector = focusedId
      ? '.console-card[data-id="' + cssEscape(focusedId) + '"]'
      : '.console-card[data-host="' + cssEscape(focusedHost) + '"]';
    var restored = listEl.querySelector(selector);
    if (restored) {
      if (focusedDelete) {
        var del = restored.querySelector('.cc-del');
        if (del) restored = del;
      }
      restored.focus();
    }
  }
}

function escapeHtml(s) {
  var d = document.createElement('div');
  d.textContent = s == null ? '' : String(s);
  return d.innerHTML;
}

function cssEscape(s) {
  if (window.CSS && CSS.escape) return CSS.escape(s);
  return String(s).replace(/["\\]/g, '\\$&');
}

function activateConsole(card) {
  var host = card.dataset.host;
  var ps5 = card.dataset.ps5 === '1';
  if (card.dataset.paired !== '1') {
    // Prefilled-but-unpaired: carry its account/label into the pairing form so
    // only the 8-digit code is left to enter.
    var pre = card.dataset.id ? findConsoleById(card.dataset.id) : null;
    openRegist(host, ps5, pre && pre.accountB64, pre && pre.label);
    return;
  }
  // Paired. Resolve the specific saved entry (host + account) so we use the
  // right registration when a console is listed under more than one account.
  var entry = card.dataset.id ? findConsoleById(card.dataset.id) : null;
  if (!entry || !entry.registKeyB64) { openRegist(host, ps5); return; }
  var onLan = !!state.discovered[entry.host];
  var token = REMOTE_PLAY_INTERNET_ENABLED
    && window.ChiakiPSN && window.ChiakiPSN.loadToken && window.ChiakiPSN.loadToken();
  if (!onLan && token) startRemoteStream(entry);
  else startStream(entry);
}

// Over-the-internet stream via the PSN signaling flow (psn-signaling.js).
function startRemoteStream(saved) {
  if (!saved || !saved.registKeyB64 || !saved.rpKeyB64) { openRegist(saved && saved.host, saved && saved.ps5); return; }
  var host = saved.host, ps5 = !!saved.ps5;
  show('stream');
  var video = document.getElementById('stream-video');
  video.muted = false; video.volume = 1.0;
  var overlay = document.getElementById('stream-overlay');
  overlay.classList.remove('hidden', 'error');
  document.getElementById('stream-status').textContent = 'Connecting over the internet…';
  state.currentHost = host;
  resetHud();
  state.streaming = true;
  startGamepadLoop();
  window.ChiakiPSN.connectRemote({
    host: host, ps5: ps5,
    registKeyB64: saved.registKeyB64, rpKeyB64: saved.rpKeyB64,
    id: saved.id || entryId(saved),
    accountB64: saved.accountB64 || '',
    duid: saved.duid || '',
    nickname: saved.nickname,
    label: saved.label,
    discoveredName: saved.discoveredName
  }).catch(function(e) {
    onStreamQuit({ reason: 'remote: ' + (e && e.message ? e.message : 'failed') });
  });
}

function wakeConsole(card) {
  var saved = card.dataset.id ? findConsoleById(card.dataset.id)
    : loadConsoles().find(function(c) { return c.host === card.dataset.host; });
  if (saved && saved.registKeyB64) {
    C.wakeup(saved.host, saved.registKeyB64, saved.ps5 ? 1 : 0);
  }
}

// ---------------------------------------------------------------------------
// Pairing
// ---------------------------------------------------------------------------
function openRegist(host, ps5, account, label) {
  state.registTargetHost = host || '';
  document.getElementById('regist-host').value = host || '';
  document.getElementById('regist-target').value = ps5 ? 'ps5' : 'ps4';
  syncTvSelect('regist-target');
  document.getElementById('regist-account').value =
    (account != null && account !== '') ? account : (localStorage.getItem(LOCAL_PAIRING_ACCOUNT_KEY) || '');
  document.getElementById('regist-label').value = label || '';
  document.getElementById('regist-status').textContent = '';
  updateAccountHint();
  show('regist');
}

// Live-decode the entered PSN Account ID base64 to its numeric account id.
function updateAccountHint() {
  var hintEl = document.getElementById('regist-account-hint');
  if (!hintEl) return;
  var b64 = document.getElementById('regist-account').value.trim();
  if (!b64) { hintEl.textContent = ''; return; }
  var dec = accountIdDecimal(b64);
  hintEl.textContent = dec ? ('Account ID: ' + dec) : 'Not a valid account ID';
}

function submitRegist() {
  if (!state.moduleReady) {
    var waitStatus = document.getElementById('regist-status');
    waitStatus.textContent = moduleWaitMessage();
    waitStatus.className = state.moduleFailed ? 'status err' : 'status';
    return;
  }

  var host = document.getElementById('regist-host').value.trim();
  var ps5 = document.getElementById('regist-target').value === 'ps5';
  var account = document.getElementById('regist-account').value.trim();
  var label = document.getElementById('regist-label').value.trim();
  var pin = parseInt(document.getElementById('regist-pin').value.trim(), 10);
  var status = document.getElementById('regist-status');

  if (!host || !account || isNaN(pin)) {
    status.textContent = 'Fill in the console IP, PSN account ID and the 8-digit code.';
    status.className = 'status err';
    return;
  }
  status.textContent = 'Pairing\u2026';
  status.className = 'status';
  localStorage.setItem(LOCAL_PAIRING_ACCOUNT_KEY, account);
  state.registTargetHost = host;
  state.registTargetPs5 = ps5;
  state.registTargetAccount = account;
  state.registTargetLabel = label;
  // TEMPORARY: ct_regist_start() now always dispatches asynchronously (it
  // has to run on the pthread worker main()/ct_init() use, not wherever this
  // click handler executes on \u2014 see CT_DISPATCH_TO_WORKER in bridge.cc), so
  // its return value here is just "queued", never a real pass/fail. Every
  // outcome, including immediate validation failures that used to show here
  // synchronously, now arrives via the "registFinished" event -> onRegistFinished().
  C.registStart(host, ps5 ? 1 : 0, account, pin >>> 0);
}

function onRegistFinished(ev) {
  var status = document.getElementById('regist-status');
  C.registStop();
  if (ev.success) {
    upsertConsole({
      host: state.registTargetHost,
      ps5: !!state.registTargetPs5,
      nickname: ev.serverNickname,
      registKeyB64: ev.registKeyB64,
      rpKeyB64: ev.rpKeyB64,
      target: ev.target,
      accountB64: state.registTargetAccount || '',
      label: state.registTargetLabel || ''
    });
    status.textContent = 'Paired with ' + (ev.serverNickname || 'console') + '.';
    status.className = 'status ok';
    restartDiscovery(); // start polling the newly-paired console's status
    setTimeout(function() { show('consoles'); renderConsoleList(); }, 1200);
  } else {
    status.textContent = ev.error ? ('Pairing failed: ' + ev.error)
      : 'Pairing failed. Check the code and that Remote Play is enabled.';
    status.className = 'status err';
  }
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
var resetDataArmedUntil = 0;
var settingsPage = 'video';

function hasPsnRemoteLogin() {
  return !!(localStorage.getItem('psnOAuthToken') || localStorage.getItem('psnRefreshToken'));
}

function updatePsnStatusChip() {
  var chip = document.getElementById('psn-status-chip');
  if (chip) chip.classList.toggle('hidden', !hasPsnRemoteLogin());
}

function showSettingsPage(page, focusPanel) {
  settingsPage = page === 'psn' ? 'psn' : 'video';
  var videoPage = document.getElementById('settings-page-video');
  var psnPage = document.getElementById('settings-page-psn');
  var videoTab = document.getElementById('btn-settings-video-tab');
  var psnTab = document.getElementById('btn-settings-psn-tab');
  var panel = document.querySelector('#screen-settings .settings-panel');
  if (videoPage) videoPage.classList.toggle('active', settingsPage === 'video');
  if (psnPage) psnPage.classList.toggle('active', settingsPage === 'psn');
  if (videoTab) videoTab.classList.toggle('active', settingsPage === 'video');
  if (psnTab) psnTab.classList.toggle('active', settingsPage === 'psn');
  if (panel) panel.classList.toggle('psn-page', settingsPage === 'psn');
  document.getElementById('debug-report').classList.add('hidden');
  document.getElementById('settings-status').textContent = '';
  if (focusPanel) {
    var first = document.querySelector('#settings-page-' + settingsPage + ' [data-nav]');
    if (first) first.focus();
  }
}

function openSettings() {
  resetDataArmedUntil = 0;
  var settings = loadStreamSettings();
  document.getElementById('setting-resolution').value = String(settings.resolution);
  document.getElementById('setting-fps').value = String(settings.fps);
  document.getElementById('setting-hdr').value = settings.hdr ? '1' : '0';
  syncAllTvSelects();
  var authRow = document.getElementById('setting-psn-auth-row');
  if (authRow) authRow.classList.toggle('hidden', !PSN_AUTH_ENABLED || !window.ChiakiPSN);
  var redirectEl = document.getElementById('setting-psn-redirect');
  if (redirectEl) redirectEl.value = '';
  var loginHelper = document.getElementById('psn-login-helper');
  if (loginHelper) loginHelper.classList.add('hidden');
  var tokenRow = document.getElementById('setting-psn-token-row');
  if (tokenRow) tokenRow.classList.toggle('hidden', !PSN_RAW_TOKEN_FIELD_ENABLED);
  var tokenEl = document.getElementById('setting-psn-token');
  if (PSN_RAW_TOKEN_FIELD_ENABLED && tokenEl && window.ChiakiPSN)
    tokenEl.value = window.ChiakiPSN.loadToken() || '';
  document.getElementById('debug-report').classList.add('hidden');
  document.getElementById('settings-status').textContent = '';
  showSettingsPage('video', false);
  show('settings');
}

function setSettingsStatus(message, cls) {
  var status = document.getElementById('settings-status');
  status.textContent = message || '';
  status.className = 'status' + (cls ? ' ' + cls : '');
}

function openPsnLogin() {
  if (!window.ChiakiPSN || !window.ChiakiPSN.loginUrl) {
    setSettingsStatus('PSN login is not available in this build.', 'err');
    return;
  }
  var url = window.ChiakiPSN.loginUrl();
  var qr = document.getElementById('psn-login-qr');
  var urlEl = document.getElementById('psn-login-url');
  var helper = document.getElementById('psn-login-helper');
  if (qr)
    qr.src = 'https://api.qrserver.com/v1/create-qr-code/?size=220x220&margin=12&data='
      + encodeURIComponent(url);
  if (urlEl)
    urlEl.value = url;
  if (helper)
    helper.classList.remove('hidden');
  setSettingsStatus('Scan the QR on another device, sign in with Sony, then paste the redirect URL or code above. SmartThings and Apps2Samsung keyboards can help paste it.', '');
}

function savePsnLogin() {
  var redirectEl = document.getElementById('setting-psn-redirect');
  var redirect = redirectEl ? redirectEl.value.trim() : '';
  if (!window.ChiakiPSN || !window.ChiakiPSN.exchangeRedirect) {
    setSettingsStatus('PSN login is not available in this build.', 'err');
    return;
  }
  setSettingsStatus('Saving PSN login...', '');
  window.ChiakiPSN.exchangeRedirect(redirect).then(function(result) {
    if (redirectEl) redirectEl.value = '';
    updatePsnStatusChip();
    setSettingsStatus('PSN login saved for internet Remote Play. Pairing Account ID unchanged.', 'ok');
  }).catch(function(err) {
    setSettingsStatus(err && err.message ? err.message : 'PSN login failed.', 'err');
  });
}

function clearPsnLogin() {
  if (window.ChiakiPSN && window.ChiakiPSN.clearTokens)
    window.ChiakiPSN.clearTokens();
  localStorage.removeItem('psnAccountId'); // legacy shared key from test builds
  localStorage.removeItem('psnAccountIdDecimal');
  var helper = document.getElementById('psn-login-helper');
  if (helper) helper.classList.add('hidden');
  var redirectEl = document.getElementById('setting-psn-redirect');
  if (redirectEl) redirectEl.value = '';
  var tokenEl = document.getElementById('setting-psn-token');
  if (tokenEl) tokenEl.value = '';
  updatePsnStatusChip();
  setSettingsStatus('PSN login cleared.', 'ok');
}

function saveSettings() {
  var settings = {
    resolution: parseInt(document.getElementById('setting-resolution').value, 10),
    fps: parseInt(document.getElementById('setting-fps').value, 10),
    hdr: document.getElementById('setting-hdr').value === '1'
  };
  saveStreamSettings(settings);
  var tokenEl = document.getElementById('setting-psn-token');
  if (PSN_RAW_TOKEN_FIELD_ENABLED && tokenEl && window.ChiakiPSN) {
    var tok = tokenEl.value.trim();
    if (tok) window.ChiakiPSN.saveToken(tok);
  }
  var status = document.getElementById('settings-status');
  status.textContent = 'Saved.';
  status.className = 'status ok';
  setTimeout(function() { show('consoles'); renderConsoleList(); }, 500);
}

function redactDebugText(text) {
  return String(text == null ? '' : text)
    .replace(/\b(\d{1,3}\.\d{1,3}\.\d{1,3})\.\d{1,3}\b/g, '$1.x')
    .replace(/\b[A-Za-z0-9+/]{28,}={0,2}\b/g, '<redacted-token>');
}

function simpleObjectSummary(obj) {
  if (!obj) return 'unavailable';
  var out = {};
  var keys = [];
  try { keys = Object.keys(obj); } catch (e) {}
  [
    'model', 'manufacturer', 'buildVersion', 'name', 'version',
    'resolutionWidth', 'resolutionHeight', 'dotsPerInchWidth', 'dotsPerInchHeight',
    'physicalWidth', 'physicalHeight', 'networkType', 'ipAddress', 'ipv6Address',
    'macAddress', 'ssid', 'status', 'cable'
  ].forEach(function(k) {
    if (keys.indexOf(k) < 0) keys.push(k);
  });
  keys.forEach(function(k) {
    var v;
    try { v = obj[k]; } catch (e) { return; }
    if (v == null || typeof v === 'string' || typeof v === 'number' || typeof v === 'boolean')
      out[k] = v;
  });
  try { return redactDebugText(JSON.stringify(out)); }
  catch (e) { return 'unavailable'; }
}

function storageSummary() {
  var consoles = loadConsoles();
  var paired = 0, ps5 = 0, ps4 = 0;
  consoles.forEach(function(c) {
    if (c.registKeyB64 && c.rpKeyB64) paired++;
    if (c.ps5) ps5++; else ps4++;
  });
  var keys = [];
  try {
    for (var i = 0; i < localStorage.length; i++) keys.push(localStorage.key(i));
  } catch (e) {}
  keys.sort();
  return [
    'savedConsoles=' + consoles.length,
    'paired=' + paired,
    'ps5=' + ps5,
    'ps4=' + ps4,
    'localStorageKeys=' + keys.join(',')
  ].join(' ');
}

function appendSystemInfo(reportLines, render, done) {
  function complete() {
    if (done && !complete.done) {
      complete.done = true;
      done();
    }
  }

  if (!(window.tizen && tizen.systeminfo)) {
    reportLines.push('tizen.systeminfo: unavailable');
    render();
    complete();
    return;
  }

  var caps = [
    'http://tizen.org/system/model_name',
    'http://tizen.org/system/manufacturer',
    'http://tizen.org/system/platform.name',
    'http://tizen.org/system/platform.version',
    'http://tizen.org/system/build.string',
    'http://tizen.org/feature/platform.version',
    'http://tizen.org/feature/network.wifi',
    'http://tizen.org/feature/network.ethernet'
  ];
  caps.forEach(function(key) {
    try {
      var val = tizen.systeminfo.getCapability(key);
      if (val != null) reportLines.push('capability ' + key + '=' + redactDebugText(val));
    } catch (e) {}
  });
  render();

  var pending = 0;
  ['BUILD', 'DISPLAY', 'NETWORK', 'WIFI_NETWORK', 'ETHERNET_NETWORK'].forEach(function(prop) {
    try {
      pending++;
      tizen.systeminfo.getPropertyValue(prop, function(info) {
        reportLines.push('systeminfo ' + prop + '=' + simpleObjectSummary(info));
        render();
        if (--pending === 0) complete();
      }, function(err) {
        reportLines.push('systeminfo ' + prop + '=unavailable' +
          (err && err.name ? ' (' + err.name + ')' : ''));
        render();
        if (--pending === 0) complete();
      });
    } catch (e) {
      reportLines.push('systeminfo ' + prop + '=unavailable');
      render();
      pending--;
    }
  });
  if (pending > 0)
    setTimeout(complete, 1200);
  else
    complete();
}

function sendDebugReportText(report, status) {
  if (navigator.share) {
    navigator.share({ title: 'Chiaki for Tizen debug report', text: report }).then(function() {
      status.textContent = 'Debug report shared.';
      status.className = 'status ok';
    }, function() {
      status.textContent = 'Debug report shown below.';
      status.className = 'status ok';
    });
    return;
  }

  var body = encodeURIComponent(report.slice(0, 6000));
  var url = 'https://github.com/trent407/chiaki-tizen/issues/new?title=' +
    encodeURIComponent('TV debug report') + '&body=' + body;
  try {
    status.textContent = 'Opening issue page; report also shown below.';
    status.className = 'status ok';
    var w = window.open(url, '_blank');
    if (!w) location.href = url;
  } catch (e) {
    status.textContent = 'Debug report shown below.';
    status.className = 'status ok';
  }
}

function showDebugReport() {
  var reportEl = document.getElementById('debug-report');
  var status = document.getElementById('settings-status');
  var settings = loadStreamSettings();
  var lines = [
    'Chiaki for Tizen debug report',
    'generated=' + new Date().toISOString(),
    'location=' + redactDebugText(location.href),
    'userAgent=' + redactDebugText(navigator.userAgent || ''),
    'screen=' + screen.width + 'x' + screen.height +
      ' devicePixelRatio=' + (window.devicePixelRatio || 1),
    'moduleReady=' + state.moduleReady + ' moduleFailed=' + state.moduleFailed +
      ' screen=' + state.screen + ' streaming=' + state.streaming,
    'ui performanceMode=' + document.body.classList.contains('perf-ui') +
      ' tvModelYear=' + (state.tvModelYear || 'unknown'),
    'settings resolutionPreset=' + settings.resolution + ' fps=' + settings.fps +
      ' hdr=' + settings.hdr,
    'storage ' + storageSummary()
  ];

  try {
    if (window.tizen && tizen.application) {
      var app = tizen.application.getCurrentApplication();
      if (app && app.appInfo)
        lines.push('appInfo=' + simpleObjectSummary(app.appInfo));
    }
  } catch (e) {}

  lines.push('diagnosticsTail:');
  if (window.__ctDiagnostics && window.__ctDiagnostics.length) {
    window.__ctDiagnostics.slice(-80).forEach(function(line) {
      lines.push(redactDebugText(line));
    });
  } else {
    lines.push('(none)');
  }

  function render() {
    reportEl.value = lines.join('\n');
  }

  reportEl.classList.remove('hidden');
  status.textContent = 'Collecting debug report...';
  status.className = 'status';
  render();
  appendSystemInfo(lines, render, function() {
    render();
    sendDebugReportText(reportEl.value, status);
  });
  reportEl.focus();
}

function resetSavedData() {
  var status = document.getElementById('settings-status');
  var now = Date.now();
  if (now > resetDataArmedUntil) {
    resetDataArmedUntil = now + 6000;
    status.textContent = 'Press Reset saved data again to clear consoles, pairing keys, token and settings.';
    status.className = 'status err';
    return;
  }

  try {
    localStorage.clear();
    // Prevent a bundled personal prefill.json from immediately rehydrating
    // data after an intentional privacy reset.
    localStorage.setItem('prefillApplied', '1');
  } catch (e) {}
  resetDataArmedUntil = 0;
  state.discovered = {};
  state.currentHost = null;
  state.lastStats = null;
  state.consoleListSignature = '';
  document.getElementById('setting-resolution').value = '3';
  document.getElementById('setting-fps').value = '30';
  document.getElementById('setting-hdr').value = '0';
  syncAllTvSelects();
  document.getElementById('setting-psn-token').value = '';
  document.getElementById('debug-report').classList.add('hidden');
  if (window.ChiakiPSN && window.ChiakiPSN.loadToken)
    window.ChiakiPSN.loadToken();
  renderConsoleList();
  restartDiscovery();
  status.textContent = 'Saved data cleared.';
  status.className = 'status ok';
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------
function startStream(saved) {
  if (!state.moduleReady) return;

  if (!saved || !saved.registKeyB64 || !saved.rpKeyB64) {
    openRegist(saved && saved.host, saved && saved.ps5);
    return;
  }
  var host = saved.host, ps5 = !!saved.ps5;
  show('stream');
  var video = document.getElementById('stream-video');
  video.muted = false;
  video.volume = 1.0;
  var overlay = document.getElementById('stream-overlay');
  overlay.classList.remove('hidden', 'error');
  document.getElementById('stream-status').textContent = 'Connecting\u2026';

  state.currentHost = host;
  resetHud();
  var streamSettings = loadStreamSettings();
  // sessionStart dispatches asynchronously to the wasm worker (sockets can't
  // run on this thread — see CT_DISPATCH_TO_WORKER in bridge.cc), so its
  // return value is just "queued". Start failures arrive as a "quit" event
  // -> onStreamQuit, same path as any runtime disconnect.
  // HDR is PS5-only HEVC Main10; the bridge already ignores it for PS4, but
  // gate here too so a PS4 stream never even requests it.
  var hdr = (streamSettings.hdr && ps5) ? 1 : 0;
  try {
    C.sessionStart(host, ps5 ? 1 : 0, saved.registKeyB64, saved.rpKeyB64,
      streamSettings.resolution, streamSettings.fps, hdr);
  } catch (e) {
    // Older wasm without the hdr parameter: retry the 6-arg signature. HDR is
    // simply unavailable until the module is rebuilt from the current bridge.cc.
    C.sessionStartNoHdr(host, ps5 ? 1 : 0, saved.registKeyB64, saved.rpKeyB64,
      streamSettings.resolution, streamSettings.fps);
  }
  state.streaming = true;
  startGamepadLoop();
}

function stopStream() {
  if (!state.streaming) return;
  state.streaming = false;
  stopGamepadLoop();
  closeHud();
  C.sessionStop();
  show('consoles');
  renderConsoleList();
}

function onStreamConnected() {
  document.getElementById('stream-overlay').classList.add('hidden');
}

function onStreamQuit(ev) {
  state.streaming = false;
  stopGamepadLoop();
  closeHud();
  var overlay = document.getElementById('stream-overlay');
  overlay.classList.remove('hidden');
  overlay.classList.add('error');
  document.getElementById('stream-status').textContent =
    'Disconnected: ' + (ev.reason || 'unknown');
  setTimeout(function() { show('consoles'); renderConsoleList(); }, 2000);
}

function onLoginPinRequest(ev) {
  var dlg = document.getElementById('login-pin-dialog');
  dlg.classList.remove('hidden');
  dlg.querySelector('h3').textContent = 'Console login PIN';
  var input = document.getElementById('login-pin');
  input.value = '';
  input.focus();
  if (ev.incorrect)
    dlg.querySelector('h3').textContent = 'PIN incorrect — try again';
}

// ---------------------------------------------------------------------------
// Stream HUD (yellow / "C" remote key): live stats + on-screen PS button
// ---------------------------------------------------------------------------
var RES_LABEL = { 3: '720p', 4: '1080p' };

function openHud() {
  state.hudOpen = true;
  document.getElementById('stream-hud').classList.remove('hidden');
  state.hudFocusIndex = 0;
  populateHud();
  hudApplyFocus();
}
function closeHud() {
  state.hudOpen = false;
  document.getElementById('stream-hud').classList.add('hidden');
}
function resetHud() {
  state.lastStats = null;
  closeHud();
}
function hudNavEls() {
  return Array.prototype.slice.call(
    document.querySelectorAll('#stream-hud [data-hud-nav]'));
}
function hudApplyFocus() {
  var els = hudNavEls();
  els.forEach(function(el, i) {
    el.classList.toggle('hud-nav-focus', i === (state.hudFocusIndex || 0));
  });
}
function hudMoveFocus(delta) {
  var els = hudNavEls();
  if (!els.length) return;
  state.hudFocusIndex = Math.min(els.length - 1,
    Math.max(0, (state.hudFocusIndex || 0) + delta));
  hudApplyFocus();
}
function hudActivate() {
  var el = hudNavEls()[state.hudFocusIndex || 0];
  if (!el) return;
  if (el.id === 'hud-home') pulsePs();
  else if (el.id === 'hud-close') closeHud();
}
// Send a brief PS-button press to the console (works around the physical
// PS/Guide button being swallowed by the TV when the pad is paired to the TV).
function pulsePs() {
  state.remoteButtons |= BTN.PS;
  setTimeout(function() { state.remoteButtons &= ~BTN.PS; }, 200);
}
function populateHud() {
  var s = loadStreamSettings();
  var st = state.lastStats;
  document.getElementById('hud-res').textContent =
    (st && st.width) ? (st.width + '×' + st.height)
                     : (RES_LABEL[s.resolution] || ('preset ' + s.resolution));
  document.getElementById('hud-fps').textContent =
    ((st && st.fps) ? st.fps : s.fps) + ' fps';
  var brEl = document.getElementById('hud-bitrate');
  if (st && typeof st.kbps === 'number')
    brEl.textContent = st.kbps >= 1000 ? (st.kbps / 1000).toFixed(1) + ' Mbps'
                                       : st.kbps + ' kbps';
  else
    brEl.textContent = '—';
  var hdrEl = document.getElementById('hud-hdr');
  if (st) {
    hdrEl.textContent = st.hdr ? 'Active (HEVC HDR10)' : 'Off';
    hdrEl.className = st.hdr ? 'ok' : 'off';
  } else {
    hdrEl.textContent = s.hdr ? 'Requested' : 'Off';
    hdrEl.className = s.hdr ? '' : 'off';
  }
  document.getElementById('hud-ip').textContent = state.currentHost || '—';
}

// ---------------------------------------------------------------------------
// Controller input: HTML5 Gamepad API (DualShock/DualSense over Bluetooth)
// with the TV remote as a minimal fallback.
// ---------------------------------------------------------------------------
function startGamepadLoop() {
  var AXIS = 32767;
  function poll() {
    if (!state.streaming) return;
    var pads = navigator.getGamepads ? navigator.getGamepads() : [];
    var gp = null;
    for (var i = 0; i < pads.length; i++) if (pads[i]) { gp = pads[i]; break; }

    var buttons = state.remoteButtons;
    var l2 = 0, r2 = 0, lx = 0, ly = 0, rx = 0, ry = 0;

    if (gp) {
      var b = gp.buttons;
      var pressed = function(i) { return b[i] && b[i].pressed; };
      // Standard mapping: 0=A(cross) 1=B(circle) 2=X(square) 3=Y(triangle)
      if (pressed(0)) buttons |= BTN.CROSS;
      if (pressed(1)) buttons |= BTN.MOON;
      if (pressed(2)) buttons |= BTN.BOX;
      if (pressed(3)) buttons |= BTN.PYRAMID;
      if (pressed(4)) buttons |= BTN.L1;
      if (pressed(5)) buttons |= BTN.R1;
      if (pressed(8)) buttons |= BTN.SHARE;
      if (pressed(9)) buttons |= BTN.OPTIONS;
      if (pressed(10)) buttons |= BTN.L3;
      if (pressed(11)) buttons |= BTN.R3;
      if (pressed(12)) buttons |= BTN.DPAD_UP;
      if (pressed(13)) buttons |= BTN.DPAD_DOWN;
      if (pressed(14)) buttons |= BTN.DPAD_LEFT;
      if (pressed(15)) buttons |= BTN.DPAD_RIGHT;
      if (pressed(16)) buttons |= BTN.PS;
      if (pressed(17)) buttons |= BTN.TOUCHPAD;
      l2 = b[6] ? Math.round(b[6].value * 255) : 0;
      r2 = b[7] ? Math.round(b[7].value * 255) : 0;
      lx = Math.round((gp.axes[0] || 0) * AXIS);
      ly = Math.round((gp.axes[1] || 0) * AXIS);
      rx = Math.round((gp.axes[2] || 0) * AXIS);
      ry = Math.round((gp.axes[3] || 0) * AXIS);
    }

    C.controllerState(buttons >>> 0, l2, r2, lx, ly, rx, ry);
    state.gamepadTimer = requestAnimationFrame(poll);
  }
  state.gamepadTimer = requestAnimationFrame(poll);
}

function stopGamepadLoop() {
  if (state.gamepadTimer) cancelAnimationFrame(state.gamepadTimer);
  state.gamepadTimer = null;
  state.remoteButtons = 0;
  stopRumble();
}

// ---------------------------------------------------------------------------
// Rumble: drive the Gamepad API vibration actuator from chiaki rumble events.
// This is the one DualSense output reachable on a TV-paired pad (haptics/
// adaptive triggers need raw HID, which Tizen's web sandbox doesn't expose).
// chiaki emits { left, right } 0-255: left = low-frequency (strong) motor,
// right = high-frequency (weak). The Web effect has a finite duration, so a
// held rumble is kept alive by re-issuing it until a zero update arrives.
// ---------------------------------------------------------------------------
var rumbleState = { strong: 0, weak: 0, timer: null };

function applyRumble() {
  var pads = navigator.getGamepads ? navigator.getGamepads() : [];
  for (var i = 0; i < pads.length; i++) {
    var gp = pads[i];
    if (!gp || !gp.vibrationActuator || !gp.vibrationActuator.playEffect) continue;
    try {
      gp.vibrationActuator.playEffect('dual-rumble', {
        duration: 1000, startDelay: 0,
        strongMagnitude: rumbleState.strong, weakMagnitude: rumbleState.weak
      });
    } catch (e) { /* actuator not available on this pad/platform */ }
  }
}

function onRumble(left, right) {
  rumbleState.strong = Math.max(0, Math.min(1, (left || 0) / 255));
  rumbleState.weak = Math.max(0, Math.min(1, (right || 0) / 255));
  if (rumbleState.timer) { clearInterval(rumbleState.timer); rumbleState.timer = null; }
  applyRumble();
  if (rumbleState.strong > 0 || rumbleState.weak > 0)
    rumbleState.timer = setInterval(applyRumble, 900); // outlast the 1s effect
}

function stopRumble() {
  rumbleState.strong = 0; rumbleState.weak = 0;
  if (rumbleState.timer) { clearInterval(rumbleState.timer); rumbleState.timer = null; }
  var pads = navigator.getGamepads ? navigator.getGamepads() : [];
  for (var i = 0; i < pads.length; i++) {
    var gp = pads[i];
    if (!gp || !gp.vibrationActuator) continue;
    try {
      if (gp.vibrationActuator.reset) gp.vibrationActuator.reset();
      else if (gp.vibrationActuator.playEffect)
        gp.vibrationActuator.playEffect('dual-rumble',
          { duration: 0, strongMagnitude: 0, weakMagnitude: 0 });
    } catch (e) {}
  }
}

// TV remote → controller while streaming
var REMOTE_MAP = {};
REMOTE_MAP[KEY.UP] = BTN.DPAD_UP;
REMOTE_MAP[KEY.DOWN] = BTN.DPAD_DOWN;
REMOTE_MAP[KEY.LEFT] = BTN.DPAD_LEFT;
REMOTE_MAP[KEY.RIGHT] = BTN.DPAD_RIGHT;
REMOTE_MAP[KEY.ENTER] = BTN.CROSS;
REMOTE_MAP[KEY.RED] = BTN.MOON;
REMOTE_MAP[KEY.GREEN] = BTN.PYRAMID;
// YELLOW (C) is the HUD toggle during streaming — not a controller button.
REMOTE_MAP[KEY.BLUE] = BTN.TOUCHPAD;
REMOTE_MAP[KEY.PLAYPAUSE] = BTN.OPTIONS;
REMOTE_MAP[KEY.PLAY] = BTN.OPTIONS;

// ---------------------------------------------------------------------------
// Global key handling
// ---------------------------------------------------------------------------
document.addEventListener('keydown', function(e) {
  var code = e.keyCode;

  if (state.screen === 'stream') {
    var pinDlg = document.getElementById('login-pin-dialog');
    if (!pinDlg.classList.contains('hidden')) return; // let the input take it

    // HUD open: the remote drives the overlay, not the game.
    if (state.hudOpen) {
      e.preventDefault();
      switch (code) {
        case KEY.LEFT: case KEY.UP: hudMoveFocus(-1); break;
        case KEY.RIGHT: case KEY.DOWN: hudMoveFocus(1); break;
        case KEY.ENTER: hudActivate(); break;
        case KEY.YELLOW: case KEY.BACK: case KEY.EXIT: closeHud(); break;
      }
      return;
    }

    if (code === KEY.YELLOW) { e.preventDefault(); openHud(); return; }
    if (code === KEY.BACK || code === KEY.EXIT || code === KEY.STOP) {
      e.preventDefault();
      stopStream();
      return;
    }
    if (REMOTE_MAP[code] !== undefined) {
      e.preventDefault();
      state.remoteButtons |= REMOTE_MAP[code];
      return;
    }
    return;
  }

  if (openTvSelectId && (code === KEY.BACK || code === KEY.EXIT)) {
    e.preventDefault();
    closeTvSelect(true);
    return;
  }

  switch (code) {
    case KEY.UP: e.preventDefault(); focusCardIfOnDelete(); moveFocus(-1); break;
    case KEY.DOWN: e.preventDefault(); focusCardIfOnDelete(); moveFocus(1); break;
    case KEY.LEFT: {
      var dl = document.activeElement;
      if (dl && dl.classList.contains('cc-del')) { e.preventDefault(); dl.closest('.console-card').focus(); }
      else moveFocus(-1);
      break;
    }
    case KEY.RIGHT: {
      var dr = document.activeElement;
      var del = dr && dr.classList.contains('console-card') && dr.querySelector('.cc-del');
      if (del) { e.preventDefault(); del.focus(); }
      else moveFocus(1);
      break;
    }
    case KEY.ENTER: {
      var el = document.activeElement;
      if (el && el.classList.contains('console-card')) { e.preventDefault(); activateConsole(el); }
      else if (el && el.classList.contains('cc-del')) { e.preventDefault(); removeConsoleById(el.dataset.del); }
      break;
    }
    case KEY.GREEN: { // Triangle hint = wake
      var el2 = focusedConsoleCard();
      if (el2) wakeConsole(el2);
      break;
    }
    case KEY.YELLOW: { // Square hint = pair
      var el3 = focusedConsoleCard();
      if (el3) openRegist(el3.dataset.host, el3.dataset.ps5 === '1');
      break;
    }
    case KEY.BLUE: // TEMPORARY: toggle the debug log panel (see index.html)
      window.__ctToggleDebugLog && window.__ctToggleDebugLog();
      break;
    case KEY.CHANNEL_UP: // TEMPORARY: scroll #ct-debug-log
    case KEY.CHANNEL_DOWN: {
      var logEl = document.getElementById('ct-debug-log');
      if (logEl) {
        e.preventDefault();
        logEl.scrollTop += (code === KEY.CHANNEL_UP ? -1 : 1) * (logEl.clientHeight * 0.8);
      }
      break;
    }
    case KEY.BACK:
      e.preventDefault();
      if (state.screen === 'regist' || state.screen === 'settings') show('consoles');
      else if (window.tizen && tizen.application)
        tizen.application.getCurrentApplication().exit();
      break;
  }
});

document.addEventListener('keyup', function(e) {
  if (state.screen === 'stream' && REMOTE_MAP[e.keyCode] !== undefined)
    state.remoteButtons &= ~REMOTE_MAP[e.keyCode];
});

// ---------------------------------------------------------------------------
// Wire up buttons + register TV keys
// ---------------------------------------------------------------------------
document.getElementById('btn-add-manual').addEventListener('click', function() {
  openRegist('', true); // blank IP; default to PS5
});
document.getElementById('btn-settings').addEventListener('click', function() {
  openSettings();
});
document.getElementById('btn-settings-video-tab').addEventListener('click', function() {
  showSettingsPage('video', true);
});
document.getElementById('btn-settings-psn-tab').addEventListener('click', function() {
  showSettingsPage('psn', true);
});
document.getElementById('btn-regist-go').addEventListener('click', submitRegist);
document.getElementById('btn-regist-cancel').addEventListener('click', function() {
  C && C.registStop();
  show('consoles');
});
document.getElementById('btn-settings-save').addEventListener('click', saveSettings);
document.getElementById('btn-debug-report').addEventListener('click', showDebugReport);
document.getElementById('btn-reset-data').addEventListener('click', resetSavedData);
document.getElementById('btn-settings-cancel').addEventListener('click', function() {
  show('consoles');
});
document.getElementById('btn-psn-login').addEventListener('click', openPsnLogin);
document.getElementById('btn-psn-save').addEventListener('click', savePsnLogin);
document.getElementById('btn-psn-clear').addEventListener('click', clearPsnLogin);
document.getElementById('btn-login-pin').addEventListener('click', function() {
  var pin = document.getElementById('login-pin').value.trim();
  if (pin) {
    C.setLoginPin(pin);
    document.getElementById('login-pin-dialog').classList.add('hidden');
  }
});
document.getElementById('console-list').addEventListener('click', function(e) {
  var del = e.target.closest('.cc-del');
  if (del) { e.stopPropagation(); removeConsoleById(del.dataset.del); return; }
  var card = e.target.closest('.console-card');
  if (card) activateConsole(card);
});
document.getElementById('regist-account').addEventListener('input', updateAccountHint);
document.getElementById('hud-home').addEventListener('click', pulsePs);
document.getElementById('hud-close').addEventListener('click', closeHud);

// Register the non-default remote keys we rely on
if (window.tizen && tizen.tvinputdevice) {
  ['ColorF0Red', 'ColorF1Green', 'ColorF2Yellow', 'ColorF3Blue',
   'MediaPlayPause', 'MediaPlay', 'MediaPause', 'MediaStop', 'Exit',
   'ChannelUp', 'ChannelDown' // TEMPORARY: scroll #ct-debug-log
  ].forEach(function(k) {
    try { tizen.tvinputdevice.registerKey(k); } catch (err) { /* optional */ }
  });
}

initTvSelects();
updatePsnStatusChip();

// Restore remembered local-pairing Account ID. PSN Remote login uses separate
// storage so it cannot overwrite this field.
var savedAccount = localStorage.getItem(LOCAL_PAIRING_ACCOUNT_KEY) || '';
document.getElementById('regist-account').value = savedAccount;

setModuleStatus(state.moduleStatus, false);
setTimeout(function() {
  if (!state.moduleReady) {
    // TEMPORARY: used to dump the full pipe-joined diagnosticTrail() here,
    // which duplicated (and visually fought with) the persistent
    // #ct-debug-log panel in index.html now that it shows the same history
    // continuously. Keep this short — the full trail is already on screen.
    // Was gated at 12s, which turned out to be a false-alarm deadline: spawning
    // PTHREAD_POOL_SIZE=16 workers (each importing a ~740KB script) plus real
    // init can legitimately take longer than that on TV-class hardware, so this
    // fired and left stale "still loading" text sitting around even on runs
    // that went on to boot successfully a few seconds later. Bumped to 30s.
    var message = state.moduleFailed ? state.moduleStatus :
      'Still loading Chiaki core after 30s — see debug log below.';
    setModuleStatus(message, false);
    document.getElementById('console-list').innerHTML =
      '<p class="empty-hint">' + escapeHtml(message) + '</p>';
    hideSplash();
  }
}, 30000);

applyPrefill();
show('consoles');
renderConsoleList();

if (window.Module && Module.runtimeReady)
  window.ChiakiTizen._onModuleReady();

})();
