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

A single `InoWebViewClient` subclass handles:

- `shouldInterceptRequest` — serves `https://<host>/*` from the
  mapped local folder (virtual-host mapping).
- `shouldOverrideUrlLoading` — navigation lockdown; returns `true` for
  non-whitelisted URIs.
- `onPageStarted` — *fallback* bridge / dev-overlay injection, used
  only on pre-2020 System WebView (see "Bridge injection timing" below).
- `onPageFinished` / `onReceivedError` — drives
  `OnNavigationCompleted`.
- `onRenderProcessGone` — drives `OnProcessFailed` (API 26+).

A single `InoWebChromeClient` handles:

- `onReceivedTitle` — drives `OnDocumentTitleChanged`.
- `onJsAlert` / `onJsConfirm` / `onJsPrompt` / `onJsBeforeUnload` —
  dialog suppression.
- `onCreateWindow` — `window.open` / `_blank` handling.

Both clients are installed once per WebView inside `createWebView`. They
read per-WebView state from a `sConfigs: SparseArray<Config>` that the
various `configureXxx` JNI methods populate between `createWebView`
and the first `loadURL`.

## JS bridge

The JS side is identical across platforms:
`window.InoWebUI.send`, `.on`, `.off`.

`window.chrome.webview.postMessage` does not exist on Android, so the
bridge on Android routes through `addJavascriptInterface` instead. User
JavaScript does not have to care — the injected shim abstracts it.

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
The pinned `1.8.0` artifact builds against compileSdk 34 (UE 5.7's
default); if a project overrides `compileSdk` lower, lower the webkit
version in the UPL to a matching one (the API itself exists since
webkit 1.4.0).

## Java -> C++ events

Six `nativeOn*` JNI exports carry Java events back to C++. They all
route through a single `DispatchOnGameThread<Lambda>` helper that:

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
| Two-way messaging | yes | `addJavascriptInterface` + `evaluateJavascript` |
| Navigation events | yes | `OnNavigationStarting` / `OnNavigationCompleted` / `OnDocumentTitleChanged` |
| Lockdown | yes | `shouldOverrideUrlLoading` + wildcard rules |
| JS dialog suppression | yes | `WebChromeClient.onJs*` |
| `window.open` blocking | yes | `onCreateWindow` with transport-WebView trick to capture the URL |
| Focus events + `FocusWebView` | yes | `setOnFocusChangeListener` + `requestFocus` |
| `SetZoomFactor` | yes | `setInitialScale(percent)` |
| `ClearAllCookies` | yes | `CookieManager.removeAllCookies` |
| `OnProcessFailed` | yes | `WebViewClient.onRenderProcessGone` (API 26+) |
| DevTools (remote) | yes | `setWebContentsDebuggingEnabled` — see below |
| `ExecuteJavaScript` | yes | `webView.evaluateJavascript` |
| `UserAgentOverride` | yes | `WebSettings.setUserAgentString` |
| `bEnableContextMenus` | yes | `setOnLongClickListener` + `getHitTestResult` — suppresses the browser long-press menu but keeps editable-field Paste/Select (see note) |
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

## Remote DevTools

Android's `WebView` exposes Chromium DevTools **remotely** — no extra
plumbing required. With the device connected via adb:

1. Open `chrome://inspect/#devices` in desktop Chrome.
2. Select your app's WebView from the device list.
3. Click **inspect**.

This gives you the full Chromium DevTools panel against the live
WebView running on the phone. No build flags beyond
`setWebContentsDebuggingEnabled(true)`, which the plugin calls when
`bEnableDevTools == true`.

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
