# Android

InoWebUI on Android is full feature parity with Windows, with a couple
of platform quirks called out below.

## How it works

```
UE game thread (C++)
     |
     |   JNI static-void calls (one per operation)
     v
Android UI thread (Java)                       via Activity.runOnUiThread
     v
android.webkit.WebView
     - sibling of UE's SurfaceView inside GameActivity's content FrameLayout
     - transparent background optional -> UE scene shows through
```

The C++ methods assert `IsInGameThread()`, then call Java static
methods fire-and-forget. Java marshals onto the UI thread via
`Activity.runOnUiThread`. Because all dispatches share one run loop,
ordering is preserved end-to-end.

## Files

```
Plugins/InoWebUI/Source/InoWebUI/
|-- InoWebUI_UPL.xml                     Unreal Plugin Language config:
|                                          - copies Java into the APK
|                                          - adds INTERNET permission
|                                          - hooks GameActivity lifecycle
|                                            (onPause / onResume / onDestroy)
|                                          - ProGuard -keep rule for R8
|-- Java/src/net/inoland/webui/
|   `-- InoWebViewAndroid.java           UI-thread-only helper, owns
|                                          SparseArray<WebView> keyed by
|                                          primitive int IDs
`-- Private/Impl/Android/
    |-- InoWebViewImpl_Android.h
    `-- InoWebViewImpl_Android.cpp       Cached jmethodIDs via
                                           FAndroidApplication::FindJavaClass
```

## Runtime architecture

Three client classes, installed per WebView. A single `InoWebViewClient`
subclass handles:

- `shouldInterceptRequest` — serves `https://<host>/*` from the
  mapped local folder (virtual-host mapping).
- `shouldOverrideUrlLoading` — navigation lockdown; returns `true` for
  non-whitelisted URIs.
- `onPageStarted` — *fallback* bridge / dev-overlay injection, used
  only on pre-2020 System WebView (see "Bridge injection timing" below).
- `onPageFinished` / `onReceivedError` — drives
  `OnNavigationCompleted` (and `nativeOnNavStateChanged` for the
  back/forward flags).
- `onRenderProcessGone` — does the doc-mandated detach + `destroy()` +
  drop from `sWebViews` / `sConfigs`, then drives `OnProcessFailed`
  (API 26+) → C++ auto-recover.

A single `InoWebChromeClient` handles:

- `onReceivedTitle` — drives `OnDocumentTitleChanged`.
- `onConsoleMessage` — drives `OnConsoleMessage` (see the feature
  matrix below).
- `onJsAlert` / `onJsConfirm` / `onJsPrompt` / `onJsBeforeUnload` —
  dialog suppression.
- `onCreateWindow` — `window.open` / `_blank` handling.

A single `InoRenderProcessClient` (AndroidX `WebViewRenderProcessClient`,
installed via `WebViewCompat.setWebViewRenderProcessClient`) handles
hung-renderer detection:

- `onRenderProcessUnresponsive` — drives `OnRenderProcessUnresponsive`
  and, if `UnresponsiveTimeoutMs > 0`, schedules an auto-terminate.
- `onRenderProcessResponsive` — drives `OnRenderProcessResponsive` and
  cancels any pending auto-terminate.

All three clients are installed once per WebView inside `createWebView`
(the render-process client via `configureUnresponsiveTimeout`). They
read per-WebView state from a `sConfigs: SparseArray<Config>` that the
various `configureXxx` JNI methods populate between `createWebView`
and the first `loadURL`.

## JS bridge

The JS side is identical across platforms:
`window.InoWebUI.send`, `.on`, `.off`, `.once`.

`window.chrome.webview.postMessage` does not exist on Android, so the
native bridge is installed by feature-detection in `setupMessaging`,
in order of preference:

1. **`WebViewCompat.addWebMessageListener`** (modern, ~Chrome 82+) — the
   default path on effectively every device since 2020. Origin-
   allowlisted (`https://<vhost>` when a virtual host is set, `*`
   otherwise), so locked-down third-party iframes can't see the bridge;
   the callback runs on the UI thread. Injects `window._InoWebUIHost`
   with `.postMessage` / `.onmessage`.
