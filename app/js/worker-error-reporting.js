(function(global) {
  'use strict';

  function send(text) {
    try {
      global.postMessage({ cmd: 'printErr', text: text });
    } catch (e) {
      if (global.console && console.error)
        console.error(text);
    }
  }

  send('worker bootstrap started');

  global.onerror = function(message, source, lineno, colno, error) {
    var file = source ? String(source).split('/').pop() : 'worker';
    var text = 'worker global error ' + file + ':' + (lineno || 0) + ':' +
      (colno || 0) + ' ' + message;
    if (error && error.stack)
      text += ' | ' + String(error.stack).split('\n')[0];
    send(text);
    return false;
  };

  global.onunhandledrejection = function(ev) {
    var reason = ev && ev.reason;
    send('worker promise error ' + (reason && reason.message ? reason.message : String(reason)));
  };

  if (typeof global.importScripts === 'function') {
    var originalImportScripts = global.importScripts;
    global.importScripts = function() {
      send('worker importScripts begin ' +
        Array.prototype.slice.call(arguments).map(function(arg) {
          return String(arg).split('/').pop();
        }).join(','));
      try {
        var result = originalImportScripts.apply(global, arguments);
        send('worker importScripts complete');
        return result;
      } catch (e) {
        send('worker importScripts threw ' + (e && e.stack ? String(e.stack).split('\n')[0] : String(e)));
        throw e;
      }
    };
  } else {
    send('worker importScripts missing');
  }
})(typeof self !== 'undefined' ? self : this);
