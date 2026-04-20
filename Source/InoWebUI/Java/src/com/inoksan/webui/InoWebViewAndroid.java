// Copyright Inoksan. All Rights Reserved.
//
// InoWebViewAndroid — Java-side helper that owns all android.webkit.WebView
// instances for the InoWebUI plugin. Called by C++ via JNI (see
// Source/InoWebUI/Private/Impl/Android/InoWebViewImpl_Android.cpp).
//
// Every public static method here dispatches its real work onto the Android
// UI thread via Activity.runOnUiThread — WebView is NOT thread-safe and must
// only be touched from the UI thread, whereas the JNI calls arrive on UE's
// game thread.

package com.inoksan.webui;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.util.SparseArray;
import android.view.View;
import android.view.ViewGroup;
import android.os.Message;
import android.webkit.CookieManager;
import android.webkit.JavascriptInterface;
import android.webkit.RenderProcessGoneDetail;
import android.webkit.JsPromptResult;
import android.webkit.JsResult;
import android.webkit.MimeTypeMap;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;

import java.io.File;
import java.io.FileInputStream;
import java.util.Collections;

import com.epicgames.unreal.GameActivity;
import com.epicgames.unreal.Logger;

public class InoWebViewAndroid
{
    private static final Logger Log = new Logger("UE", "InoWebUI");

    /** Per-WebView configuration, read by the unified InoWebViewClient on
     *  every navigation event. Mutable: virtual-host / messaging / other
     *  config methods flip fields here after createWebView. */
    private static class Config
    {
        // Virtual host
        String virtualHost;         // null = disabled
        File   virtualHostFolder;   // null = disabled
        String virtualHostPrefix;   // "https://<virtualHost>/" — derived

        // Messaging
        boolean messagingEnabled;   // true once setupMessaging has been called

        // Lockdown
        boolean  lockToVirtualHost;
        String[] allowedURIPatterns;

        // Hardening
        boolean allowScriptDialogs;
        boolean allowNewWindows;

        // Dev overlay (the floating circular dev-tools button)
        boolean devOverlayEnabled;
    }

    /** Kind values match the C++ EInoScriptDialogKind enum in InoWebUITypes.h.
     *  Pass one of these constants through nativeOnScriptDialog. */
    private static final int DIALOG_KIND_ALERT         = 0;
    private static final int DIALOG_KIND_CONFIRM       = 1;
    private static final int DIALOG_KIND_PROMPT        = 2;
    private static final int DIALOG_KIND_BEFORE_UNLOAD = 3;

    /** id → WebView */
    private static final SparseArray<WebView> sWebViews = new SparseArray<>();

    /** id → mutable Config. Parallel array to sWebViews. */
    private static final SparseArray<Config> sConfigs = new SparseArray<>();

