/* Tizen Sockets host-binding shim.
 *
 * How Samsung's glue actually wires sockets (from createWasm() in the
 * generated chiaki-tizen.js): the wasm import table references bare globals
 * (__wasm_accept ... __wasm_socket), and createWasm() post-processes every
 * STRING-valued import entry by treating it as a dotted path (e.g.
 * "tizentvwasm.SocketsHostBindings.create"), resolving it against the
 * current global scope, and installing the resolved NATIVE function object
 * directly as the wasm import. Socket calls then run wasm -> native with no
 * JS frame — which matters because invoking a host-binding function from a
 * JS frame throws "Cannot call host binding function from JS" on every
 * thread. If any path fails to resolve, the glue stubs that import and
 * disables host bindings (USE_HOST_BINDINGS=false), falling back to
 * emscripten's in-wasm socket emulation.
 *
 * So these globals must be defined as STRING PATHS before the glue
 * evaluates. Two previous versions of this shim defined them as plain JS
 * wrapper functions — createWasm() skips non-string entries, so the
 * wrappers themselves became the wasm imports and every socket call died
 * at Samsung's from-JS restriction. That was this app's entire pairing
 * failure.
 *
 * Defined guarded (only if not already present) so a second copy of this
 * shim in the same scope (build.sh prepends it to both the worker script
 * and the main glue, and index.html loads it standalone too) is a no-op.
 */
(function(global) {
  'use strict';

  function report(detail) {
    if (typeof __ctReport === 'function') {
      __ctReport('socket', detail);
      return;
    }
    try { // pthread worker: route through the printErr message channel
      global.postMessage({ cmd: 'printErr', text: '[socket] ' + detail });
      return;
    } catch (e) { /* main page without __ctReport, or clone failure */ }
    if (global.console && console.error)
      console.error('[socket] ' + detail);
  }

  var BINDINGS = {
    __wasm_accept: 'tizentvwasm.SocketsHostBindings.accept',
    __wasm_bind: 'tizentvwasm.SocketsHostBindings.bind',
    __wasm_close: 'tizentvwasm.SocketsHostBindings.close',
    __wasm_connect: 'tizentvwasm.SocketsHostBindings.connect',
    __wasm_getpeername: 'tizentvwasm.SocketsHostBindings.getPeerName',
    __wasm_getsockname: 'tizentvwasm.SocketsHostBindings.getSockName',
    __wasm_getsockopt: 'tizentvwasm.SocketsHostBindings.getSockOpt',
    __wasm_listen: 'tizentvwasm.SocketsHostBindings.listen',
    __wasm_poll: 'tizentvwasm.SocketsHostBindings.poll',
    __wasm_recv: 'tizentvwasm.SocketsHostBindings.recv',
    __wasm_recvfrom: 'tizentvwasm.SocketsHostBindings.recvFrom',
    __wasm_recvmsg: 'tizentvwasm.SocketsHostBindings.recvMsg',
    __wasm_select: 'tizentvwasm.SocketsHostBindings.select',
    __wasm_send: 'tizentvwasm.SocketsHostBindings.send',
    __wasm_sendmsg: 'tizentvwasm.SocketsHostBindings.sendMsg',
    __wasm_sendto: 'tizentvwasm.SocketsHostBindings.sendTo',
    __wasm_setsockopt: 'tizentvwasm.SocketsHostBindings.setSockOpt',
    __wasm_shutdown: 'tizentvwasm.SocketsHostBindings.shutdown',
    __wasm_socket: 'tizentvwasm.SocketsHostBindings.create'
  };

  // Pre-check which paths resolve in THIS scope, purely for diagnostics —
  // the glue does the real resolution itself. If any are listed missing in
  // the debug log, the glue will run with USE_HOST_BINDINGS=false in that
  // scope (emulated sockets), and that's the next thing to chase.
  function resolves(path) {
    var component = global;
    var parts = path.split('.');
    for (var i = 0; i < parts.length; i++) {
      if (typeof component[parts[i]] === 'undefined') return false;
      component = component[parts[i]];
    }
    return true;
  }

  var defined = 0, kept = 0, missing = [];
  Object.keys(BINDINGS).forEach(function(name) {
    var path = BINDINGS[name];
    if (!resolves(path)) missing.push(path.split('.').pop());
    if (typeof global[name] === 'undefined') {
      global[name] = path; // string path — createWasm() resolves it natively
      defined++;
    } else {
      kept++;
    }
  });

  report('shim: ' + defined + ' path(s) defined, ' + kept + ' pre-existing' +
    (missing.length ? '; UNRESOLVABLE here: ' + missing.join(',') : '; all resolvable'));
})(typeof self !== 'undefined' ? self : this);
