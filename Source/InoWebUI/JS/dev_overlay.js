// InoWebUI dev-tools overlay — floating gear FAB + a clean vertical toolbar.
//
// Edit this file then run Plugins/InoWebUI/Scripts/GenerateJSConstants.ps1
// to regenerate the C++/Java/Obj-C constants.
//
// Injected only when FInoWebViewConfig::bEnableDevTools is true. Actions:
//   • Refresh      → window.InoWebUI.send('_devtools.refresh', {})  → UInoWebView::Reload
//   • DevTools     → window.InoWebUI.send('_devtools.openDevTools', {}) → UInoWebView::OpenDevTools
//                    (Windows: real Chromium DevTools window; Android: logs
//                     chrome://inspect hint; iOS: logs Safari Web Inspector hint)
//   • Info         → JS-only modal (never hops to UE)
//   • Dev Callback → window.InoWebUI.send('_devtools.devCallback', {}) → OnDevCallback
// UInoWebView intercepts the '_devtools.' prefix in DispatchIncomingEnvelope;
// user OnMessageReceived never sees these.
//
// Self-contained: every element uses `all:initial` (via S()) so the host
// page's CSS can't bleed in, and inline styles only (the overlay is injected
// into arbitrary pages, it can't rely on the showcase's CSS variables).
(function() {
  if (window.__inoDevOverlayLoaded) return;
  window.__inoDevOverlayLoaded = true;

  var GRAD = 'linear-gradient(115deg,#7c8cff 0%,#4fdce4 55%,#ff7eb6 100%)';
  var SPRING = 'cubic-bezier(0.34,1.56,0.64,1)';

  // 'handler' = JS-only (no UE hop). Order = top-to-bottom in the stack.
  var ACTIONS = [
    { id: 'refresh',     label: 'Refresh',  svg: '<path d="M20 11a8 8 0 1 0-2.3 5.6M20 5v6h-6"/>' },
    { id: 'openDevTools', label: 'DevTools', svg: '<path d="M9 8l-3.5 4 3.5 4M15 8l3.5 4-3.5 4"/>' },
    { id: 'info',        label: 'Info',     svg: '<circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8h.01"/>',
      handler: function() { showInfoModal(); } },
    { id: 'devCallback', label: 'Dev Callback', svg: '<path d="M12 3l2.4 5 5.6.8-4 4 1 5.6-5-2.7-5 2.7 1-5.6-4-4 5.6-.8z"/>' }
  ];

  function detectPlatform() {
    if (window.chrome && window.chrome.webview) return 'Windows (WebView2)';
    if (window.webkit && window.webkit.messageHandlers
                      && window.webkit.messageHandlers._InoWebUIHost) return 'iOS (WKWebView)';
    if (window._InoWebUIHost) return 'Android (WebView)';
    return 'Browser (no bridge)';
  }

  function S(extras) {
    return 'all:initial;box-sizing:border-box;'
         + 'font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Inter,sans-serif;'
         + 'line-height:1;color:#eef2f8;' + extras;
  }

  // Inline SVG icon with the page-proof reset baked into a wrapper span.
  function icon(path, size) {
    return '<svg viewBox="0 0 24 24" width="' + size + '" height="' + size + '" '
         + 'fill="none" stroke="currentColor" stroke-width="2" '
         + 'stroke-linecap="round" stroke-linejoin="round" '
         + 'style="display:block;pointer-events:none">' + path + '</svg>';
  }

  function showInfoModal() {
    var existing = document.getElementById('__ino-dev-info-modal');
    if (existing) { existing.remove(); return; }

    var info = {
      'URL':                location.href,
      'Title':              document.title || '(none)',
      'Platform':           detectPlatform(),
      'Viewport':           window.innerWidth + ' x ' + window.innerHeight,
      'Screen':             screen.width + ' x ' + screen.height,
      'Device Pixel Ratio': window.devicePixelRatio,
      'Language':           navigator.language,
      'Online':             navigator.onLine ? 'yes' : 'no',
      'Touch':              ('ontouchstart' in window) ? 'yes' : 'no',
      'InoWebUI bridge':    window.InoWebUI ? ('loaded v' + window.InoWebUI.version) : 'NOT loaded',
      'User Agent':         navigator.userAgent
    };

    var modal = document.createElement('div');
    modal.id = '__ino-dev-info-modal';
    modal.style.cssText = S('position:fixed;inset:0;z-index:2147483646;'
      + 'background:rgba(5,6,12,0.6);backdrop-filter:blur(8px);'
      + '-webkit-backdrop-filter:blur(8px);'
      + 'display:flex;align-items:center;justify-content:center;'
      + 'pointer-events:auto;padding:24px;'
      + 'animation:__inoFade 0.2s ease;');

    var card = document.createElement('div');
    card.style.cssText = S('position:relative;background:rgba(24,28,46,0.94);'
      + 'border:1px solid rgba(255,255,255,0.14);border-radius:18px;'
      + 'padding:0 0 20px;max-width:560px;width:100%;'
      + 'box-shadow:0 24px 70px rgba(0,0,0,0.6);overflow:hidden;'
      + 'animation:__inoPop 0.4s ' + SPRING + ';');

    var accent = document.createElement('div');
    accent.style.cssText = 'height:3px;background:' + GRAD + ';';
    card.appendChild(accent);

    var header = document.createElement('div');
    header.style.cssText = 'display:flex;align-items:center;justify-content:space-between;'
      + 'padding:18px 22px 14px;';
    var h = document.createElement('div');
    h.textContent = 'WebView Info';
    h.style.cssText = S('font-size:15px;font-weight:700;letter-spacing:-0.01em;');
    var x = document.createElement('button');
    x.innerHTML = icon('<path d="M6 6l12 12M18 6L6 18"/>', 16);
    x.style.cssText = S('display:flex;align-items:center;justify-content:center;'
      + 'width:28px;height:28px;border-radius:8px;background:rgba(255,255,255,0.05);'
      + 'color:rgba(255,255,255,0.6);cursor:pointer;');
    x.addEventListener('mouseenter', function() { x.style.background = 'rgba(251,111,132,0.2)'; x.style.color = '#fb6f84'; });
    x.addEventListener('mouseleave', function() { x.style.background = 'rgba(255,255,255,0.05)'; x.style.color = 'rgba(255,255,255,0.6)'; });
    header.appendChild(h); header.appendChild(x);
    card.appendChild(header);

    var table = document.createElement('div');
    table.style.cssText = 'display:grid;grid-template-columns:auto 1fr;'
      + 'gap:9px 16px;padding:0 22px;';
    Object.keys(info).forEach(function(k) {
      var kEl = document.createElement('div');
      kEl.textContent = k;
      kEl.style.cssText = S('color:rgba(255,255,255,0.5);font-size:12px;');
      var vEl = document.createElement('div');
      vEl.textContent = info[k];
      vEl.style.cssText = S('font-family:ui-monospace,"SF Mono",Menlo,Consolas,monospace;'
        + 'font-size:12px;word-break:break-all;color:#cdd5e6;'
        + 'user-select:text;-webkit-user-select:text;');
      table.appendChild(kEl); table.appendChild(vEl);
    });
    card.appendChild(table);

    modal.appendChild(card);
    x.addEventListener('click', function() { modal.remove(); });
    modal.addEventListener('click', function(e) { if (e.target === modal) modal.remove(); });
    document.body.appendChild(modal);
  }

  function build() {
    // Keyframes (scoped via unique names so they can't clash with the page).
    var st = document.createElement('style');
    st.textContent =
      '@keyframes __inoFade{from{opacity:0}}' +
      '@keyframes __inoPop{from{opacity:0;transform:scale(0.85) translateY(16px)}}';
    document.head.appendChild(st);

    var root = document.createElement('div');
    root.id = '__ino-dev-overlay';
    root.style.cssText = 'position:fixed;bottom:18px;right:18px;'
      + 'z-index:2147483647;pointer-events:none;'
      + 'display:flex;flex-direction:column;align-items:flex-end;gap:10px;';

    // Action pills (rendered above the FAB, hidden until expanded).
    var stack = document.createElement('div');
    stack.style.cssText = 'display:flex;flex-direction:column;align-items:flex-end;gap:8px;';

    var pills = [];
    ACTIONS.forEach(function(a, i) {
      var p = document.createElement('button');
      p.title = a.label;
      p.innerHTML =
        '<span style="display:flex;align-items:center;justify-content:center;'
          + 'width:26px;height:26px;border-radius:8px;background:rgba(255,255,255,0.07);'
          + 'flex:none">' + icon(a.svg, 15) + '</span>'
        + '<span style="font-size:13px;font-weight:600;white-space:nowrap">' + a.label + '</span>';
      p.style.cssText = S('display:flex;align-items:center;gap:10px;'
        + 'padding:8px 16px 8px 8px;border-radius:999px;cursor:pointer;'
        + 'background:rgba(20,24,40,0.86);'
        + 'border:1px solid rgba(255,255,255,0.12);'
        + 'box-shadow:0 8px 22px rgba(0,0,0,0.45);'
        + 'backdrop-filter:blur(16px) saturate(150%);'
        + '-webkit-backdrop-filter:blur(16px) saturate(150%);'
        + 'pointer-events:none;opacity:0;'
        + 'transform:translateY(14px) scale(0.92);'
        + 'transition:opacity 0.2s ease,transform 0.3s ' + SPRING
        + ',background 0.15s,border-color 0.15s;');
      p.addEventListener('mouseenter', function() {
        p.style.background = 'rgba(40,46,72,0.92)';
        p.style.borderColor = 'rgba(255,255,255,0.22)';
      });
      p.addEventListener('mouseleave', function() {
        p.style.background = 'rgba(20,24,40,0.86)';
        p.style.borderColor = 'rgba(255,255,255,0.12)';
      });
      p.addEventListener('click', function(e) {
        e.stopPropagation();
        collapse();
        if (a.handler) { a.handler(); return; }
        if (window.InoWebUI && typeof window.InoWebUI.send === 'function') {
          try { window.InoWebUI.send('_devtools.' + a.id, {}); }
          catch (err) { if (window.console) console.error('InoDevOverlay:', err); }
        }
      });
      stack.appendChild(p);
      pills.push(p);
    });
    root.appendChild(stack);

    // Gear FAB.
    var fab = document.createElement('button');
    fab.title = 'InoWebUI dev tools';
    fab.innerHTML = '<span id="__inoGear" style="display:block;transition:transform 0.35s '
      + SPRING + '">'
      + icon('<path d="M12 8.5A3.5 3.5 0 1 0 12 15.5 3.5 3.5 0 0 0 12 8.5z"/>'
           + '<path d="M19.4 13a7.6 7.6 0 0 0 .1-2l2-1.5-2-3.4-2.3 1a7.6 7.6 0 0 0-1.7-1l-.3-2.5h-4l-.3 2.5a7.6 7.6 0 0 0-1.7 1l-2.3-1-2 3.4 2 1.5a7.6 7.6 0 0 0 0 2l-2 1.5 2 3.4 2.3-1a7.6 7.6 0 0 0 1.7 1l.3 2.5h4l.3-2.5a7.6 7.6 0 0 0 1.7-1l2.3 1 2-3.4z"/>', 22)
      + '</span>';
    fab.style.cssText = S('width:50px;height:50px;border-radius:50%;cursor:pointer;'
      + 'pointer-events:auto;display:flex;align-items:center;justify-content:center;'
      + 'background:rgba(20,24,40,0.86);'
      + 'border:1px solid rgba(255,255,255,0.16);'
      + 'box-shadow:0 8px 26px rgba(0,0,0,0.5);'
      + 'backdrop-filter:blur(16px) saturate(150%);'
      + '-webkit-backdrop-filter:blur(16px) saturate(150%);'
      + 'transition:transform 0.2s ' + SPRING + ',background 0.2s,box-shadow 0.2s;'
      + 'flex:none;');

    // Always-visible FPS chip, docked left of the gear FAB. Measures the
    // WebView layer's own frame rate (UI jank), NOT UE's render thread.
    var fps = document.createElement('div');
    fps.title = 'WebView UI frame rate';
    fps.style.cssText = S('display:flex;align-items:center;gap:7px;'
      + 'height:34px;padding:0 13px;border-radius:999px;pointer-events:auto;'
      + 'background:rgba(20,24,40,0.86);'
      + 'border:1px solid rgba(255,255,255,0.12);'
      + 'box-shadow:0 8px 22px rgba(0,0,0,0.45);'
      + 'backdrop-filter:blur(16px) saturate(150%);'
      + '-webkit-backdrop-filter:blur(16px) saturate(150%);'
      + 'font-variant-numeric:tabular-nums;flex:none;');
    fps.innerHTML =
      '<span id="__inoFpsDot" style="width:7px;height:7px;border-radius:50%;'
        + 'background:#43e08b;flex:none;transition:background 0.3s,box-shadow 0.3s"></span>'
      + '<span style="font-size:10.5px;font-weight:700;letter-spacing:0.08em;'
        + 'color:rgba(255,255,255,0.5)">FPS</span>'
      + '<b id="__inoFpsN" style="font-size:14px;font-weight:800;color:#eef2f8;'
        + 'min-width:22px;text-align:right">--</b>';

    // Bottom row: [ FPS chip ] [ gear FAB ]. The action stack expands above.
    var bottom = document.createElement('div');
    bottom.style.cssText = 'display:flex;align-items:center;gap:10px;';
    bottom.appendChild(fps);
    bottom.appendChild(fab);
    root.appendChild(bottom);

    // FPS sampler — rAF frame count, refreshed every ~500ms, colour-coded.
    (function() {
      var nEl = fps.querySelector('#__inoFpsN');
      var dEl = fps.querySelector('#__inoFpsDot');
      var last = (window.performance && performance.now) ? performance.now() : Date.now();
      var frames = 0, acc = 0;
      function tick(t) {
        if (t == null) t = (window.performance && performance.now) ? performance.now() : Date.now();
        frames++; acc += t - last; last = t;
        if (acc >= 500) {
          var v = Math.round(frames * 1000 / acc);
          frames = 0; acc = 0;
          nEl.textContent = v;
          var col = v >= 55 ? '#43e08b' : (v >= 30 ? '#fbbf24' : '#fb6f84');
          nEl.style.color = col;
          dEl.style.background = col;
          dEl.style.boxShadow = '0 0 8px ' + col;
        }
        requestAnimationFrame(tick);
      }
      requestAnimationFrame(tick);
    })();

    var expanded = false;
    var gear = fab.querySelector('#__inoGear');
    function expand() {
      expanded = true;
      gear.style.transform = 'rotate(120deg)';
      fab.style.background = GRAD;
      fab.style.color = '#07070d';
      fab.style.boxShadow = '0 10px 30px rgba(124,140,255,0.5)';
      pills.forEach(function(p, i) {
        setTimeout(function() {
          p.style.opacity = '1';
          p.style.pointerEvents = 'auto';
          p.style.transform = 'translateY(0) scale(1)';
        }, i * 45);
      });
    }
    function collapse() {
      expanded = false;
      gear.style.transform = 'rotate(0deg)';
      fab.style.background = 'rgba(20,24,40,0.86)';
      fab.style.color = '#eef2f8';
      fab.style.boxShadow = '0 8px 26px rgba(0,0,0,0.5)';
      pills.forEach(function(p) {
        p.style.opacity = '0';
        p.style.pointerEvents = 'none';
        p.style.transform = 'translateY(14px) scale(0.92)';
      });
    }
    fab.addEventListener('click', function(e) {
      e.stopPropagation();
      if (expanded) collapse(); else expand();
    });
    fab.addEventListener('mouseenter', function() { if (!expanded) fab.style.transform = 'scale(1.08)'; });
    fab.addEventListener('mouseleave', function() { fab.style.transform = 'scale(1)'; });
    document.addEventListener('click', function(e) {
      if (expanded && !root.contains(e.target)) collapse();
    });

    return root;
  }

  function mount() {
    if (document.body) document.body.appendChild(build());
    else document.addEventListener('DOMContentLoaded', mount);
  }
  mount();
})();