2. **`addJavascriptInterface`** (legacy) — installed only when
   `WebViewFeature.WEB_MESSAGE_LISTENER` is unsupported (pre-2020 System
   WebView). No origin check; callback runs on the JavaBridge background
   thread. Injects `window._InoWebUIHost.receive`.

For the UE → JS direction, `postMessageJson` prefers
`JavaScriptReplyProxy.postMessage` (captured from the page's most recent
post — no JS-engine round-trip, no string-interpolation risk) and falls
back to `evaluateJavascript("window._InoWebUIDispatch(...)")` pre-
handshake or on the legacy path.

User JavaScript does not have to care — `bridge.js` feature-detects the
shape of `window._InoWebUIHost` and mounts the matching transport, so the
page-side API is identical regardless of which native path was chosen.

### Bridge injection timing

`bridge.js` (and the dev overlay) are registered via AndroidX
`WebViewCompat.addDocumentStartJavaScript` whenever
`WebViewFeature.DOCUMENT_START_SCRIPT` is supported — ≈Chrome-WebView 83+,
effectively every device since 2020. That API runs the script **before
any page script, on every navigation**, the same hard guarantee
WebView2 (`AddScriptToExecuteOnDocumentCreated`) and WKWebView
(`WKUserScriptInjectionTimeAtDocumentStart`) give. So a consumer page can
do `if (window.InoWebUI) …` at the top of its own first script and it is
reliably there.

The legacy `onPageStarted` + `evaluateJavascript` path had **no** ordering
guarantee against the page's inline scripts — a page that checked
`window.InoWebUI` once at load could see it undefined and conclude
"bridge not connected" (this is why a page that works on Windows could
fail on Android). It is now the **fallback only**, for System WebView too
old to support `DOCUMENT_START_SCRIPT`.

Requires `androidx.webkit:webkit` (added by `InoWebUI_UPL.xml` via
`buildGradleAdditions`). AndroidX must be enabled — the UE 5.x default.
The pinned `1.16.0` artifact (current stable) requires min SDK 24 — well
below the project's min SDK 28 — and builds against compileSdk 34+ (UE
5.7's default is 34). If a project overrides `compileSdk` lower, lower the
webkit version in the UPL to a matching one (the
`addDocumentStartJavaScript` API itself exists since webkit 1.4.0).

## Java -> C++ events

Fourteen `nativeOn*` JNI exports carry Java events back to C++:

```
nativeOnMessageReceived          nativeOnGotFocus
nativeOnNavigationStarting       nativeOnLostFocus
nativeOnNavigationCompleted      nativeOnProcessFailed
nativeOnDocumentTitleChanged     nativeOnRenderProcessUnresponsive
nativeOnScriptDialog             nativeOnRenderProcessResponsive
nativeOnNewWindowRequested       nativeOnConsoleMessage
nativeOnNavStateChanged          nativeOnCapturePreviewComplete
```

They all route through a single `DispatchOnGameThread<Lambda>` helper
that:

1. Marshals onto the game thread via `AsyncTask`.
2. Re-acquires the impl registry lock.
3. Invokes the lambda with the live impl pointer.

The lock is held through the callback so `Shutdown` — which removes
the impl from the registry — serialises correctly.

## Feature matrix

