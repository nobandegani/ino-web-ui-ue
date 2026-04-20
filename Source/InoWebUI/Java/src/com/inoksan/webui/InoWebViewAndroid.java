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
import android.webkit.JavascriptInterface;
import android.webkit.MimeTypeMap;
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
    }

    /** id → WebView */
    private static final SparseArray<WebView> sWebViews = new SparseArray<>();

    /** id → mutable Config. Parallel array to sWebViews. */
    private static final SparseArray<Config> sConfigs = new SparseArray<>();

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
            if (c != null && c.messagingEnabled)
            {
                // Inject the bridge as early as possible so page scripts can
                // rely on window.InoWebUI being present.
                view.evaluateJavascript(BRIDGE_JS, null);
            }
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

    /** JNI function implemented in InoWebViewImpl_Android.cpp.
     *  Called on whatever thread WebView chooses (usually not UI thread);
     *  C++ side marshals onto the UE game thread before firing delegates. */
    private static native void nativeOnMessageReceived(int id, String envelopeJson);

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

                // Single unified client drives virtual-host + bridge injection
                // + (future) nav events + lockdown. Config flips flags on sConfigs.
                wv.setWebViewClient(new InoWebViewClient(id));

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
    //  Layout / bounds
    // ─────────────────────────────────────────────────────────────────────
    public static void syncBounds(final int id,
                                  final int x, final int y,
                                  final int width, final int height)
    {
        final Activity activity = getActivity();
        if (activity == null) return;

        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv == null) return;

                FrameLayout.LayoutParams lp;
                if (width <= 0 || height <= 0) {
                    lp = new FrameLayout.LayoutParams(
                            ViewGroup.LayoutParams.MATCH_PARENT,
                            ViewGroup.LayoutParams.MATCH_PARENT);
                } else {
                    lp = new FrameLayout.LayoutParams(width, height);
                    lp.leftMargin = x;
                    lp.topMargin  = y;
                }
                wv.setLayoutParams(lp);
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
