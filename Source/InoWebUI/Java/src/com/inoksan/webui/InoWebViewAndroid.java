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
import android.graphics.Color;
import android.util.SparseArray;
import android.view.View;
import android.view.ViewGroup;
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

    /** id → WebView. SparseArray: primitive int keys, no autoboxing overhead. */
    private static final SparseArray<WebView> sWebViews = new SparseArray<>();

    private static Activity getActivity()
    {
        Activity a = GameActivity.Get();
        if (a == null)
        {
            Log.warn("GameActivity not available (called too early?)");
        }
        return a;
    }

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
                if (transparent) {
                    wv.setBackgroundColor(Color.TRANSPARENT);
                }

                wv.getSettings().setJavaScriptEnabled(true);
                wv.getSettings().setDomStorageEnabled(true);

                // Keep navigation in this WebView rather than punting to the
                // system browser when a link is clicked.
                wv.setWebViewClient(new WebViewClient());

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
                Log.debug("createWebView(" + id + ")");

                // Navigation is a separate step (loadURL) so C++ can apply
                // virtual-host / messaging / lockdown configuration between
                // construction and the first page load.
            }
        });
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Virtual host mapping
    //
    //  Installs a WebViewClient whose shouldInterceptRequest handler serves
    //  any request to https://<host>/* from the given local folder. This is
    //  Android's equivalent of WebView2's SetVirtualHostNameToFolderMapping.
    //
    //  Must be called AFTER createWebView and BEFORE loadURL — the URL
    //  being navigated to must resolve through the handler.
    // ─────────────────────────────────────────────────────────────────────
    public static void setVirtualHost(final int id, final String host, final String folder)
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        if (host == null || host.isEmpty() || folder == null || folder.isEmpty()) return;

        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                WebView wv = sWebViews.get(id);
                if (wv == null) {
                    Log.warn("setVirtualHost(" + id + "): no WebView");
                    return;
                }

                final File rootFolder = new File(folder);
                if (!rootFolder.isDirectory()) {
                    Log.warn("setVirtualHost(" + id + "): folder does not exist: " + folder +
                             " (requests to " + host + " will 404)");
                }

                final String prefix = "https://" + host + "/";

                wv.setWebViewClient(new WebViewClient() {
                    @Override
                    public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                        final String url = request.getUrl().toString();
                        if (!url.startsWith(prefix)) {
                            return null; // let WebView handle normally
                        }

                        // Strip the prefix and any query/fragment to get a pure file path.
                        String path = url.substring(prefix.length());
                        int q = path.indexOf('?');     if (q >= 0) path = path.substring(0, q);
                        int h = path.indexOf('#');     if (h >= 0) path = path.substring(0, h);

                        // Reject path-traversal attempts up front.
                        if (path.contains("..")) {
                            return notFound(); // 404 even for traversal — don't leak "forbidden"
                        }
                        if (path.isEmpty()) path = "index.html";

                        File file = new File(rootFolder, path);
                        if (!file.isFile()) return notFound();

                        try {
                            FileInputStream fis = new FileInputStream(file);
                            return new WebResourceResponse(guessMimeType(file.getName()), null, fis);
                        } catch (Exception e) {
                            Log.error("shouldInterceptRequest: " + e.getMessage());
                            return notFound();
                        }
                    }
                });

                Log.debug("setVirtualHost(" + id + "): https://" + host + "/ -> " + folder);
            }
        });
    }

    /** 404 response — empty body, no data stream (WebResourceResponse allows null input). */
    private static WebResourceResponse notFound()
    {
        return new WebResourceResponse("text/plain", "UTF-8", 404, "Not Found",
                Collections.<String, String>emptyMap(), null);
    }

    /** Best-effort MIME type from extension. Falls back to common web types
     *  that Android's MimeTypeMap sometimes misses (mjs, woff2), then to
     *  application/octet-stream (browser will content-sniff). */
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
                Log.debug("destroyWebView(" + id + ")");
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
    //
    //  MVP convention: width/height are the WebView's pixel size; x/y are
    //  the WebView's margins from the root FrameLayout's top-left. The
    //  subsystem currently sends SWindow-client coordinates; on Android
    //  that happens to land correctly when the activity fills the screen.
    //  Non-positive width/height falls back to MATCH_PARENT.
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
    //  Activity lifecycle forwarding
    //  (called from GameActivity.java via UPL-injected code)
    // ─────────────────────────────────────────────────────────────────────

    public static void onActivityPause()
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                for (int i = 0; i < sWebViews.size(); i++) {
                    sWebViews.valueAt(i).onPause();
                }
            }
        });
    }

    public static void onActivityResume()
    {
        final Activity activity = getActivity();
        if (activity == null) return;
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                for (int i = 0; i < sWebViews.size(); i++) {
                    sWebViews.valueAt(i).onResume();
                }
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
            }
        });
    }
}
