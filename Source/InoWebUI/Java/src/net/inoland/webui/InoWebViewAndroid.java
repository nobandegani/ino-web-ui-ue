// Copyright Inoland. All Rights Reserved.
//
// InoWebViewAndroid — Java-side helper that owns all android.webkit.WebView
// instances for the InoWebUI plugin. Called by C++ via JNI (see
// Source/InoWebUI/Private/Impl/Android/InoWebViewImpl_Android.cpp).
//
// Every public static method here dispatches its real work onto the Android
// UI thread via Activity.runOnUiThread — WebView is NOT thread-safe and must
// only be touched from the UI thread, whereas the JNI calls arrive on UE's
// game thread.

package net.inoland.webui;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.util.SparseArray;
import android.view.View;
import android.view.ViewGroup;
import android.os.Message;
import android.graphics.Canvas;
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
import android.webkit.WebStorage;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

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

        // Manual bounds — when true, syncBounds applies the supplied X/Y/W/H
        // (with DP scaling) instead of forcing MATCH_PARENT. Default false.
        boolean manualBounds;
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

    // BRIDGE_JS and DEVTOOLS_OVERLAY_JS live in InoWebUIScripts.java —
    // generated from Source/InoWebUI/JS/*.js by GenerateJSConstants.ps1.

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

            // URL-decode percent-encodings BEFORE the traversal check and
            // before constructing the File, so:
            //   • the traversal check catches "%2E%2E/secret" → "../secret"
            //   • filenames with spaces or other escaped chars actually open
            //     ("foo%20bar" → "foo bar" instead of literal "foo%20bar")
            try {
                path = URLDecoder.decode(path, "UTF-8");
            } catch (Exception e) {
                Log.warn("vhost: URL-decode failed for " + url + ": " + e.getMessage());
                return notFound();
            }

            if (path.contains("..")) { Log.warn("vhost: rejected traversal in " + url); return notFound(); }
            if (path.isEmpty()) path = "index.html";

            File file = new File(c.virtualHostFolder, path);
            if (!file.isFile()) {
                Log.warn("vhost: file not found: " + file.getAbsolutePath()
                        + "  (root=" + c.virtualHostFolder + ", path=" + path + ")");
                return notFound();
            }

            try {
                FileInputStream fis = new FileInputStream(file);
                String mime = guessMimeType(file.getName());
                Log.debug("vhost: serving " + file.getAbsolutePath() + " as " + mime);
                // Explicit 200 status + non-null headers map — some Android WebView
                // versions react to null fields with ERR_INVALID_RESPONSE.
                return new WebResourceResponse(mime, "UTF-8", 200, "OK",
                        Collections.<String, String>emptyMap(), fis);
            } catch (Exception e) {
                Log.error("vhost: open failed for " + file.getAbsolutePath()
                        + ": " + e.getMessage());
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
                view.evaluateJavascript(InoWebUIScripts.BRIDGE_JS, null);
            }
            // Dev overlay runs AFTER the bridge because its buttons call
            // window.InoWebUI.send(...).
            if (c.devOverlayEnabled)
            {
                view.evaluateJavascript(InoWebUIScripts.DEVTOOLS_OVERLAY_JS, null);
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
            nativeOnNavStateChanged(id, view.canGoBack(), view.canGoForward());
        }

        @Override
        public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error)
        {
            if (request != null && request.isForMainFrame())
            {
                nativeOnNavigationCompleted(id, false, request.getUrl().toString());
                nativeOnNavStateChanged(id, view.canGoBack(), view.canGoForward());
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
    /** Fires after every page-end (success or fail) so C++ can cache nav-history flags. */
    private static native void nativeOnNavStateChanged     (int id, boolean canGoBack, boolean canGoForward);

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

                // Wrap the envelope in a JS string literal so the host->JS path
                // can't be broken by U+2028 / U+2029 (legal in JSON, illegal
                // in pre-ES2019 JS string literals) or by other unicode that
                // is valid JSON but invalid as a raw JS expression. The
                // bridge's dispatcher takes either a string (JSON.parse) or
                // an object — passing a string keeps both platforms uniform.
                String jsLiteral = jsStringLiteral(envelopeJson);
                wv.evaluateJavascript(
                    "window._InoWebUIDispatch && window._InoWebUIDispatch(" + jsLiteral + ")",
                    null);
            }
        });
    }

    /** Encode a string as a JS string literal (with surrounding quotes) safe
     *  to inline into a JS expression. Escapes everything that would break a
     *  pre-ES2019 string literal, including U+2028 / U+2029. */
    private static String jsStringLiteral(String s) {
        StringBuilder sb = new StringBuilder(s.length() + 16);
        sb.append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '\\': sb.append("\\\\"); break;
                case '"':  sb.append("\\\""); break;
                case '\n': sb.append("\\n");  break;
                case '\r': sb.append("\\r");  break;
                case '\t': sb.append("\\t");  break;
                case '\b': sb.append("\\b");  break;
                case '\f': sb.append("\\f");  break;
                case 0x2028: sb.append("\\u2028"); break;
                case 0x2029: sb.append("\\u2029"); break;
                default:
                    if (c < 0x20) {
                        sb.append(String.format("\\u%04x", (int) c));
                    } else {
                        sb.append(c);
                    }
            }
        }
        sb.append('"');
        return sb.toString();
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

    /**
     * Allow / disallow pinch + double-tap zoom on the WebView. When false
     * (default in InoWebUI), supportZoom + builtInZoomControls are both off.
     * displayZoomControls is always off (we never want the on-screen +/-
     * UI even if zoom itself is on, that's a browser concept).
     */
    public static void setAllowZoom(final int id, final boolean allow)
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv == null) return;
                wv.getSettings().setSupportZoom(allow);
                wv.getSettings().setBuiltInZoomControls(allow);
                wv.getSettings().setDisplayZoomControls(false);
            }
        });
    }

    /** Show / hide both scroll indicators. Scrolling itself still works. */
    public static void setShowScrollBars(final int id, final boolean show)
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv == null) return;
                wv.setVerticalScrollBarEnabled(show);
                wv.setHorizontalScrollBarEnabled(show);
            }
        });
    }

    /**
     * Allow / disallow HTML5 media to start without a user gesture. When true,
     * setMediaPlaybackRequiresUserGesture(false) — the page can autoplay
     * audio / video. When false, the user must tap before playback begins
     * (matches Chrome's default policy). Mirrors FInoWebViewSettings::
     * bAllowMediaAutoplay.
     */
    public static void setAllowMediaAutoplay(final int id, final boolean allow)
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv != null) wv.getSettings().setMediaPlaybackRequiresUserGesture(!allow);
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
     * Two modes:
     *
     *   • Auto (manualBounds=false, default): the supplied X/Y/W/H are
     *     ignored; the WebView is sized MATCH_PARENT inside the content
     *     FrameLayout. UE's `GetClientRectInScreen` reports values whose
     *     density scaling doesn't line up with FrameLayout's physical-pixel
     *     params, so the safe-by-default behavior is fullscreen.
     *
     *   • Manual (manualBounds=true): X/Y/W/H are honoured. The values
     *     arrive in UE pixels (== logical px); we scale to physical px
     *     using the activity's display density before applying.
     *
     * Mode is set with {@link #setBoundsMode}.
     */
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

                Config c = sConfigs.get(id);
                final boolean manual = (c != null && c.manualBounds);

                if (manual) {
                    // Convert UE-pixel (logical) values to physical pixels via
                    // density. This matches what FrameLayout.LayoutParams
                    // expects.
                    float density = activity.getResources().getDisplayMetrics().density;
                    int px = Math.round(x * density);
                    int py = Math.round(y * density);
                    int pw = Math.max(0, Math.round(width  * density));
                    int ph = Math.max(0, Math.round(height * density));

                    FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(pw, ph);
                    lp.leftMargin = px;
                    lp.topMargin  = py;
                    wv.setLayoutParams(lp);
                    return;
                }

                // Auto mode — keep MATCH_PARENT.
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

    /** Toggle manual-bounds mode for a WebView. See {@link #syncBounds}. */
    public static void setBoundsMode(final int id, final boolean manual)
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                getOrCreateConfig(id).manualBounds = manual;
            }
        });
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Browser-style nav
    // ─────────────────────────────────────────────────────────────────────

    public static void goBack(final int id)
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv != null && wv.canGoBack()) wv.goBack();
            }
        });
    }

    public static void goForward(final int id)
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv != null && wv.canGoForward()) wv.goForward();
            }
        });
    }

    /**
     * SYNCHRONOUS query — must be called on the UI thread, NOT from JNI.
     * The C++ side caches {@link CanGoBack} state instead.
     */
    public static boolean canGoBack(final int id)
    {
        WebView wv = sWebViews.get(id);
        return wv != null && wv.canGoBack();
    }

    public static boolean canGoForward(final int id)
    {
        WebView wv = sWebViews.get(id);
        return wv != null && wv.canGoForward();
    }

    public static void stopLoading(final int id)
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv != null) wv.stopLoading();
            }
        });
    }

    /**
     * UE → Android. base may be null/empty (passed as null to
     * loadDataWithBaseURL, in which case the page sees about:blank).
     */
    public static void loadHTMLString(final int id, final String html, final String base)
    {
        final Activity activity = getActivity();
        if (activity == null || html == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv == null) return;
                String b = (base == null || base.isEmpty()) ? null : base;
                wv.loadDataWithBaseURL(b, html, "text/html", "UTF-8", null);
            }
        });
    }

    /**
     * Set a single cookie via CookieManager. The cookie string is HTTP cookie
     * syntax: "name=value; Path=/; Expires=...; HttpOnly; Secure; SameSite=...".
     */
    public static void setCookie(final int id, final String url, final String cookie)
    {
        final Activity activity = getActivity();
        if (activity == null || url == null || cookie == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                CookieManager cm = CookieManager.getInstance();
                cm.setCookie(url, cookie);
                cm.flush();
            }
        });
    }

    /** Wipe cookies, cache, history, form data, and Web Storage. */
    public static void clearAllData(final int id)
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv != null) {
                    wv.clearCache(true);
                    wv.clearFormData();
                    wv.clearHistory();
                }
                WebStorage.getInstance().deleteAllData();
                CookieManager.getInstance().removeAllCookies(null);
                CookieManager.getInstance().flush();
                Log.debug("clearAllData(" + id + ")");
            }
        });
    }

    /**
     * Render the WebView into a Bitmap and write to outFilePath. Format = 0
     * for PNG, 1 for JPEG (matches EInoImageFormat). Async — fires
     * nativeOnCapturePreviewComplete with success bool when done.
     */
    public static void capturePreview(final int id, final int format, final String outFilePath)
    {
        final Activity activity = getActivity();
        if (activity == null || outFilePath == null) {
            nativeOnCapturePreviewComplete(id, false, outFilePath);
            return;
        }
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                boolean ok = false;
                try {
                    WebView wv = sWebViews.get(id);
                    if (wv == null) {
                        nativeOnCapturePreviewComplete(id, false, outFilePath);
                        return;
                    }
                    int w = Math.max(1, wv.getWidth());
                    int h = Math.max(1, wv.getHeight());
                    Bitmap bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
                    Canvas canvas = new Canvas(bmp);
                    wv.draw(canvas);

                    Bitmap.CompressFormat cf = (format == 1)
                            ? Bitmap.CompressFormat.JPEG
                            : Bitmap.CompressFormat.PNG;
                    FileOutputStream fos = new FileOutputStream(outFilePath);
                    try {
                        ok = bmp.compress(cf, 90, fos);
                    } finally {
                        try { fos.close(); } catch (Exception ignore) {}
                    }
                    bmp.recycle();
                } catch (Exception e) {
                    Log.error("capturePreview(" + id + ") failed: " + e.getMessage());
                    ok = false;
                }
                nativeOnCapturePreviewComplete(id, ok, outFilePath);
            }
        });
    }

    /**
     * Navigate with extra HTTP headers attached to the top-level request.
     * Headers arrive as parallel arrays so we can keep the JNI signature simple.
     */
    public static void loadURLWithHeaders(final int id, final String url,
                                          final String[] headerNames,
                                          final String[] headerValues)
    {
        final Activity activity = getActivity();
        if (activity == null || url == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv == null) return;
                Map<String, String> hs = new HashMap<>();
                if (headerNames != null && headerValues != null) {
                    int n = Math.min(headerNames.length, headerValues.length);
                    for (int i = 0; i < n; i++) {
                        if (headerNames[i] != null && headerValues[i] != null) {
                            hs.put(headerNames[i], headerValues[i]);
                        }
                    }
                }
                wv.loadUrl(url, hs);
            }
        });
    }

    /** JNI callback used by capturePreview to deliver the result back to C++. */
    private static native void nativeOnCapturePreviewComplete(int id, boolean success, String filePath);

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
        // Non-null body: WebResourceResponse with a null data stream can trigger
        // net::ERR_INVALID_RESPONSE on some Android WebView versions, which
        // looks nothing like a regular 404 to the user.
        byte[] body = "404 Not Found".getBytes(StandardCharsets.UTF_8);
        return new WebResourceResponse("text/plain", "UTF-8", 404, "Not Found",
                Collections.<String, String>emptyMap(),
                new ByteArrayInputStream(body));
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
