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
import android.graphics.Rect;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Message;
import android.util.SparseArray;
import android.view.PixelCopy;
import android.view.View;
import android.view.ViewGroup;
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

import androidx.webkit.WebViewCompat;
import androidx.webkit.WebViewFeature;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;

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

        // True once the bridge / dev-overlay have been registered through
        // WebViewCompat.addDocumentStartJavaScript (the pre-page-script
        // injection path). When set, InoWebViewClient.onPageStarted must NOT
        // re-inject via evaluateJavascript — the document-start API already
        // ran the script before any page code, exactly like Windows/iOS.
        // When the API is unsupported (very old System WebView) these stay
        // false and onPageStarted remains the fallback.
        boolean docStartBridgeInstalled;
        boolean docStartOverlayInstalled;
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

            // FALLBACK PATH ONLY. On any System WebView that supports
            // DOCUMENT_START_SCRIPT (≈Chrome 83+, i.e. effectively every
            // device since 2020) the bridge / overlay were already registered
            // via WebViewCompat.addDocumentStartJavaScript and ran BEFORE any
            // page script — re-injecting here would be a redundant second
            // copy (bridge.js self-guards with `if (window.InoWebUI) return;`
            // so it's harmless, but the overlay would mount twice). We only
            // inject here when the document-start API was unavailable.
            //
            // onPageStarted + evaluateJavascript has no ordering guarantee
            // against the page's own inline scripts; this legacy path is
            // therefore best-effort and exists purely so ancient WebView
            // builds still get a (late) bridge rather than none.
            if (c.messagingEnabled && !c.docStartBridgeInstalled)
            {
                view.evaluateJavascript(InoWebUIScripts.BRIDGE_JS, null);
            }
            // Dev overlay runs AFTER the bridge because its buttons call
            // window.InoWebUI.send(...).
            if (c.devOverlayEnabled && !c.docStartOverlayInstalled)
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
            // Renderer process died (OOM-killed by Android or subprocess
            // crash). Per Android docs the WebView is permanently unusable
            // after this point — calling any method on it from this point
            // on is undefined behavior. The doc-mandated cleanup is to
            // detach it from its parent and call destroy() promptly.
            //   https://developer.android.com/develop/ui/views/layout/webapps/managing-webview
            //
            // We do that here, on the UI thread, so the dead view's native
            // resources are released right away. Then we drop our SparseArray
            // entries so subsequent JNI calls (destroyWebView, loadURL, ...)
            // become safe no-ops while the C++ side decides whether to
            // auto-recover (creates a NEW WebView with a NEW InstanceId) or
            // hand off to the BP/C++ OnProcessFailed handler.
            String desc = (detail != null && detail.didCrash())
                    ? "renderer crashed"
                    : "renderer killed by system (OOM?)";
            Log.warn("onRenderProcessGone(" + id + "): " + desc + " — "
                    + "detaching and destroying dead WebView");

            try {
                ViewGroup parent = (ViewGroup) view.getParent();
                if (parent != null) parent.removeView(view);
                view.destroy();
            } catch (Exception e) {
                // Best-effort cleanup; an exception here is logged but not
                // propagated — we still need to fire the native callback so
                // the C++ side knows the renderer is gone.
                Log.warn("onRenderProcessGone(" + id + "): cleanup threw "
                        + e.getClass().getSimpleName() + ": " + e.getMessage());
            }
            sWebViews.remove(id);
            sConfigs.remove(id);

            // Fire the C++ callback AFTER the Java cleanup so by the time the
            // game-thread handler runs, the dead WebView is fully gone from
            // our state — the recreate path (if enabled) can allocate a fresh
            // InstanceId without colliding with stale entries.
            nativeOnProcessFailed(id, desc);

            // Returning true means "we handled it — don't bubble up and
            // crash the host app." Critical: returning false here makes
            // Android terminate the whole process.
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

                // Renderer priority — pinning the renderer at IMPORTANT (the
                // framework default) is appropriate for a primary-content
                // browser, but we're a game-UI overlay. Drop to BOUND so the
                // renderer runs at the host Activity's priority, and pass
                // waivedWhenNotVisible=true so Android can fully reclaim
                // renderer memory the moment our overlay is hidden (chat
                // panel dismissed, menu closed, app backgrounded). Re-bound
                // automatically next time the WebView is shown.
                //
                // No-op on API < 26; our minSdk is 28 so always applies.
                wv.setRendererPriorityPolicy(WebView.RENDERER_PRIORITY_BOUND, true);

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
                // it behind the nice window.InoWebUI.send() API. The
                // interface is bound to the WebView before the first load,
                // so it is present in the JS context by the time any
                // document-start script runs.
                wv.addJavascriptInterface(new InoWebBridge(id), "_InoWebUIHost");

                Config cfg = getOrCreateConfig(id);
                cfg.messagingEnabled = true;

                // Preferred path: register bridge.js as a document-start
                // script. WebViewCompat runs it BEFORE any page script on
                // every navigation (and re-runs on each navigation), which
                // is exactly the guarantee WebView2 and WKWebView give. This
                // is what makes window.InoWebUI reliably present when the
                // page's own scripts execute — closing the onPageStarted
                // race. allowedOriginRules = {"*"}: the bridge is needed on
                // whatever origin the (vhost-locked) content is served from.
                if (WebViewFeature.isFeatureSupported(WebViewFeature.DOCUMENT_START_SCRIPT))
                {
                    try {
                        Set<String> allOrigins = Collections.singleton("*");
                        WebViewCompat.addDocumentStartJavaScript(
                                wv, InoWebUIScripts.BRIDGE_JS, allOrigins);
                        cfg.docStartBridgeInstalled = true;
                        Log.debug("setupMessaging(" + id + "): bridge via "
                                + "addDocumentStartJavaScript (pre-page guarantee)");
                    } catch (Exception e) {
                        // Defensive: if registration throws for any reason,
                        // leave docStartBridgeInstalled false so onPageStarted
                        // still injects (degraded but functional).
                        Log.warn("setupMessaging(" + id + "): addDocumentStartJavaScript "
                                + "failed (" + e.getMessage() + ") — falling back to "
                                + "onPageStarted injection");
                    }
                }
                else
                {
                    Log.warn("setupMessaging(" + id + "): DOCUMENT_START_SCRIPT not "
                            + "supported by this System WebView — using onPageStarted "
                            + "fallback (bridge may load after early page scripts)");
                }
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
                Config c = getOrCreateConfig(id);
                c.devOverlayEnabled = enabled;

                if (enabled) {
                    // Same document-start path as the bridge. dev_overlay.js
                    // self-defers to DOMContentLoaded and guards window.InoWebUI,
                    // so registering it at document-start is safe and matches
                    // the Windows/iOS injection model. Guard with the flag so
                    // a repeated setDevToolsEnabled(true) can't stack multiple
                    // overlay copies. Registration order (bridge first, in
                    // setupMessaging; overlay here) is preserved by WebViewCompat.
                    WebView wv = sWebViews.get(id);
                    if (wv != null && !c.docStartOverlayInstalled
                            && WebViewFeature.isFeatureSupported(WebViewFeature.DOCUMENT_START_SCRIPT))
                    {
                        try {
                            WebViewCompat.addDocumentStartJavaScript(
                                    wv, InoWebUIScripts.DEVTOOLS_OVERLAY_JS,
                                    Collections.singleton("*"));
                            c.docStartOverlayInstalled = true;
                        } catch (Exception e) {
                            Log.warn("setDevToolsEnabled(" + id + "): overlay "
                                    + "addDocumentStartJavaScript failed ("
                                    + e.getMessage() + ") — onPageStarted fallback");
                        }
                    }
                    Log.debug("DevTools enabled — dev overlay "
                            + (c.docStartOverlayInstalled
                                ? "registered at document-start"
                                : "will inject on next page load")
                            + "; chrome://inspect available for remote Chrome DevTools");
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

    /**
     * Suppress the browser's built-in long-press menu (link "open in new
     * tab", image "save image", arbitrary page-text selection) for
     * locked-down game UI — WITHOUT killing long-press inside editable
     * fields. An &lt;input&gt;/&lt;textarea&gt; still gets its Paste /
     * Select-All action toolbar (so the user can paste a password, etc.);
     * we only consume the long-press when the hit target is NOT editable.
     *
     * Implementation note: setLongClickable(true) must stay set even in
     * the "disabled" branch. If it were false, Android never dispatches
     * the long-press to our listener, so the editable-field action mode
     * would never start either (this was the old bug — a blanket
     * setLongClickable(false) + always-consume listener killed paste in
     * password / text inputs). We discriminate per-gesture via
     * WebView.getHitTestResult(): EDIT_TEXT_TYPE means the press landed
     * on an editable element, so we let WebView handle it normally.
     */
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
                    // Keep long-press dispatching ON so editable fields
                    // still get their selection/paste toolbar; only consume
                    // (suppress the browser menu) for non-editable targets.
                    wv.setLongClickable(true);
                    wv.setOnLongClickListener(new android.view.View.OnLongClickListener() {
                        @Override public boolean onLongClick(android.view.View v) {
                            android.webkit.WebView.HitTestResult r =
                                    ((WebView) v).getHitTestResult();
                            int t = (r != null)
                                    ? r.getType()
                                    : android.webkit.WebView.HitTestResult.UNKNOWN_TYPE;
                            if (t == android.webkit.WebView.HitTestResult.EDIT_TEXT_TYPE) {
                                return false; // editable → allow paste/select toolbar
                            }
                            return true;      // else consume — no browser menu
                        }
                    });
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
     * nativeOnCapturePreviewComplete with the success bool when done.
     *
     * Why PixelCopy instead of WebView.draw(Canvas)?
     *
     *   WebView has been hardware-accelerated by default since API 19. Calling
     *   `wv.draw(softwareCanvas)` triggers Chromium's deprecated software-draw
     *   path, which silently does NOT capture the actual rendered content on
     *   modern devices — you get a blank or partial PNG instead of a
     *   screenshot. The software-draw path is also documented as not
     *   supporting <video>, WebGL, or accelerated CSS effects:
     *     https://chromium.googlesource.com/chromium/src/+/HEAD/android_webview/docs/software_draw_deprecated.md
     *
     *   PixelCopy.request(Window, Rect, Bitmap, ...) reads pixels from the
     *   final SurfaceFlinger output of the host window, so the result
     *   matches what the user actually sees — HW-accelerated WebView
     *   content, video frames, and everything else.
     *
     *   Available since API 24 with the Rect overload requiring API 26;
     *   our minSdk is 28 so both are always available.
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
                final WebView wv = sWebViews.get(id);
                if (wv == null) {
                    nativeOnCapturePreviewComplete(id, false, outFilePath);
                    return;
                }
                final int w = wv.getWidth();
                final int h = wv.getHeight();
                if (w <= 0 || h <= 0) {
                    Log.warn("capturePreview(" + id + "): WebView has zero size; "
                            + "did you capture before the first layout?");
                    nativeOnCapturePreviewComplete(id, false, outFilePath);
                    return;
                }

                // PixelCopy sources from the host Window's surface; we need
                // the WebView's bounds relative to that window so we crop to
                // just our overlay rather than capturing the whole screen.
                int[] loc = new int[2];
                wv.getLocationInWindow(loc);
                final Rect srcRect = new Rect(loc[0], loc[1], loc[0] + w, loc[1] + h);

                // OOM-guard the bitmap allocation: a 1440×3120 ARGB_8888 is
                // ~18 MB; a tablet at 2560×1600 is ~16 MB; both are fine, but
                // pathological resolutions can fail. Surface the failure
                // through the callback instead of letting it crash the app.
                final Bitmap bmp;
                try {
                    bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
                } catch (OutOfMemoryError oom) {
                    Log.error("capturePreview(" + id + "): OutOfMemoryError "
                            + "allocating " + w + "x" + h + " bitmap (~"
                            + (w * h * 4L / (1024 * 1024)) + " MB)");
                    nativeOnCapturePreviewComplete(id, false, outFilePath);
                    return;
                }

                // PixelCopy delivers its callback onto the supplied Handler.
                // Use a dedicated background thread so the file I/O (and the
                // JPEG/PNG compress) doesn't block the main thread on big
                // bitmaps. The thread quits after one capture; we don't reuse
                // it because the API is one-shot per request.
                final HandlerThread copyThread = new HandlerThread("InoWebUI-Capture");
                copyThread.start();
                final Handler copyHandler = new Handler(copyThread.getLooper());

                try {
                    PixelCopy.request(
                        activity.getWindow(), srcRect, bmp,
                        new PixelCopy.OnPixelCopyFinishedListener() {
                            @Override public void onPixelCopyFinished(int copyResult) {
                                boolean ok = false;
                                try {
                                    if (copyResult != PixelCopy.SUCCESS) {
                                        Log.warn("capturePreview(" + id
                                                + "): PixelCopy failed result=" + copyResult);
                                    } else {
                                        Bitmap.CompressFormat cf = (format == 1)
                                                ? Bitmap.CompressFormat.JPEG
                                                : Bitmap.CompressFormat.PNG;
                                        FileOutputStream fos = new FileOutputStream(outFilePath);
                                        try {
                                            ok = bmp.compress(cf, 90, fos);
                                            if (!ok) {
                                                Log.warn("capturePreview(" + id
                                                        + "): Bitmap.compress returned false "
                                                        + "(path=" + outFilePath + ")");
                                            }
                                        } finally {
                                            try { fos.close(); } catch (Exception ignore) {}
                                        }
                                    }
                                } catch (Exception e) {
                                    // Most commonly hits scoped-storage paths on
                                    // API 29+: writing to /sdcard outside the
                                    // app-private dirs fails with FileNotFound.
                                    Log.error("capturePreview(" + id + ") write failed: "
                                            + e.getMessage() + " (path=" + outFilePath + "). "
                                            + "If the path is outside the app's private "
                                            + "directory, scoped storage may be blocking it.");
                                } finally {
                                    bmp.recycle();
                                    copyThread.quitSafely();
                                    nativeOnCapturePreviewComplete(id, ok, outFilePath);
                                }
                            }
                        },
                        copyHandler);
                } catch (IllegalArgumentException iae) {
                    // PixelCopy throws synchronously if the source surface
                    // isn't available yet, or the rect is invalid.
                    Log.error("capturePreview(" + id + "): PixelCopy.request threw "
                            + iae.getClass().getSimpleName() + ": " + iae.getMessage());
                    bmp.recycle();
                    copyThread.quitSafely();
                    nativeOnCapturePreviewComplete(id, false, outFilePath);
                }
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