    /** Dev overlay — a floating circular dev-tools button in the bottom-right
     *  corner of the page. Injected on every page load only when
     *  config.devOverlayEnabled is true (which mirrors FInoWebViewConfig::
     *  bEnableDevTools). If you modify this, ALSO update the identical copy
     *  in InoWebViewImpl_Windows.cpp's GInoWebUIDevToolsOverlayScript. */
    private static final String DEVTOOLS_OVERLAY_JS = String.join("\n",
        "(function() {",
        "  if (window.__inoDevOverlayLoaded) return;",
        "  window.__inoDevOverlayLoaded = true;",
        "  function detectPlatform(){",
        "    if(window.chrome&&window.chrome.webview)return 'Windows (WebView2)';",
        "    if(window._InoWebUIHost)return 'Android (WebView)';",
        "    return 'Browser (no bridge)';",
        "  }",
        "  function showInfoModal(){",
        "    var existing=document.getElementById('__ino-dev-info-modal');",
        "    if(existing){existing.remove();return;}",
        "    var info={",
        "      'URL':location.href,",
        "      'Title':document.title||'(none)',",
        "      'Platform':detectPlatform(),",
        "      'Viewport':window.innerWidth+' x '+window.innerHeight,",
        "      'Screen':screen.width+' x '+screen.height,",
        "      'Device Pixel Ratio':window.devicePixelRatio,",
        "      'Language':navigator.language,",
        "      'Online':navigator.onLine?'yes':'no',",
        "      'Touch':('ontouchstart' in window)?'yes':'no',",
        "      'InoWebUI bridge':window.InoWebUI?('loaded v'+window.InoWebUI.version):'NOT loaded',",
        "      'User Agent':navigator.userAgent",
        "    };",
        "    var modal=document.createElement('div');",
        "    modal.id='__ino-dev-info-modal';",
        "    modal.style.cssText='position:fixed;inset:0;z-index:2147483646;background:rgba(0,0,0,0.62);backdrop-filter:blur(4px);-webkit-backdrop-filter:blur(4px);display:flex;align-items:center;justify-content:center;pointer-events:auto;padding:24px;font-family:-apple-system,BlinkMacSystemFont,\\\"Segoe UI\\\",Roboto,sans-serif;';",
        "    var card=document.createElement('div');",
        "    card.style.cssText=S('background:rgba(18,18,26,0.96);border:1px solid rgba(255,255,255,0.12);border-radius:14px;padding:20px 24px;max-width:560px;width:100%;color:#e7ecf3;font-size:13px;box-shadow:0 20px 60px rgba(0,0,0,0.5);');",
        "    var header=document.createElement('div');",
        "    header.style.cssText='display:flex;align-items:center;justify-content:space-between;margin-bottom:14px;';",
        "    var h=document.createElement('div');h.textContent='WebView Info';",
        "    h.style.cssText='font-size:15px;font-weight:600;color:#fff;';",
        "    var x=document.createElement('button');x.textContent='\\u00D7';",
        "    x.style.cssText=S('background:transparent;border:0;color:rgba(255,255,255,0.6);font-size:22px;cursor:pointer;padding:0 4px;line-height:1;');",
        "    header.appendChild(h);header.appendChild(x);card.appendChild(header);",
        "    var table=document.createElement('div');",
        "    table.style.cssText='display:grid;grid-template-columns:auto 1fr;gap:6px 14px;';",
        "    Object.keys(info).forEach(function(k){",
        "      var kEl=document.createElement('div');kEl.textContent=k;",
        "      kEl.style.cssText='color:rgba(255,255,255,0.55);font-size:12px;';",
        "      var vEl=document.createElement('div');vEl.textContent=info[k];",
        "      vEl.style.cssText='font-family:monospace;font-size:12px;word-break:break-all;user-select:text;-webkit-user-select:text;';",
        "      table.appendChild(kEl);table.appendChild(vEl);",
        "    });",
        "    card.appendChild(table);",
        "    modal.appendChild(card);",
        "    x.addEventListener('click',function(){modal.remove();});",
        "    modal.addEventListener('click',function(e){if(e.target===modal)modal.remove();});",
        "    document.body.appendChild(modal);",
        "  }",
        "  var ACTIONS = [",
        "    { id: 'refresh',            icon: '\\u21BB', title: 'Refresh' },",
        "    { id: 'openDevTools',       icon: '\\u2325', title: 'Open DevTools' },",
        "    { id: 'clearData',          icon: '\\u232B', title: 'Clear Data' },",
        "    { id: 'info',               icon: '\\u24D8', title: 'Info', handler: function(){showInfoModal();} },",
        "    { id: 'toggleTransparency', icon: '\\u25C9', title: 'Toggle Transparency' },",
        "    { id: 'hideWebUI',          icon: '\\u2298', title: 'Hide WebUI' },",
        "    { id: 'devCallback',        icon: '\\u25C6', title: 'Dev Callback' }",
        "  ];",
        "  var POSITIONS = [",
        "    { tx: 0, ty: -140 }, { tx: -36, ty: -135 },",
        "    { tx: -70, ty: -121 }, { tx: -99, ty: -99 },",
        "    { tx: -121, ty: -70 }, { tx: -135, ty: -36 },",
        "    { tx: -140, ty: 0 }",
        "  ];",
        "  function S(x){return 'all:initial;font-family:-apple-system,BlinkMacSystemFont,\\\"Segoe UI\\\",Roboto,sans-serif;line-height:1;color:#fff;'+x;}",
        "  function build(){",
        "    var root=document.createElement('div');",
        "    root.id='__ino-dev-overlay';",
        "    root.style.cssText='position:fixed;bottom:16px;right:16px;width:180px;height:180px;pointer-events:none;z-index:2147483647;';",
        "    var main=document.createElement('button');",
        "    main.textContent='\\u2699';",
        "    main.style.cssText=S('position:absolute;bottom:0;right:0;width:52px;height:52px;border-radius:50%;background:rgba(20,20,28,0.82);border:1px solid rgba(255,255,255,0.18);box-shadow:0 6px 24px rgba(0,0,0,0.4);cursor:pointer;pointer-events:auto;display:flex;align-items:center;justify-content:center;font-size:24px;transition:transform 0.2s,background 0.2s;');",
        "    root.appendChild(main);",
        "    var children=[];",
        "    ACTIONS.forEach(function(a,i){",
        "      var pos=POSITIONS[i];",
        "      var btn=document.createElement('button');",
        "      btn.textContent=a.icon;",
        "      btn.title=a.title;",
        "      btn.style.cssText=S('position:absolute;bottom:6px;right:6px;width:40px;height:40px;border-radius:50%;background:rgba(20,20,28,0.9);border:1px solid rgba(255,255,255,0.12);box-shadow:0 4px 12px rgba(0,0,0,0.4);cursor:pointer;pointer-events:none;display:flex;align-items:center;justify-content:center;font-size:18px;opacity:0;transform:translate(0,0) scale(0.3);transition:transform 0.25s cubic-bezier(0.175,0.885,0.32,1.275),opacity 0.2s,background 0.15s;');",
        "      btn.addEventListener('click',function(e){",
        "        e.stopPropagation();",
        "        collapse();",
        "        if(a.handler){a.handler();return;}",
        "        if(window.InoWebUI&&typeof window.InoWebUI.send==='function'){",
        "          try{window.InoWebUI.send('_devtools.'+a.id,{});}catch(err){console.error('InoDevOverlay:',err);}",
        "        }",
        "      });",
        "      btn.addEventListener('mouseenter',function(){",
        "        if(expanded){btn.style.transform='translate('+pos.tx+'px,'+pos.ty+'px) scale(1.12)';btn.style.background='rgba(60,60,80,0.95)';}",
        "      });",
        "      btn.addEventListener('mouseleave',function(){",
        "        if(expanded){btn.style.transform='translate('+pos.tx+'px,'+pos.ty+'px) scale(1)';btn.style.background='rgba(20,20,28,0.9)';}",
        "      });",
        "      root.appendChild(btn);",
        "      children.push({btn:btn,pos:pos});",
        "    });",
        "    var expanded=false;",
        "    function expand(){",
        "      expanded=true;",
        "      main.style.transform='rotate(45deg)';",
        "      main.style.background='rgba(60,60,80,0.92)';",
        "      children.forEach(function(c){",
        "        c.btn.style.opacity='1';c.btn.style.pointerEvents='auto';",
        "        c.btn.style.transform='translate('+c.pos.tx+'px,'+c.pos.ty+'px) scale(1)';",
        "      });",
        "    }",
        "    function collapse(){",
        "      expanded=false;",
        "      main.style.transform='rotate(0deg)';",
        "      main.style.background='rgba(20,20,28,0.82)';",
        "      children.forEach(function(c){",
        "        c.btn.style.opacity='0';c.btn.style.pointerEvents='none';",
        "        c.btn.style.transform='translate(0,0) scale(0.3)';",
        "      });",
        "    }",
        "    main.addEventListener('click',function(e){e.stopPropagation();if(expanded)collapse();else expand();});",
        "    main.addEventListener('mouseenter',function(){if(!expanded)main.style.transform='scale(1.08)';});",
        "    main.addEventListener('mouseleave',function(){if(!expanded)main.style.transform='scale(1)';});",
        "    document.addEventListener('click',function(e){if(expanded&&!root.contains(e.target))collapse();});",
        "    return root;",
        "  }",
        "  function mount(){if(document.body)document.body.appendChild(build());else document.addEventListener('DOMContentLoaded',mount);}",
        "  mount();",
        "})();"
    );

