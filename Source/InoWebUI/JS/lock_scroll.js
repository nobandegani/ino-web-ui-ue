// InoWebUI — disable outer-page scroll for game-UI overlays.
//
// Edit this file then run Plugins/InoWebUI/Scripts/GenerateJSConstants.ps1
// to regenerate the C++/Java/Obj-C constants.
//
// Injected on every page load when FInoWebViewConfig::bAllowOuterScroll
// is false (the default). Sets html / body to overflow:hidden so vertical
// drag inside the WebView doesn't drift the whole UI up / down. Long
// content should use a CSS overflow:auto inner container instead.
(function() {
  var STYLE_ID = '__inoLockScroll';
  if (document.getElementById(STYLE_ID)) return;

  var css = 'html,body{'
          + 'overflow:hidden!important;'
          + 'height:100%!important;'
          + 'margin:0!important;'
          + 'overscroll-behavior:none!important;'
          + 'touch-action:pan-x pan-y!important;'
          + '}';

  function install() {
    if (document.getElementById(STYLE_ID)) return;
    var head = document.head || document.getElementsByTagName('head')[0] || document.documentElement;
    if (!head) return;
    var s = document.createElement('style');
    s.id = STYLE_ID;
    s.textContent = css;
    head.appendChild(s);
  }

  if (document.head) install();
  else if (document.addEventListener) {
    document.addEventListener('readystatechange', function() {
      if (document.readyState !== 'loading') install();
    });
    document.addEventListener('DOMContentLoaded', install);
  }
})();
