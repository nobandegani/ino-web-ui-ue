// InoWebUI iOS content-world bridge — pageWorld ↔ defaultClientWorld.
//
// Edit this file then run Plugins/InoWebUI/Scripts/GenerateJSConstants.ps1
// to regenerate the iOS Obj-C constant. (No Win64 / Android emission for
// this script — iOS-only.)
//
// Why this exists
// ───────────────
// To stop page JavaScript from spoofing or intercepting the UE↔JS message
// bridge, the iOS impl installs the WK message handler `_InoWebUIHost` and
// the cross-platform `bridge.js` into `WKContentWorld.defaultClientWorld`.
// That hides the bridge from page scripts — but page code still needs
// `window.InoWebUI.send / on / off / once` to work the same as on Win64
// and Android.
//
// This script is injected into BOTH worlds. It detects which world it's
// in by checking for `_InoWebUIHost`:
//
//   • defaultClientWorld (handler IS present) → it's the RELAY:
//       – Listens for outbound page→bridge DOM events and forwards them
//         via the in-world window.InoWebUI.send() that bridge.js exposes.
//       – Wraps _InoWebUIDispatch so every native→JS message also echoes
//         to pageWorld via a DOM event.
//
//   • pageWorld (handler NOT present) → it's the SHIM:
//       – Exposes window.InoWebUI with the same API as bridge.js so page
//         code keeps working unchanged.
//       – send() dispatches a DOM event the relay listens for.
//       – on/off/once track handlers locally and fire on inbound events.
//
// Cross-world payload transfer rides JSON strings on `CustomEvent.detail`
// so we don't depend on WebKit's content-world structured-cloning rules
// for arbitrary objects — strings are unambiguously copied across worlds.
// ES5-only so it works on any page regardless of transpile target.
(function() {
  if (typeof window === 'undefined') return;
  if (window.__inoIOSWorldBridgeInstalled) return;
  window.__inoIOSWorldBridgeInstalled = true;

  // Presence of _InoWebUIHost is the canonical "I'm in defaultClientWorld"
  // signal — it's the WK message handler the native side installed there,
  // invisible from pageWorld.
  var IS_ISOLATED = !!(window.webkit
                    && window.webkit.messageHandlers
                    && window.webkit.messageHandlers._InoWebUIHost);

  // DOM CustomEvent names — chosen with a __ino prefix so they can't
  // collide with the page's own events.
  var EVT_TO_BRIDGE = '__inoUIToBridge';  // pageWorld → defaultClientWorld
  var EVT_TO_PAGE   = '__inoUIToPage';    // defaultClientWorld → pageWorld

  if (IS_ISOLATED) {
    // ── defaultClientWorld RELAY ──────────────────────────────────────────
    // bridge.js was added to this world BEFORE this script in the iOS
    // impl's Initialize, so its IIFE has already run and window.InoWebUI
    // is up. If for any reason it isn't, the relay is a no-op.
    document.addEventListener(EVT_TO_BRIDGE, function(e) {
      if (!window.InoWebUI) return;
      var json = e && e.detail;
      if (typeof json !== 'string') return;
      var msg;
      try { msg = JSON.parse(json); }
      catch (err) { return; }
      if (!msg || typeof msg.channel !== 'string') return;
      try { window.InoWebUI.send(msg.channel, msg.payload); }
      catch (err) {
        if (typeof console !== 'undefined' && console.error) {
          console.error('InoWebUI relay send error:', err);
        }
      }
    });

    // Wrap _InoWebUIDispatch so each native→JS message also echoes to
    // pageWorld. bridge.js installed _InoWebUIDispatch in its transport
    // .install() step before we ran.
    var origDispatch = window._InoWebUIDispatch;
    window._InoWebUIDispatch = function(data) {
      // Fire the in-world dispatch first (so bridge.js's own listeners
      // and dev_overlay handlers run synchronously, before the pageWorld
      // echo lands as an async DOM event).
      try { if (origDispatch) origDispatch(data); }
      catch (err) {}
      // Echo to pageWorld. The data may already be a JSON string (Phase
      // 2 wire format) or a structured object (defensive); normalise.
      try {
        var json = (typeof data === 'string') ? data : JSON.stringify(data);
        document.dispatchEvent(new CustomEvent(EVT_TO_PAGE, { detail: json }));
      } catch (err) {}
    };
  } else {
    // ── pageWorld SHIM ────────────────────────────────────────────────────
    // If the page (or another script) already defined window.InoWebUI,
    // step aside.
    if (window.InoWebUI) return;

    var listeners = Object.create(null);

    function validChannel(c) { return typeof c === 'string' && c.length > 0; }

    document.addEventListener(EVT_TO_PAGE, function(e) {
      var json = e && e.detail;
      if (typeof json !== 'string') return;
      var env;
      try { env = JSON.parse(json); }
      catch (err) { return; }
      if (!env || typeof env.channel !== 'string') return;
      var arr = listeners[env.channel];
      if (!arr) return;
      // Snapshot before iterating (same shape as bridge.js) so a handler
      // that calls .off() during dispatch can't skip neighbours.
      var snapshot = arr.slice();
      for (var i = 0; i < snapshot.length; i++) {
        try { snapshot[i](env.payload); }
        catch (err) {
          if (typeof console !== 'undefined' && console.error) {
            console.error('InoWebUI page handler:', err);
          }
        }
      }
    });

    window.InoWebUI = {
      version: '1.2',

      send: function(channel, payload) {
        if (!validChannel(channel)) {
          if (typeof console !== 'undefined' && console.error) {
            console.error('InoWebUI.send: channel must be a non-empty string');
          }
          return;
        }
        try {
          var json = JSON.stringify({ channel: channel, payload: payload });
          document.dispatchEvent(new CustomEvent(EVT_TO_BRIDGE, { detail: json }));
        } catch (err) {
          if (typeof console !== 'undefined' && console.error) {
            console.error('InoWebUI.send failed:', err);
          }
        }
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
  }
})();