    /** Bridge JS injected on every page load when messagingEnabled. Matches
     *  the Windows-side window.InoWebUI API exactly: send / on / off. */
    private static final String BRIDGE_JS =
        "(function(){"
        + "if(window.InoWebUI)return;"
        + "var listeners={};"
        + "window.InoWebUI={version:'1.0',"
        + "  send:function(channel,payload){"
        + "    try{window._InoWebUIHost.receive(JSON.stringify({channel:String(channel),payload:payload}));}"
        + "    catch(e){console.error('InoWebUI.send failed:',e);}"
        + "  },"
        + "  on:function(channel,handler){"
        + "    if(typeof handler!=='function')return;"
        + "    if(!listeners[channel])listeners[channel]=[];"
        + "    listeners[channel].push(handler);"
        + "  },"
        + "  off:function(channel,handler){"
        + "    var a=listeners[channel];if(!a)return;"
        + "    var i=a.indexOf(handler);if(i>=0)a.splice(i,1);"
        + "  }"
        + "};"
        + "window._InoWebUIDispatch=function(env){"
        + "  try{"
        + "    if(typeof env==='string')env=JSON.parse(env);"
        + "    if(!env||typeof env.channel!=='string')return;"
        + "    var a=listeners[env.channel];if(!a)return;"
        + "    for(var i=0;i<a.length;i++){"
        + "      try{a[i](env.payload);}catch(e){console.error('InoWebUI handler error:',e);}"
        + "    }"
        + "  }catch(e){console.error('InoWebUI receive error:',e);}"
        + "};"
        + "})();";

    private static Activity getActivity()
    {
        Activity a = GameActivity.Get();
        if (a == null) Log.warn("GameActivity not available (called too early?)");
        return a;
    }

    private static Config getOrCreateConfig(int id)
    {
        Config c = sConfigs.get(id);
        if (c == null) { c = new Config(); sConfigs.put(id, c); }
        return c;
    }