| API | Status | Notes |
|---|:-:|---|
| Create / Destroy | yes | |
| Navigate / Reload / Show / Hide | yes | |
| SyncBounds (margins + size) | yes | but see the MATCH_PARENT note below |
| Transparent background | yes | `bTransparentBackground` |
| Activity lifecycle hooks | yes | Pause / Resume / Destroy via UPL |
| Virtual-host mapping | yes | `WebViewClient.shouldInterceptRequest` serves `https://<host>/*` from a local folder |
| Web Bundle assets | yes | cross-platform runtime logic |
| Two-way messaging | yes | `addWebMessageListener` (modern, origin-allowlisted) → `addJavascriptInterface` (legacy fallback); UE→JS via `JavaScriptReplyProxy.postMessage` → `evaluateJavascript` |
| Navigation events | yes | `OnNavigationStarting` / `OnNavigationCompleted` / `OnDocumentTitleChanged` |
| Lockdown | yes | `shouldOverrideUrlLoading` + wildcard rules |
| JS dialog suppression | yes | `WebChromeClient.onJs*` |
| `window.open` blocking | yes | `onCreateWindow` with transport-WebView trick to capture the URL |
| Focus events + `FocusWebView` | yes | `setOnFocusChangeListener` + `requestFocus` |
| `SetZoomFactor` | yes | `setInitialScale(percent)` |
| `ClearAllCookies` | yes | `CookieManager.removeAllCookies` |
| `OnProcessFailed` | yes | `WebViewClient.onRenderProcessGone` (API 26+) |
| **Auto-recover on process failure** | yes | After `onRenderProcessGone` Java detaches + `destroy()`s the dead view; C++ recreates a fresh impl, reloads the snapshotted URL. Budget: 3 attempts / 60 s (handled in the UObject layer). |
| **Hung-renderer detection + auto-terminate** | yes | `InoRenderProcessClient` (`WebViewRenderProcessClient`). `OnRenderProcessUnresponsive` / `OnRenderProcessResponsive` delegates. `UnresponsiveTimeoutMs > 0` (default 10000 ms) schedules `WebViewRenderProcess.terminate()` → `onRenderProcessGone` → auto-recover. |
| **Console message capture** | yes | `WebChromeClient.onConsoleMessage` → `OnConsoleMessage(Level, Message, SourceID, Line)` delegate, also mirrored to `LogInoWebUI` at matching verbosity |
| **Explicit security defaults** | yes | `configureSecurity` → `setMixedContentMode(NEVER_ALLOW)` + `setAllowFileAccess(false)` + `setAllowContentAccess(false)`. Config: `bAllowMixedContent` / `bAllowFileURLs` / `bAllowContentURIs`, all default false. |
| **Renderer priority** | yes | `setRendererPriorityPolicy(RENDERER_PRIORITY_BOUND, true)` — Android may reclaim renderer memory while the overlay is hidden |
| **Cleartext-URL warning** | yes | `Initialize` / `Navigate` / `LoadURLWithHeaders` log a loud warning on `http://` (Android 28+ blocks cleartext silently) |
| `CapturePreview` | yes | `PixelCopy.request(Window, Rect, Bitmap, …)` reads the real SurfaceFlinger output (HW-accelerated WebView, video, WebGL); async → `nativeOnCapturePreviewComplete` |
| `SetCookie` / `ClearAllData` | yes | `CookieManager.setCookie` / wipe cookies + cache + history + form data + Web Storage |
| `LoadHTMLString` / `LoadURLWithHeaders` | yes | `loadDataWithBaseURL` / `loadUrl(url, headers)` |
| `GoBack` / `GoForward` / `StopLoading` | yes | `WebView.goBack` / `goForward` / `stopLoading`; back/forward enablement cached via `nativeOnNavStateChanged` |
| `DevTools` (remote) + floating overlay | yes | `bEnableDevTools` enables `setWebContentsDebuggingEnabled` (remote `chrome://inspect`) **and** injects the floating dev-tools overlay — see below |
| `ExecuteJavaScript` | yes | `webView.evaluateJavascript` |
| `UserAgentOverride` | yes | `WebSettings.setUserAgentString` |
| `bEnableContextMenus` | yes | `setOnLongClickListener` + `getHitTestResult` — suppresses the browser long-press menu but keeps editable-field Paste/Select (see note) |
| **`bForceHardwareLayer`** | yes | `configureLayerType` → `View.setLayerType(LAYER_TYPE_HARDWARE)`. Default false. Workaround for transparent-WebView scroll-compositing corruption on Pixel 10 / Tensor G5 / PowerVR drivers — promotes the WebView to a single offscreen hardware texture so each scroll frame is a full-frame blend. |
| `OpenDevTools` (programmatic) | no | Android has no in-process API; remote inspect only |
| `SetMuted` / `bStartMuted` | no | Android `WebView` has no audio-mute API; a warning is logged |
| `bEnableAcceleratorKeys` | N/A | F5 / F12 / Ctrl+F are desktop-only concepts |

> **Context menus & paste.** On Android, the Paste / Select-All toolbar is
> raised by a *long-press*. A blanket `setLongClickable(false)` (the old
> behavior) therefore also broke pasting into `<input>` / `<textarea>` —
> e.g. a user could not paste a password. With `bEnableContextMenus =
> false` the long-press listener now inspects
> `WebView.getHitTestResult()`: `EDIT_TEXT_TYPE` (editable element) lets
> the gesture through so the Paste/Select toolbar appears; every other
> target is consumed, so the browser long-press menu (open-link-in-tab,
> save-image, page-text selection) stays suppressed for game UI. No need
> to set `bEnableContextMenus = true` just to allow paste.

