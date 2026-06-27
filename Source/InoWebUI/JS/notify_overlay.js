// InoWebUI notification overlay — stacked corner toasts for on-screen logs.
//
// Edit this file then run Plugins/InoWebUI/Scripts/GenerateJSConstants.ps1
// to regenerate the C++/Java/Obj-C constants.
//
// Injected at document-start on every page, right after bridge.js. The toast
// container is invisible until a notification arrives, so it costs nothing on
// pages that never call it.
//
// Two ways to raise a toast:
//   • From UE  — UInoWebView::ShowNotification(Text, Category, Duration) sends
//                window.InoWebUI.PostMessage('_notify.show', { text, category,
//                durationMs }); this overlay subscribes via .on('_notify.show').
//   • From the page itself — window.InoNotify.show(text, category, durationMs)
//                or window.InoNotify.clear().
//
// Category ∈ { 'info', 'warning', 'error' } (anything else → 'info').
// durationMs ≤ 0 → DEFAULT_MS. The '_notify.' channel is UE→JS only, so it
// never reaches the user's OnMessageReceived delegate.
//
// Self-contained: every element uses `all:initial` (via S()) so the host
// page's CSS can't bleed in, and inline styles only.
(function() {
  if (window.__inoNotifyLoaded) return;
  window.__inoNotifyLoaded = true;

  var DEFAULT_MS = 4000;   // used when duration <= 0
  var MAX_STACK  = 6;      // oldest toast is evicted past this
  var SPRING     = 'cubic-bezier(0.34,1.56,0.64,1)';

  // Per-category visual identity: accent colour + inline SVG glyph path.
  var CATS = {
    info:    { color: '#4fb0ff', svg: '<circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8h.01"/>' },
    warning: { color: '#fbbf24', svg: '<path d="M12 3l9 16H3z"/><path d="M12 10v4M12 17h.01"/>' },
    error:   { color: '#fb6f84', svg: '<circle cx="12" cy="12" r="9"/><path d="M15 9l-6 6M9 9l6 6"/>' }
  };

  function S(extras) {
    return 'all:initial;box-sizing:border-box;'
         + 'font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Inter,sans-serif;'
         + 'line-height:1.35;color:#eef2f8;' + extras;
  }

  function icon(path, size, color) {
    return '<svg viewBox="0 0 24 24" width="' + size + '" height="' + size + '" '
         + 'fill="none" stroke="' + color + '" stroke-width="2" '
         + 'stroke-linecap="round" stroke-linejoin="round" '
         + 'style="display:block;pointer-events:none;flex:none">' + path + '</svg>';
  }

  var root = null;   // the fixed stacking container, lazily created

  function ensureRoot() {
    if (root) return root;
    // Scoped keyframes so they can't clash with the page.
    var st = document.createElement('style');
    st.textContent =
      '@keyframes __inoToastIn{from{opacity:0;transform:translateX(40px) scale(0.96)}}';
    (document.head || document.documentElement).appendChild(st);

    root = document.createElement('div');
    root.id = '__ino-notify-overlay';
    // Top-right corner; column-reverse so newest sits on top of the stack.
    root.style.cssText = 'position:fixed;top:16px;right:16px;'
      + 'z-index:2147483645;pointer-events:none;'
      + 'display:flex;flex-direction:column-reverse;align-items:flex-end;gap:10px;'
      + 'max-width:min(380px,calc(100vw - 32px));';
    mountRoot();
    return root;
  }

  function mountRoot() {
    if (document.body) document.body.appendChild(root);
    else document.addEventListener('DOMContentLoaded', function() { document.body.appendChild(root); });
  }

  function dismiss(card) {
    if (card.__inoGone) return;
    card.__inoGone = true;
    if (card.__inoTimer) { clearTimeout(card.__inoTimer); card.__inoTimer = null; }
    card.style.transition = 'opacity 0.25s ease,transform 0.3s ' + SPRING + ',margin 0.3s ease';
    card.style.opacity = '0';
    card.style.transform = 'translateX(40px) scale(0.96)';
    // Collapse the gap it occupied so the rest of the stack slides smoothly.
    card.style.marginBottom = (-card.offsetHeight) + 'px';
    setTimeout(function() { if (card.parentNode) card.parentNode.removeChild(card); }, 320);
  }

  function show(text, category, durationMs) {
    var cat = CATS[category] || CATS.info;
    var ms = (typeof durationMs === 'number' && durationMs > 0) ? durationMs : DEFAULT_MS;
    var r = ensureRoot();

    var card = document.createElement('div');
    card.style.cssText = S('position:relative;display:flex;align-items:flex-start;gap:11px;'
      + 'pointer-events:auto;cursor:pointer;width:100%;'
      + 'padding:12px 14px 12px 14px;border-radius:13px;'
      + 'background:rgba(20,24,40,0.9);'
      + 'border:1px solid rgba(255,255,255,0.12);'
      + 'border-left:3px solid ' + cat.color + ';'
      + 'box-shadow:0 10px 28px rgba(0,0,0,0.5);'
      + 'backdrop-filter:blur(16px) saturate(150%);'
      + '-webkit-backdrop-filter:blur(16px) saturate(150%);'
      + 'animation:__inoToastIn 0.34s ' + SPRING + ';'
      + 'transition:transform 0.15s ' + SPRING + ',border-color 0.15s;');

    var ic = document.createElement('div');
    ic.innerHTML = icon(cat.svg, 18, cat.color);
    ic.style.cssText = 'margin-top:1px;';

    var msg = document.createElement('div');
    msg.textContent = (text == null) ? '' : String(text);
    msg.style.cssText = S('flex:1;min-width:0;font-size:13px;font-weight:500;'
      + 'color:#eef2f8;word-break:break-word;white-space:pre-wrap;'
      + 'user-select:text;-webkit-user-select:text;');

    card.appendChild(ic);
    card.appendChild(msg);

    // Click anywhere on the toast to dismiss early.
    card.addEventListener('click', function() { dismiss(card); });
    card.addEventListener('mouseenter', function() {
      card.style.transform = 'scale(1.015)';
      card.style.borderColor = 'rgba(255,255,255,0.22)';
      // Pause auto-dismiss while hovered.
      if (card.__inoTimer) { clearTimeout(card.__inoTimer); card.__inoTimer = null; }
    });
    card.addEventListener('mouseleave', function() {
      card.style.transform = 'scale(1)';
      card.style.borderColor = 'rgba(255,255,255,0.12)';
      if (!card.__inoGone) card.__inoTimer = setTimeout(function() { dismiss(card); }, 1200);
    });

    r.appendChild(card);

    // Evict the oldest (last child, since the column is reversed) past the cap.
    while (r.children.length > MAX_STACK) {
      var oldest = r.children[0];
      if (oldest && !oldest.__inoGone) dismiss(oldest); else break;
    }

    card.__inoTimer = setTimeout(function() { dismiss(card); }, ms);
    return card;
  }

  function clear() {
    if (!root) return;
    var kids = Array.prototype.slice.call(root.children);
    for (var i = 0; i < kids.length; i++) dismiss(kids[i]);
  }

  // Page-facing API.
  window.InoNotify = { show: show, clear: clear };

  // Subscribe to UE → JS pushes. bridge.js is injected immediately before this
  // script (document-start, bridge-first ordering is guaranteed on every
  // platform), so window.InoWebUI is already present. Retry defensively in
  // case of the legacy Android onPageStarted path where ordering is best-effort.
  function subscribe() {
    if (window.InoWebUI && typeof window.InoWebUI.on === 'function') {
      window.InoWebUI.on('_notify.show', function(d) {
        if (!d) return;
        show(d.text, d.category, d.durationMs);
      });
      return true;
    }
    return false;
  }
  if (!subscribe()) {
    var tries = 0;
    var iv = setInterval(function() {
      if (subscribe() || ++tries > 40) clearInterval(iv);   // ~4s max
    }, 100);
  }
})();
