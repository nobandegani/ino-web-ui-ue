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
//   window.InoWebUI.send(channel, payload)   — push a message to UE
//   window.InoWebUI.on(channel, handler)     — subscribe to UE messages
//   window.InoWebUI.off(channel, handler)    — unsubscribe
//
// Wire format both directions: { channel: string, payload: any } as JSON.
// ES5-only (no const/let/arrow/Map) so it runs on any page regardless of
// the page's transpile target.
(function() {
  if (typeof window === 'undefined' || window.InoWebUI) return;

  var listeners = {};
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
    // No host bridge — running in a plain browser. Leave window.InoWebUI
    // unset so feature-detection (`if (window.InoWebUI)`) works as expected.
    return;
  }

  window.InoWebUI = {
    version: '1.0',
    send: function(channel, payload) {
      try {
        var envelope = { channel: String(channel), payload: payload };
        transport.send(JSON.stringify(envelope));
      } catch (e) { console.error('InoWebUI.send failed:', e); }
    },
    on: function(channel, handler) {
      if (typeof handler !== 'function') return;
      if (!listeners[channel]) listeners[channel] = [];
      listeners[channel].push(handler);
    },
    off: function(channel, handler) {
      var arr = listeners[channel];
      if (!arr) return;
      var i = arr.indexOf(handler);
      if (i >= 0) arr.splice(i, 1);
    }
  };

  transport.install(function(data) {
    try {
      var envelope = (typeof data === 'string') ? JSON.parse(data) : data;
      if (!envelope || typeof envelope.channel !== 'string') return;
      var arr = listeners[envelope.channel];
      if (!arr) return;
      for (var i = 0; i < arr.length; i++) {
        try { arr[i](envelope.payload); }
        catch (e) { console.error('InoWebUI handler error:', e); }
      }
    } catch (e) { console.error('InoWebUI receive error:', e); }
  });
})();