## DevTools

`bEnableDevTools == true` does two things on Android.

### Remote DevTools

Android's `WebView` exposes Chromium DevTools **remotely** — no extra
plumbing required. With the device connected via adb:

1. Open `chrome://inspect/#devices` in desktop Chrome.
2. Select your app's WebView from the device list.
3. Click **inspect**.

This gives you the full Chromium DevTools panel against the live
WebView running on the phone. No build flags beyond
`setWebContentsDebuggingEnabled(true)`, which the plugin calls when
`bEnableDevTools == true`.

### Floating dev-tools overlay

The same flag also injects the floating dev-tools overlay
(`dev_overlay.js`, registered at document-start alongside `bridge.js`).
A gear FAB in the corner expands a vertical toolbar of **four** buttons —
**Refresh**, **DevTools**, **Info**, **Dev Callback** — plus an always-
visible **FPS** chip that samples the WebView's own frame rate. (The
Refresh / DevTools / Dev Callback buttons route over the bridge to
`UInoWebView::Reload` / `OpenDevTools` / the `OnDevCallback` delegate;
Info is a JS-only modal that never hops to UE.)

## Logcat filter

```
adb logcat | findstr /I "InoWebUI"
```

Catches both the native `LogInoWebUI` category and the Java-side
`Logger` output (both tagged `InoWebUI`).

## If the Java helper is not found

Error: `"InoWebViewAndroid Java class not found"`. Check, in order:

1. **UPL registered** — build log should contain
   `"InoWebUI: UPL init (Android)"`.
2. **Java file copied** — inspect
   `Intermediate/Android/APK/src/net/inoland/webui/InoWebViewAndroid.java`.
3. **ProGuard / R8** — our `-keep` rule in the UPL should stop
   stripping; check the mapping output if suspicious.

## "Bridge not connected" on Android (but fine on Windows)

Symptom: a page that reports the bridge connected on Windows shows
"not connected" / `window.InoWebUI` undefined on Android.

Cause: the page checked `window.InoWebUI` **once**, synchronously, and
the bridge was injected too late. Pre-fix, Android injected the bridge
in `onPageStarted` via `evaluateJavascript`, which has no ordering
guarantee against the page's own inline scripts. Windows never had this
because WebView2 injects at document-start.

Fix (already in the plugin): the bridge is registered via
`WebViewCompat.addDocumentStartJavaScript`, matching the Windows/iOS
document-start guarantee. If you still see it on a device:

1. Filter logcat: `adb logcat | findstr /I "InoWebUI"`.
2. Look for `setupMessaging(<id>): bridge via addDocumentStartJavaScript`
   — that confirms the pre-page path is active.
3. If you instead see `DOCUMENT_START_SCRIPT not supported` the device's
   System WebView is ancient; update *Android System WebView* (or
   Chrome) from the Play Store. The `onPageStarted` fallback still runs
   but is best-effort.
4. If you see `addDocumentStartJavaScript failed (…)`, the
   `androidx.webkit` dependency likely didn't resolve — verify the UPL
   `buildGradleAdditions` block is present and AndroidX is enabled
   (UE 5.x default) and the webkit version matches the project's
   `compileSdk`.

## Android-specific quirk: MATCH_PARENT sizing (auto mode)

UE's `SWindow::GetClientRectInScreen` reports in a coordinate system
that does not match `FrameLayout.LayoutParams`' physical-pixel
contract. When you pass the incoming rect straight through on Android,
the WebView ends up covering roughly one-third of the screen.

In **auto-bounds mode** (the default) `InoWebViewAndroid.syncBounds`
therefore **ignores** the incoming values and forces `MATCH_PARENT`
for both width and height — correct for the common "full-screen
overlay" case.

In **manual-bounds mode** (after `UInoWebView::SetBounds`), the Java
side scales the UE-pixel coords by the activity's display density
(`getResources().getDisplayMetrics().density`) to get correct
physical-pixel layout params + margins. Switch back to auto with
`UInoWebView::SetBoundsAuto()`.
