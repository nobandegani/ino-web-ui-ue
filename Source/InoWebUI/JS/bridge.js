// InoWebUI bridge — single source of truth for window.InoWebUI.
//
// Edit this file then run Plugins/InoWebUI/Scripts/GenerateJSConstants.ps1
// to regenerate the C++/Java constants.
//
// Detects transport at runtime:
//   • window.chrome.webview        → Windows (WebView2)
//   • window._InoWebUIHost         → Android (addJavascriptInterface)
//
// Public API exposed to page scripts (identical on every platform):
//   window.InoWebUI.send(channel, payload)        — fire-and-forget UE→JS post
//   window.InoWebUI.on(channel, handler)          — subscribe
//   window.InoWebUI.off(channel, handler)         — unsubscribe
//   window.InoWebUI.once(channel, handler)        — auto-removes after first fire
//   window.InoWebUI.version                       — bridge version string
//
// Wire format both directions: { channel: string, payload: any } as JSON.
// ES5-only (no const/let/arrow/Map) so it runs on any page regardless of
// the page's transpile target.
(function() {
  if (typeof window === 'undefined' || window.InoWebUI) return;

  // Object.create(null) — listeners is a plain dictionary with NO prototype,
  // so a channel literally named 'toString' or 'hasOwnProperty' behaves like
  // any other channel instead of colliding with Object.prototype.
  var listeners = Object.create(null);
  var transport = null;

  if (window.chrome && window.chrome.webview) {
    transport = {
      send: function(json) { window.chrome.webview.postMessage(json); },
      install: function(dispatch) {
        window.chrome.webview.addEventListener('message', function(evt) {
          dispatch(evt.data);
        });
      }
    };
  } else if (window._InoWebUIHost) {
    transport = {
      send: function(json) { window._InoWebUIHost.receive(json); },
      install: function(dispatch) {
        // Android host calls this global after evaluateJavascript("...").
        window._InoWebUIDispatch = function(env) { dispatch(env); };
      }
    };
  } else {
    // No host bridge — running in a plain browser. Log once so dev tooling
    // makes the situation obvious instead of silently dropping all sends.
    if (typeof console !== 'undefined' && console.warn) {
      console.warn('InoWebUI: no host bridge detected '
                 + '(window.chrome.webview / window._InoWebUIHost both absent). '
                 + 'send/on are no-ops in this environment.');
    }
    return;
  }

  function validChannel(channel) {
    return typeof channel === 'string' && channel.length > 0;
  }

  window.InoWebUI = {
    version: '1.1',

    send: function(channel, payload) {
      if (!validChannel(channel)) {
        if (console && console.error) {
          console.error('InoWebUI.send: channel must be a non-empty string');
        }
        return;
      }
      try {
        var envelope = { channel: channel, payload: payload };
        transport.send(JSON.stringify(envelope));
      } catch (e) { console.error('InoWebUI.send failed:', e); }
    },

    on: function(channel, handler) {
      if (!validChannel(channel) || typeof handler !== 'function') return;
      if (!listeners[channel]) listeners[channel] = [];
      listeners[channel].push(handler);
    },

    off: function(channel, handler) {
      var arr = listeners[channel];
      if (!arr) return;
      var i = arr.indexOf(handler);
      if (i >= 0) arr.splice(i, 1);
    },

    once: function(channel, handler) {
      if (!validChannel(channel) || typeof handler !== 'function') return;
      var self = this;
      function wrapped(payload) {
        self.off(channel, wrapped);
        handler(payload);
      }
      this.on(channel, wrapped);
    }
  };

  transport.install(function(data) {
    try {
      var envelope = (typeof data === 'string') ? JSON.parse(data) : data;
      if (!envelope || typeof envelope.channel !== 'string') return;

      var arr = listeners[envelope.channel];
      if (!arr) return;

      // Snapshot before iterating so a handler that calls off() (or on())
      // during dispatch can't skip the next handler or run a removed one.
      var snapshot = arr.slice();
      for (var i = 0; i < snapshot.length; i++) {
        try { snapshot[i](envelope.payload); }
        catch (e) { console.error('InoWebUI handler error:', e); }
      }
    } catch (e) { console.error('InoWebUI receive error:', e); }
  });
})();