    // ─────────────────────────────────────────────────────────────────────
    //  The unified WebViewClient — installed once in createWebView, reads
    //  from the per-id Config on every callback.
    // ─────────────────────────────────────────────────────────────────────
    private static class InoWebViewClient extends WebViewClient
    {
        private final int id;
        InoWebViewClient(int id) { this.id = id; }

        @Override
        public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request)
        {
            Config c = sConfigs.get(id);
            if (c == null || c.virtualHostPrefix == null || c.virtualHostFolder == null)
            {
                return null; // virtual-host disabled — let the default loader handle it
            }

            final String url = request.getUrl().toString();
            if (!url.startsWith(c.virtualHostPrefix)) return null;

            String path = url.substring(c.virtualHostPrefix.length());
            int q = path.indexOf('?'); if (q >= 0) path = path.substring(0, q);
            int h = path.indexOf('#'); if (h >= 0) path = path.substring(0, h);
            if (path.contains("..")) return notFound();
            if (path.isEmpty()) path = "index.html";

            File file = new File(c.virtualHostFolder, path);
            if (!file.isFile()) return notFound();

            try {
                FileInputStream fis = new FileInputStream(file);
                return new WebResourceResponse(guessMimeType(file.getName()), null, fis);
            } catch (Exception e) {
                Log.error("shouldInterceptRequest: " + e.getMessage());
                return notFound();
            }
        }

        @Override
        public void onPageStarted(WebView view, String url, Bitmap favicon)
        {
            Config c = sConfigs.get(id);
            if (c == null) return;

            // Inject the bridge as early as possible so page scripts can
            // rely on window.InoWebUI being present.
            if (c.messagingEnabled)
            {
                view.evaluateJavascript(BRIDGE_JS, null);
            }
            // Dev overlay runs AFTER the bridge because its buttons call
            // window.InoWebUI.send(...).
            if (c.devOverlayEnabled)
            {
                view.evaluateJavascript(DEVTOOLS_OVERLAY_JS, null);
            }
        }

        @Override
        public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request)
        {
            if (request == null) return false;
            final String url = request.getUrl().toString();

            // Lockdown check. Matches the C++ rule in InoWebViewImpl_Windows's
            // IsURIAllowed helper: internal schemes always pass, then
            // virtualHost-match, then the user's AllowedURIPatterns list.
            Config c = sConfigs.get(id);
            if (!isURIAllowed(url, c))
            {
                Log.warn("Navigation blocked by lockdown: " + url);
                nativeOnNavigationStarting(id, url);  // observation, even though blocked
                return true; // cancel
            }

            nativeOnNavigationStarting(id, url);
            return false; // let the navigation proceed
        }

        @Override
        public void onPageFinished(WebView view, String url)
        {
            nativeOnNavigationCompleted(id, true, url);
        }

        @Override
        public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error)
        {
            if (request != null && request.isForMainFrame())
            {
                nativeOnNavigationCompleted(id, false, request.getUrl().toString());
            }
        }

        @Override
        public boolean onRenderProcessGone(WebView view, RenderProcessGoneDetail detail)
        {
            // Renderer process died (OOM or crash). Fire the callback; the
            // WebView is now unusable and the caller should Reload() or
            // recreate. Returning true means "we handled it — don't bubble
            // up and crash the app."
            String desc = (detail != null && detail.didCrash())
                    ? "renderer crashed"
                    : "renderer killed by system (OOM?)";
            nativeOnProcessFailed(id, desc);
            return true;
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    //  WebChromeClient — surface page events that live on the Chrome side
    //  (title changes, dialogs in later commits, window.open in later).
    // ─────────────────────────────────────────────────────────────────────
    private static class InoWebChromeClient extends WebChromeClient
    {
        private final int id;
        InoWebChromeClient(int id) { this.id = id; }

        @Override
        public void onReceivedTitle(WebView view, String title)
        {
            if (title != null) nativeOnDocumentTitleChanged(id, title);
        }

        // ── JS dialog suppression ────────────────────────────────────────
        // Default: cancel the dialog (alert returns, confirm→false, prompt→null).
        // When allowScriptDialogs=true, accept() lets the native dialog show.
        // Always fire OnScriptDialog for observation.

        @Override
        public boolean onJsAlert(WebView view, String url, String message, JsResult result)
        {
            nativeOnScriptDialog(id, DIALOG_KIND_ALERT, message);
            Config c = sConfigs.get(id);
            if (c != null && c.allowScriptDialogs) result.confirm();
            else                                   result.cancel();
            return true;
        }

        @Override
        public boolean onJsConfirm(WebView view, String url, String message, JsResult result)
        {
            nativeOnScriptDialog(id, DIALOG_KIND_CONFIRM, message);
            Config c = sConfigs.get(id);
            if (c != null && c.allowScriptDialogs) result.confirm();
            else                                   result.cancel();
            return true;
        }

        @Override
        public boolean onJsPrompt(WebView view, String url, String message,
                                  String defaultValue, JsPromptResult result)
        {
            nativeOnScriptDialog(id, DIALOG_KIND_PROMPT, message);
            Config c = sConfigs.get(id);
            if (c != null && c.allowScriptDialogs) result.confirm(defaultValue != null ? defaultValue : "");
            else                                   result.cancel();
            return true;
        }

        @Override
        public boolean onJsBeforeUnload(WebView view, String url, String message, JsResult result)
        {
            nativeOnScriptDialog(id, DIALOG_KIND_BEFORE_UNLOAD, message);
            Config c = sConfigs.get(id);
            if (c != null && c.allowScriptDialogs) result.confirm();
            else                                   result.cancel();
            return true;
        }

        // ── window.open / target="_blank" blocking ─────────────────────────
        // Android's protocol for discovering the URL a JS window.open wants
        // is the "transport WebView" trick: attach a throwaway WebView to
        // the resultMsg so Android tells it what URL to load. We intercept
        // in shouldOverrideUrlLoading, fire our callback, and cancel.
        @Override
        public boolean onCreateWindow(WebView view, boolean isDialog,
                                      boolean isUserGesture, Message resultMsg)
        {
            final Config c = sConfigs.get(id);
            // Ignore allowNewWindows — we always intercept to get the URL.
            // A BP handler can LoadURL(uri) to redirect into the same frame
            // if desired; opening a real popup would look terrible in-game.

            WebView transport = new WebView(view.getContext());
            transport.setWebViewClient(new WebViewClient() {
                @Override
                public boolean shouldOverrideUrlLoading(WebView v, WebResourceRequest req)
                {
                    if (req != null) nativeOnNewWindowRequested(id, req.getUrl().toString());
                    return true; // don't actually navigate the throwaway
                }
            });
            ((WebView.WebViewTransport) resultMsg.obj).setWebView(transport);
            resultMsg.sendToTarget();
            return true; // consumed
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    //  JS-side bridge object — exposed via addJavascriptInterface as
    //  window._InoWebUIHost. The injected BRIDGE_JS calls .receive() on
    //  it, which hops across JNI into C++.
    // ─────────────────────────────────────────────────────────────────────
    private static class InoWebBridge
    {
        private final int id;
        InoWebBridge(int id) { this.id = id; }

        @JavascriptInterface
        public void receive(String envelopeJson)
        {
            nativeOnMessageReceived(id, envelopeJson);
        }
    }

    /** JNI functions implemented in InoWebViewImpl_Android.cpp. All are
     *  called on arbitrary threads; C++ marshals onto the game thread
     *  before firing the corresponding BP delegates. */
    private static native void nativeOnMessageReceived     (int id, String envelopeJson);
    private static native void nativeOnNavigationStarting  (int id, String uri);
    private static native void nativeOnNavigationCompleted (int id, boolean success, String uri);
    private static native void nativeOnDocumentTitleChanged(int id, String title);
    private static native void nativeOnScriptDialog        (int id, int kind, String message);
    private static native void nativeOnNewWindowRequested  (int id, String uri);
    private static native void nativeOnGotFocus            (int id);
    private static native void nativeOnLostFocus           (int id);
    private static native void nativeOnProcessFailed       (int id, String description);

    // ─────────────────────────────────────────────────────────────────────
    //  Create / Destroy
    // ─────────────────────────────────────────────────────────────────────

    public static void createWebView(final int id,
                                     final boolean transparent,
                                     final boolean visible)
    {
        final Activity activity = getActivity();
        if (activity == null) return;

        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                if (sWebViews.get(id) != null) {
                    Log.warn("createWebView(" + id + "): already exists");
                    return;
                }

                WebView wv = new WebView(activity);
                if (transparent) wv.setBackgroundColor(Color.TRANSPARENT);

                wv.getSettings().setJavaScriptEnabled(true);
                wv.getSettings().setDomStorageEnabled(true);

                // Required for WebChromeClient.onCreateWindow to fire;
                // our handler intercepts and blocks the popup explicitly.
                wv.getSettings().setSupportMultipleWindows(true);
                wv.getSettings().setJavaScriptCanOpenWindowsAutomatically(true);

                // Single unified client drives virtual-host + bridge injection
                // + nav events + lockdown. Config flips flags on sConfigs.
                wv.setWebViewClient(new InoWebViewClient(id));
                wv.setWebChromeClient(new InoWebChromeClient(id));

                // OnGotFocus / OnLostFocus parity with the Windows impl.
                wv.setOnFocusChangeListener(new View.OnFocusChangeListener() {
                    @Override public void onFocusChange(View v, boolean hasFocus) {
                        if (hasFocus) nativeOnGotFocus(id);
                        else          nativeOnLostFocus(id);
                    }
                });

                wv.setVisibility(visible ? View.VISIBLE : View.GONE);

                ViewGroup root = (ViewGroup) activity.findViewById(android.R.id.content);
                if (root == null) {
                    Log.error("createWebView(" + id + "): content view not found");
                    return;
                }

                FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT);
                root.addView(wv, lp);

                sWebViews.put(id, wv);
                getOrCreateConfig(id);
                Log.debug("createWebView(" + id + ")");
            }
        });
    }

    public static void destroyWebView(final int id)
    {
        final Activity activity = getActivity();
        if (activity == null) return;

        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv == null) return;
                ViewGroup parent = (ViewGroup) wv.getParent();
                if (parent != null) parent.removeView(wv);
                wv.destroy();
                sWebViews.remove(id);
                sConfigs.remove(id);
                Log.debug("destroyWebView(" + id + ")");
            }
        });
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Virtual host — flips flags on the config; the unified client reads
    //  them from shouldInterceptRequest. Must be called BEFORE loadURL so
    //  the initial navigation hits the handler.
    // ─────────────────────────────────────────────────────────────────────
    public static void setVirtualHost(final int id, final String host, final String folder)
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        if (host == null || host.isEmpty() || folder == null || folder.isEmpty()) return;

        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                Config c = getOrCreateConfig(id);
                c.virtualHost       = host;
                c.virtualHostFolder = new File(folder);
                c.virtualHostPrefix = "https://" + host + "/";
                if (!c.virtualHostFolder.isDirectory()) {
                    Log.warn("setVirtualHost(" + id + "): folder does not exist: " + folder);
                }
                Log.debug("setVirtualHost(" + id + "): " + c.virtualHostPrefix + " -> " + folder);
            }
        });
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Lockdown — a URI is allowed iff any of:
    //    1. Internal scheme (about:, data:, blob:)
    //    2. lockToVirtualHost == false
    //    3. Host matches virtualHost (case-insensitive)
    //    4. Full URI matches any pattern in allowedURIPatterns
    //  Matches the Windows-side rule exactly.
    // ─────────────────────────────────────────────────────────────────────
    public static void configureLockdown(final int id,
                                         final boolean lockToVirtualHost,
                                         final String[] allowedPatterns)
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                Config c = getOrCreateConfig(id);
                c.lockToVirtualHost  = lockToVirtualHost;
                c.allowedURIPatterns = allowedPatterns;
                Log.debug("configureLockdown(" + id + "): locked=" + lockToVirtualHost
                        + ", allowed=" + (allowedPatterns == null ? 0 : allowedPatterns.length));
            }
        });
    }

    private static String extractHost(String uri)
    {
        if (uri == null) return null;
        int schemeEnd = uri.indexOf("://");
        if (schemeEnd < 0) return null;
        String rest = uri.substring(schemeEnd + 3);

        int at = rest.indexOf('@');
        if (at >= 0) rest = rest.substring(at + 1);

        int end = rest.length();
        for (char ch : new char[]{'/', '?', '#', ':'})
        {
            int i = rest.indexOf(ch);
            if (i >= 0 && i < end) end = i;
        }
        return rest.substring(0, end);
    }

    private static boolean isURIAllowed(String uri, Config c)
    {
        if (uri == null || uri.isEmpty()) return true;
        if (uri.startsWith("about:") || uri.startsWith("data:") || uri.startsWith("blob:"))
            return true;
        if (c == null || !c.lockToVirtualHost) return true;

        // Virtual host: whole-host match, case-insensitive.
        if (c.virtualHost != null && !c.virtualHost.isEmpty())
        {
            String host = extractHost(uri);
            if (host != null && host.equalsIgnoreCase(c.virtualHost)) return true;
        }

        // Wildcard allowlist.
        if (c.allowedURIPatterns != null)
        {
            for (String pattern : c.allowedURIPatterns)
            {
                if (matchesWildcard(uri, pattern)) return true;
            }
        }
        return false;
    }

    /** UE-style wildcard match: "*" = any run, "?" = single char. */
    private static boolean matchesWildcard(String s, String pattern)
    {
        if (pattern == null || pattern.isEmpty()) return false;
        StringBuilder re = new StringBuilder();
        for (int i = 0; i < pattern.length(); i++)
        {
            char c = pattern.charAt(i);
            switch (c)
            {
                case '*': re.append(".*"); break;
                case '?': re.append('.');  break;
                case '.': case '\\': case '(': case ')': case '[': case ']':
                case '{': case '}': case '|': case '+': case '^': case '$':
                    re.append('\\').append(c); break;
                default:
                    re.append(c);
            }
        }
        return s.matches(re.toString());
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Hardening — JS dialog suppression + window.open blocking.
    // ─────────────────────────────────────────────────────────────────────
    public static void configureDialogs(final int id,
                                        final boolean allowScriptDialogs,
                                        final boolean allowNewWindows)
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                Config c = getOrCreateConfig(id);
                c.allowScriptDialogs = allowScriptDialogs;
                c.allowNewWindows    = allowNewWindows;
            }
        });
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Messaging — expose the JS host bridge and flip the injection flag.
    //  Must be called BEFORE loadURL so the bridge is available for the
    //  first page's scripts.
    // ─────────────────────────────────────────────────────────────────────
    public static void setupMessaging(final int id)
    {
        final Activity activity = getActivity();
        if (activity == null) return;

        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv == null) return;

                // addJavascriptInterface exposes the object as a named JS
                // global. We pick a name (_InoWebUIHost) that user code is
                // unlikely to collide with; the injected BRIDGE_JS wraps
                // it behind the nice window.InoWebUI.send() API.
                wv.addJavascriptInterface(new InoWebBridge(id), "_InoWebUIHost");

                getOrCreateConfig(id).messagingEnabled = true;
                Log.debug("setupMessaging(" + id + ")");
            }
        });
    }

    /** UE → JS. Delivers an envelope {channel, payload} to any window.InoWebUI.on
     *  subscribers on the page. envelopeJson must be a valid JSON object literal. */
    public static void postMessageJson(final int id, final String envelopeJson)
    {
        final Activity activity = getActivity();
        if (activity == null || envelopeJson == null) return;

        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv == null) return;

                // JSON is a subset of JS expression syntax, so we can inline
                // the envelope directly — no additional escaping needed.
                wv.evaluateJavascript("window._InoWebUIDispatch && window._InoWebUIDispatch("
                        + envelopeJson + ")", null);
            }
        });
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Navigation / visibility
    // ─────────────────────────────────────────────────────────────────────

    public static void loadURL(final int id, final String url)
    {
        final Activity activity = getActivity();
        if (activity == null) return;

        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv != null && url != null) wv.loadUrl(url);
            }
        });
    }

    public static void setVisible(final int id, final boolean visible)
    {
        final Activity activity = getActivity();
        if (activity == null) return;

        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv != null) wv.setVisibility(visible ? View.VISIBLE : View.GONE);
            }
        });
    }

    public static void reload(final int id)
    {
        final Activity activity = getActivity();
        if (activity == null) return;

        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv != null) wv.reload();
            }
        });
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Focus / zoom / cookies
    // ─────────────────────────────────────────────────────────────────────

    public static void focusWebView(final int id)
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv == null) return;
                wv.requestFocus();
                wv.requestFocusFromTouch(); // covers the "no touch yet" edge case
            }
        });
    }

    /** Factor is 1.0 = 100%, 1.5 = 150%. Android's API takes an int percent. */
    public static void setZoomFactor(final int id, final float factor)
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv == null) return;
                int percent = Math.round(factor * 100.0f);
                if (percent < 1) percent = 1;
                wv.setInitialScale(percent);
            }
        });
    }

    /** Clear cookies across all WebViews in this app. Async internally; we
     *  don't surface the completion callback (fire-and-forget, matching
     *  Windows's ClearAllCookies). */
    public static void clearAllCookies(final int id)
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                CookieManager.getInstance().removeAllCookies(null);
                CookieManager.getInstance().flush();
                Log.debug("clearAllCookies(" + id + ")");
            }
        });
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Phase 3 polish — DevTools / ExecuteJS / UserAgent / context menus
    // ─────────────────────────────────────────────────────────────────────

    /** Enable remote Chromium DevTools inspection for every WebView in the
     *  process. Connect Android via USB, open chrome://inspect/#devices in
     *  desktop Chrome, and you'll see this WebView listed. The "id" arg is
     *  unused (the setting is process-wide) but kept for API symmetry. */
    public static void setDevToolsEnabled(final int id, final boolean enabled)
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                // Process-wide remote-debugging toggle (affects every WebView
                // in the app, regardless of id — Android's design).
                WebView.setWebContentsDebuggingEnabled(enabled);

                // Per-WebView: flip the flag that InoWebViewClient.onPageStarted
                // reads to decide whether to inject the floating dev overlay.
                getOrCreateConfig(id).devOverlayEnabled = enabled;

                if (enabled) {
                    Log.debug("DevTools enabled — dev overlay will inject on next page "
                            + "load; chrome://inspect available for remote Chrome DevTools");
                }
            }
        });
    }

    public static void executeJavaScript(final int id, final String code)
    {
        final Activity activity = getActivity();
        if (activity == null || code == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv != null) wv.evaluateJavascript(code, null);
            }
        });
    }

    public static void setUserAgent(final int id, final String userAgent)
    {
        final Activity activity = getActivity();
        if (activity == null || userAgent == null || userAgent.isEmpty()) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv != null) wv.getSettings().setUserAgentString(userAgent);
            }
        });
    }

    /** Toggle the WebView's background between transparent and opaque at
     *  runtime. Useful for the dev overlay's "Toggle transparency" button. */
    public static void setBackgroundOpaque(final int id, final boolean opaque)
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv == null) return;
                wv.setBackgroundColor(opaque ? Color.WHITE : Color.TRANSPARENT);
            }
        });
    }

    /** Suppress the browser's built-in long-press context menu (text-select,
     *  "save image", etc.). Text fields still show the system copy/paste
     *  toolbar via the standard IME — we only kill the BROWSER menu. */
    public static void setContextMenusEnabled(final int id, final boolean enabled)
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv == null) return;
                if (enabled) {
                    wv.setOnLongClickListener(null);     // back to default
                    wv.setLongClickable(true);
                } else {
                    wv.setOnLongClickListener(new android.view.View.OnLongClickListener() {
                        @Override public boolean onLongClick(android.view.View v) {
                            return true; // consume — no menu
                        }
                    });
                    wv.setLongClickable(false);
                }
            }
        });
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Layout / bounds
    // ─────────────────────────────────────────────────────────────────────
    /**
     * On Android we deliberately ignore the size values the UE subsystem
     * sends and keep MATCH_PARENT sizing.
     *
     * Why: UE's SWindow::GetClientRectInScreen reports a coordinate system
     * that doesn't line up with Android FrameLayout's physical-pixel layout
     * params (density scaling mismatch + UE's "virtual window" concept
     * doesn't map to Android's full-screen activity model). Blindly using
     * those values leaves the WebView covering only ~1/3 of the screen on
     * a typical ~3x-density phone.
     *
     * Android games are always fullscreen; the activity's content
     * FrameLayout fills the screen; the WebView as a child of that root
     * with MATCH_PARENT is exactly what we want. Custom sub-region sizing
     * can be added later with explicit DP → px conversion when there's a
     * concrete use case.
     */
    public static void syncBounds(final int id,
                                  final int /*x*/ ignoredX, final int /*y*/ ignoredY,
                                  final int /*width*/ ignoredW, final int /*height*/ ignoredH)
    {
        final Activity activity = getActivity();
        if (activity == null) return;

        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv == null) return;

                // Only set MATCH_PARENT if it's not already MATCH_PARENT —
                // avoids a layout pass every resize event for no reason.
                ViewGroup.LayoutParams cur = wv.getLayoutParams();
                if (cur != null
                    && cur.width  == ViewGroup.LayoutParams.MATCH_PARENT
                    && cur.height == ViewGroup.LayoutParams.MATCH_PARENT)
                {
                    return;
                }
                wv.setLayoutParams(new FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT));
            }
        });
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Activity lifecycle forwarding (called from GameActivity via UPL)
    // ─────────────────────────────────────────────────────────────────────

    public static void onActivityPause()
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                for (int i = 0; i < sWebViews.size(); i++) sWebViews.valueAt(i).onPause();
            }
        });
    }

    public static void onActivityResume()
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                for (int i = 0; i < sWebViews.size(); i++) sWebViews.valueAt(i).onResume();
            }
        });
    }

    public static void onActivityDestroy()
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                for (int i = 0; i < sWebViews.size(); i++) {
                    WebView wv = sWebViews.valueAt(i);
                    ViewGroup parent = (ViewGroup) wv.getParent();
                    if (parent != null) parent.removeView(wv);
                    wv.destroy();
                }
                sWebViews.clear();
                sConfigs.clear();
            }
        });
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Helpers
    // ─────────────────────────────────────────────────────────────────────

    private static WebResourceResponse notFound()
    {
        return new WebResourceResponse("text/plain", "UTF-8", 404, "Not Found",
                Collections.<String, String>emptyMap(), null);
    }

    private static String guessMimeType(String filename)
    {
        int dot = filename.lastIndexOf('.');
        String ext = (dot >= 0 && dot + 1 < filename.length())
                ? filename.substring(dot + 1).toLowerCase()
                : "";

        String mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
        if (mime != null) return mime;

        switch (ext) {
            case "js":    case "mjs":   return "application/javascript";
            case "css":                 return "text/css";
            case "html":  case "htm":   return "text/html";
            case "json":                return "application/json";
            case "svg":                 return "image/svg+xml";
            case "woff":                return "font/woff";
            case "woff2":               return "font/woff2";
            case "wasm":                return "application/wasm";
            default:                    return "application/octet-stream";
        }
    }
}
