# CLAUDE.md — InoWebUI plugin

This file provides guidance to Claude Code (claude.ai/code) when working
inside the **InoWebUI** plugin.

---

## What this plugin is

A native WebView **overlay** for Unreal Engine 5.7. The WebView is a real OS
window stacked above the Unreal game window — not a render-to-texture, not
a UMG widget. The OS compositor (DWM on Windows) blends them. UE renders
the 3D scene; the WebView renders HTML/CSS/JS (React, etc.); transparent
pixels in the HTML reveal the 3D scene underneath.

This is fundamentally different from UE's built-in `WebBrowser` plugin, which
textures the browser output — we skip all of that, zero copy, zero stall.

**Current status: Phase 20 — Win64 (full) + Android (full parity, hardened) + iOS (`WKWebView`, full parity bar `SetMuted` and `bEnableContextMenus`) + Web Bundle assets + dev-tools overlay + DirectComposition hosting for PIE + unified `bridge.js` / `dev_overlay.js` source + browser-style API surface (back/forward, state getters, capture, headers, sub-region bounds) + Android renderer auto-recover / unresponsive detection + `addWebMessageListener` bridge + page console capture + explicit security defaults + iOS content-world-isolated bridge (`WKContentWorld.defaultClientWorld`) + iOS 14+ native `pageZoom` + iOS 16.4+ Safari Web Inspector attach via `inspectable` + iOS preferences-variant `decidePolicyForNavigationAction:preferences:` with per-nav `allowsContentJavaScript` + `WKPreferences` hardening (`fraudulentWebsiteWarningEnabled`, `isElementFullscreenEnabled`, `isTextInteractionEnabled`) + `WKWebViewConfiguration.upgradeKnownHostsToHTTPS` + additive `applicationNameForUserAgent` + `WKWebView.themeColor` KVO → `OnThemeColorChanged` BP delegate + `WKWebView.underPageBackgroundColor` synced to transparency.**

---

## The one hard constraint

**The game must run in borderless-windowed mode, not exclusive fullscreen.**
Exclusive fullscreen bypasses DWM, so any overlay window becomes invisible.
Set `FullscreenMode=1` in `DefaultGameUserSettings.ini` or launch with
`-windowed`.

---

## Architecture

Two layers. Never collapse them — the separation is load-bearing.

```
UObject / Blueprint layer (platform-agnostic, no Windows.h ever)
├── UInoWebUISubsystem     UGameInstanceSubsystem   manages all WebViews
└── UInoWebView            UObject                  one overlay handle
        │
        │  holds TUniquePtr<IInoWebViewImpl>  (pimpl — hides platform code)
        │
Native implementation layer (platform-specific)
└── IInoWebViewImpl                              pure virtual interface
    ├── FInoWebViewImpl_Windows                  WebView2 COM impl (child HWND)
    ├── FInoWebViewImpl_Windows_Composition      WebView2 + DirectComposition (PIE)
    ├── FInoWebViewImpl_Android                  JNI + android.webkit.WebView
    └── FInoWebViewImpl_iOS                      WKWebView (Obj-C++ .mm)
```

**Why pimpl?** WebView2.h drags Windows.h, wrl.h, dozens of COM headers. The
pimpl pattern keeps them confined to the two Windows impl .cpp files
(`InoWebViewImpl_Windows.cpp` and `InoWebViewImpl_Windows_Composition.cpp`);
the Apple headers stay inside `InoWebViewImpl_iOS.mm` and the JNI inside the
Android impl. Nothing else in the plugin pays that compile cost, and platform
types never leak into the UObject headers.

`IInoWebViewImpl.h` itself is **public** (not private) even though it's the
"internal" interface. It has to be public because `UInoWebView` holds a
`TUniquePtr<IInoWebViewImpl>`, and UHT-generated code (`UInoWebView.gen.cpp`,
specifically the FVTableHelper constructor) needs the complete type at
compile time to emit the TUniquePtr exception-unwinding path. IInoWebViewImpl.h
contains zero platform-specific code, so publishing it costs nothing —
`Private/Impl/Windows/` is still where the actual COM code hides.

### Key design rules

1. **Game thread only.** All public methods `check(IsInGameThread())`.
   WebView2 callbacks fire back on the thread that initiated them, which is
   the game thread. Don't cross threads.
2. **Async-safe.** WebView2 construction is 2-step async. Any operation
   (`Navigate`, `SetVisible`, `SyncBounds`, `Reload`) issued before the
   controller is ready is **queued** in `FInternal::Pending*` and replayed
   from `ApplyPendingOperations()` on ready. Don't add new ops without
   extending the queue.
3. **Lifetime tokens.** Async callbacks capture `TWeakPtr<int>` to the impl's
   `LifetimeToken`. `Shutdown()` resets the token, so any still-in-flight
   callback sees an invalid weak ptr and silently no-ops. Never capture raw
   `this` in a WebView2 callback without this guard.
4. **Shutdown order is fixed.** Controller must be `Close()`'d **while the
   parent HWND still exists**. Order: reset lifetime token → `Controller->Close()`
   → release ComPtrs. Changing this order leaks COM objects.

---

---

## Android MVP (Phase 6)

First Android implementation — scoped to Phase 1 equivalent. Everything
beyond overlay + URL / show / hide / reload / bounds is a logged no-op;
filled in by later phases.

### Architecture

```
UE game thread (C++)
     │   JNI static-void calls (one per operation)
     ▼
Android UI thread (Java)                    via Activity.runOnUiThread
     ▼
android.webkit.WebView
     • sibling of UE's SurfaceView inside GameActivity's content FrameLayout
     • transparent background optional → UE scene shows through
```

### Files

```
Plugins/InoWebUI/Source/InoWebUI/
├── InoWebUI_UPL.xml                  Unreal Plugin Language config:
│                                       • copies Java into APK
│                                       • adds INTERNET permission
│                                       • hooks GameActivity lifecycle →
│                                         InoWebViewAndroid.onActivity{Pause,Resume,Destroy}
│                                       • ProGuard keep rule for R8
├── Java/src/net/inoland/webui/
│   └── InoWebViewAndroid.java        UI-thread-only helper, owns
│                                       SparseArray<WebView> keyed by
│                                       primitive int IDs
└── Private/Impl/Android/
    ├── InoWebViewImpl_Android.h
    └── InoWebViewImpl_Android.cpp    Cached jmethodIDs via
                                        FAndroidApplication::FindJavaClass
```

### Threading rule

Same as Windows: all C++ entry points asserted `IsInGameThread()`. The C++
methods call Java static methods synchronously (fire-and-forget) and Java
marshals onto the UI thread via `Activity.runOnUiThread`. Since all
dispatches share one run loop, ordering is preserved end-to-end.

### Feature matrix (full parity with Windows)

| API | Android | Notes |
|---|---|---|
| Create / Destroy | ✔ | |
| Navigate / Reload / Show / Hide | ✔ | |
| SyncBounds (margins + size) | ✔ | |
| Transparent background | ✔ | `bTransparentBackground` |
| Activity lifecycle hooks | ✔ | Pause / Resume / Destroy via UPL |
| **Virtual-host mapping** | ✔ | `WebViewClient.shouldInterceptRequest` serves `https://<host>/*` from a local folder |
| **Web Bundle assets** | ✔ | Runtime logic is cross-platform; works identically |
| **Two-way messaging** | ✔ | `addWebMessageListener` w/ origin allowlist (modern, ~Chrome 82+) → `addJavascriptInterface` (legacy fallback). UE→JS uses `JavaScriptReplyProxy.postMessage` when available, falls back to `evaluateJavascript`. Same `window.InoWebUI` API as Windows. |
| **Navigation events** | ✔ | `OnNavigationStarting` / `OnNavigationCompleted` / `OnDocumentTitleChanged` |
| **Lockdown** | ✔ | `shouldOverrideUrlLoading` returns true for non-whitelisted URIs; same wildcard rules |
| **JS dialog suppression** | ✔ | `WebChromeClient.onJs{Alert,Confirm,Prompt,BeforeUnload}` |
| **window.open blocking** | ✔ | `onCreateWindow` with transport-WebView trick to capture URL |
| **Focus events + FocusWebView** | ✔ | `setOnFocusChangeListener` + `requestFocus` |
| **SetZoomFactor** | ✔ | `setInitialScale(percent)` |
| **ClearAllCookies** | ✔ | `CookieManager.removeAllCookies` |
| **OnProcessFailed** | ✔ | `WebViewClient.onRenderProcessGone` (API 26+) — does doc-mandated detach + destroy + `sWebViews.remove(id)` before firing the C++ callback |
| **Auto-recover on process failure** | ✔ | `bAutoRecoverOnProcessFailed` (default true). After `OnProcessFailed` fires, plugin recreates a fresh native impl with the same Config, reloads to the last-known URL (snapshotted pre-shutdown). Budget: 3 attempts per 60s window. See "Renderer failure recovery" below. |
| **Hung-renderer detection + auto-terminate** | ✔ | `WebViewCompat.setWebViewRenderProcessClient` (AndroidX). Hung renderer (~5s no event-loop response) fires `OnRenderProcessUnresponsive`. If `UnresponsiveTimeoutMs > 0` (default 10000), schedules `WebViewRenderProcess.terminate()` — which triggers `onRenderProcessGone` → auto-recover. `OnRenderProcessResponsive` cancels pending terminate if renderer unstuck itself. |
| **Console message capture** | ✔ | `WebChromeClient.onConsoleMessage`. New `OnConsoleMessage(Level, Message, SourceID, Line)` BP delegate (`EInoConsoleMessageLevel`). Page logs also mirrored to `LogInoWebUI` at matching verbosity — see them in the UE console without binding the delegate. |
| **Security defaults** | ✔ | Explicit `setMixedContentMode(NEVER_ALLOW)` + `setAllowFileAccess(false)` + `setAllowContentAccess(false)` (Config: `bAllowMixedContent` / `bAllowFileURLs` / `bAllowContentURIs`, all default false). Defense-in-depth — removes reliance on API-version defaults that changed at 30. |
| **Renderer priority** | ✔ | `setRendererPriorityPolicy(RENDERER_PRIORITY_BOUND, true)` — Android may reclaim renderer memory when our overlay is hidden. |
| **Cleartext URL warning** | ✔ | `Navigate` / `Initialize` / `LoadURLWithHeaders` log a loud warning when URL starts `http://` — Android 28+ default NSC blocks cleartext silently with `ERR_CLEARTEXT_NOT_PERMITTED`. |
| **DevTools (remote)** | ✔ | `setWebContentsDebuggingEnabled` — inspect via `chrome://inspect/#devices` on desktop Chrome |
| **ExecuteJavaScript** | ✔ | `webView.evaluateJavascript` |
| **UserAgentOverride** | ✔ | `WebSettings.setUserAgentString` |
| **bEnableContextMenus** | ✔ | `setOnLongClickListener` + `getHitTestResult` — suppresses the browser long-press menu (links / images / page-text selection) but **keeps** the Paste / Select-All toolbar inside editable `<input>` / `<textarea>` so password/text paste still works |
| **CapturePreview** | ✔ | `PixelCopy.request(Window, Rect, Bitmap, ...)` — reads the actual SurfaceFlinger output, works on HW-accelerated WebView. (Earlier `WebView.draw(Canvas)` was the deprecated software path and silently missed content.) |
| **Force hardware layer** | ✔ | `bForceHardwareLayer` (default false) → `View.setLayerType(LAYER_TYPE_HARDWARE, null)` via `configureLayerType`. Targeted workaround for a GPU-driver bug (Pixel 10 / Tensor G5 / PowerVR, Android 16) where a transparent WebView corrupts on scroll. Promotes the WebView to one full-screen texture so the whole frame re-blends. One extra view-sized texture; negligible for static UI. Leave false on Adreno / Mali. |
| `OpenDevTools` (programmatic) | — | Android has no in-process API; remote inspect only (log explains) |
| `SetMuted` / `bStartMuted` | — | `android.webkit.WebView` has no audio mute; log warns |
| `bEnableAcceleratorKeys` | N/A | F5/F12/Ctrl+F are desktop-only concepts |

### Android runtime architecture

One `InoWebViewClient` (handles `shouldInterceptRequest` for virtual host,
`shouldOverrideUrlLoading` for lockdown, `onPageStarted` as the *fallback*
JS-bridge injection path, `onPageFinished` / `onReceivedError` for nav
completion, `onRenderProcessGone` for crash) plus one `InoWebChromeClient`
(handles title changes, JS dialogs, `onCreateWindow`). Both are installed
once in `createWebView` and read per-WebView state from a `sConfigs:
SparseArray<Config>` that the various `configureXxx` JNI methods
populate between `createWebView` and the first `loadURL`.

**Bridge injection timing (important).** The bridge / dev-overlay are
registered through AndroidX `WebViewCompat.addDocumentStartJavaScript`
(in `setupMessaging` / `setDevToolsEnabled`) whenever
`WebViewFeature.DOCUMENT_START_SCRIPT` is supported — ≈Chrome-WebView 83+,
i.e. effectively every device since 2020. That API runs the script
**before any page script on every navigation**, the exact guarantee
WebView2's `AddScriptToExecuteOnDocumentCreated` and WKWebView's
`WKUserScriptInjectionTimeAtDocumentStart` give, so `window.InoWebUI`
is reliably present when the consumer page's own scripts run. The old
`onPageStarted` + `evaluateJavascript` path is kept **only as a
fallback** for pre-2020 System WebView (no ordering guarantee — best
effort). `Config.docStart{Bridge,Overlay}Installed` gates `onPageStarted`
so the preferred path never double-injects. Requires the
`androidx.webkit:webkit` gradle dep (added via `InoWebUI_UPL.xml`
`buildGradleAdditions`); AndroidX must be enabled (UE 5.x default) and
the webkit `1.16.0` artifact (pinned in the UPL) builds against
compileSdk 34 (UE 5.7 default — bump/lower the version in the UPL if the
project overrides compileSdk).

JS bridge is the same `window.InoWebUI.send / on / off / once` API as
Windows. `window.chrome.webview.postMessage` doesn't exist on Android;
the injected bridge routes through `addWebMessageListener` (modern,
origin-allowlisted, ~Chrome 82+) or `addJavascriptInterface` (legacy
fallback) — `setupMessaging` feature-detects and picks the right
install path. UE→JS push prefers `JavaScriptReplyProxy.postMessage`
when the page has already posted (giving us a reply channel) and
falls back to `evaluateJavascript` otherwise. See "Android hardening"
section below for the full migration story. User JS is identical
across platforms.

Events from Java to C++ go through fourteen `nativeOn...` JNI exports
(message / navigation-starting / navigation-completed / title-changed /
script-dialog / new-window / got-focus / lost-focus / process-failed /
render-unresponsive / render-responsive / console-message /
nav-state-changed / capture-preview-complete)
routed via a single `DispatchOnGameThread<Lambda>` helper that:
(1) marshals onto the game thread via `AsyncTask`, (2) re-acquires the
impl registry lock, (3) invokes the lambda with the live impl pointer.
Lock is held through the callback so Shutdown (which removes from the
registry) serializes correctly.

### Debugging an Android build

- `adb logcat | findstr /I "InoWebUI"` — filters to our LogInoWebUI + Java `Logger` output
- Chromium DevTools for the WebView content: `chrome://inspect/#devices` in desktop Chrome with the device connected via adb. The Android WebView exposes its own devtools remotely — no API call needed, no Phase 3 DevTools plumbing.
- If the Java helper isn't found ("InoWebViewAndroid Java class not found"), check:
  1. UPL was registered (look for "InoWebUI: UPL init (Android)" in build log)
  2. Java file got copied — check `Intermediate/Android/APK/src/net/inoland/webui/`
  3. ProGuard isn't stripping the class (our `-keep` rule should prevent this)

---

## Android hardening (Phases 14–18)

A doc-driven hardening pass aligning the Android impl with every Android /
AndroidX WebView best-practice for `minSdk 28 → targetSdk 35`. None of these
changes break the public API — old code keeps working. AndroidX webkit dep
bumped to `1.16.0` (current stable) in the UPL `buildGradleAdditions`.

### JNI hygiene (Phase 14)

- **UTF-16 round-trip**, not Modified UTF-8. `FStringToJString` /
  `JStringToFString` use `Env->NewString` / `GetStringChars` with `FTCHARToUTF16`
  / `FUTF16ToTCHAR` bridging. The old `NewStringUTF` / `GetStringUTFChars`
  pair used Java's Modified UTF-8, which corrupts U+0000 and any
  supplementary-plane character (≥ U+10000 — every emoji). Replace all
  `NewStringUTF(TCHAR_TO_UTF8(*X))` patterns with `FStringToJString(Env, X)`
  when adding new JNI call sites.
- **`ExceptionCheck` + `ExceptionClear`** after `FindJavaClass` and after the
  block of `GetStaticMethodID` calls in `InoWebUIJNI::Init`. A failed lookup
  leaves the exception sticky on the JNIEnv; the next throwing JNI call
  aborts the VM under CheckJNI.
- **`java.lang.String` cached as a global ref** (`InoWebUIJNI::StringClass`),
  not re-`FindClass`-ed per call. Drops a per-call leak risk and removes the
  missing-`ExceptionCheck` path.

### `PixelCopy` for screenshots (Phase 14)

`CapturePreview` uses `PixelCopy.request(activity.getWindow(), srcRect, bmp,
listener, copyHandler)` on a dedicated `HandlerThread`. The old
`WebView.draw(Canvas)` path was Chromium's deprecated software-draw — it
silently dropped HW-accelerated WebView content (blank or partial PNGs) and
didn't handle `<video>`, WebGL, or accelerated CSS. PixelCopy reads the final
SurfaceFlinger composite, so the result matches what's on screen. Available
since API 24; minSdk 28 satisfies the Rect overload at API 26+.

### Renderer failure recovery (Phases 15 + 16)

Two failure modes, one recovery path. The plugin handles both automatically;
user code only has to bind `OnProcessFailed` if it wants observability.

| State | Detected via | Defaults |
|---|---|---|
| **Renderer crashed / OS-killed** | `WebViewClient.onRenderProcessGone` (API 26+) | Java does doc-mandated cleanup (detach + `destroy()` + drop from `sWebViews` / `sConfigs`). C++ fires `OnProcessFailed` BP delegate, then auto-recovers if `bAutoRecoverOnProcessFailed` (default true). |
| **Renderer hung** (~5s no event-loop response) | `WebViewCompat.setWebViewRenderProcessClient.onRenderProcessUnresponsive` | Fires `OnRenderProcessUnresponsive`. If `Config.UnresponsiveTimeoutMs > 0` (default 10000), schedules `WebViewRenderProcess.terminate()` after that delay — which triggers the crashed-renderer path above. |
| **Renderer unstuck itself** | `onRenderProcessResponsive` | Pending terminate is cancelled via `view.removeCallbacks`. `OnRenderProcessResponsive` BP delegate fires. |

**Auto-recover semantics** (`UInoWebView::RecreateImpl`):

1. Snapshot `Impl->GetURL()` BEFORE `Shutdown`. This is what we reload to —
   not `SavedConfig.InitialURL` — so a user deep into an SPA hash route
   comes back to that route, not the home page.
2. `Impl->Shutdown()` → reset `Impl` to nullptr.
3. `Impl = CreateInoWebViewImpl()` — fresh native impl with a NEW
   `InstanceId` (the C++ counter increments). Java's `sWebViews` for the old
   ID was already cleared in `onRenderProcessGone`.
4. `WireImplCallbacks()` re-binds every `IInoWebViewImpl` callback slot on
   the fresh impl. Extracted from `Init` precisely for this reuse.
5. `Impl->Initialize(SavedParentNativeHandle, ConfigCopyWithSnapshottedURL)`.
6. Re-assert runtime state that diverged from config defaults: visibility,
   transparency, manual bounds.
7. `RefreshCoveringState()` re-engages the subsystem's auto engine-idle.

**State preservation across recreate** (lives in the WebView data dir, not
the renderer process — survives automatically):

- Cookies, localStorage, sessionStorage, IndexedDB, Service Worker caches.
- All C++ / BP delegate bindings on the persistent `UInoWebView` UObject.
- Bounds / visibility / opacity / manual-bounds state (re-asserted above).

**Lost** (same as a normal F5 reload — page must handle):

- In-page JS variables, live WebSocket / EventSource connections, scroll
  position, history past the snapshotted URL.

**Recovery budget**: sliding window of 3 attempts per 60 seconds. When
exceeded, the WebView is left dead and a clear error is logged. Prevents an
infinite recreate loop on a page that consistently crashes the renderer
(broken WebGL shader, OOM-on-load, etc.).

**User-destroyed-during-handler guard**: if the BP `OnProcessFailed` handler
calls `DestroyWebView`, our `Impl` is `Reset` to null. `HandleProcessFailed`
checks `Impl.IsValid()` after the Broadcast and bails before scheduling the
recreate. The deferred `AsyncTask` also re-checks `bAwaitingRecreate` (which
`ShutdownImpl` clears) so a recreate scheduled moments before destroy can't
materialize a phantom WebView.

**Multi-WebView warning**: Android shares renderer processes across
WebViews by default. `WebViewRenderProcess.terminate()` for one hung
WebView kills the whole process — every co-resident WebView also fires
`onRenderProcessGone` and auto-recovers (brief visible reload). Logged
when `sWebViews.size() > 1` at terminate time so the dev understands the
collateral.

### JS bridge: `addWebMessageListener` (Phase 17)

`setupMessaging` chooses bridge install path by feature-detection:

1. **`WebViewCompat.addWebMessageListener`** (modern, ~Chrome 82+) — gated
   by `WebViewFeature.WEB_MESSAGE_LISTENER`. Injects a `_InoWebUIHost` with
   `.postMessage` + `.onmessage`. **Origin-allowlisted**: `{"https://<vhost>"}`
   when a virtual host is set, `{"*"}` otherwise. Listener runs on the UI
   thread (vs. JavaBridge background thread). Stores
   `JavaScriptReplyProxy` per post for the UE→JS direction.
2. **`addJavascriptInterface`** (legacy) — installed only when the modern
   feature isn't supported. Injects `_InoWebUIHost.receive`. No origin
   check, callback runs on `JavaBridge` thread.

**UE → JS push**, in fall-through order:

1. `JavaScriptReplyProxy.postMessage(json)` — when the page has already
   posted at least once (we captured a reply channel). No JS-engine
   round-trip, no string-interpolation risk, supports binary `ArrayBuffer`
   if we ever extend the wire format.
2. `evaluateJavascript("window._InoWebUIDispatch(<jsLiteral>)")` — legacy
   fallback. Used pre-handshake (before JS posts) and on the
   addJavascriptInterface path. `jsStringLiteral` helper handles U+2028 /
   U+2029 escaping.

**`bridge.js`** detects both shapes on `window._InoWebUIHost` (`.postMessage`
vs `.receive`) and mounts the matching transport. Bumped to v1.2. Page
authors don't change anything — the `window.InoWebUI.send / on / off / once`
API is identical regardless of which native path was used.

**Security model**: in production with a virtual host configured, only the
page served from `https://<vhost>` sees `_InoWebUIHost`. Third-party
iframes, redirects, ads — none of them can call the bridge. With the
legacy path the bridge was exposed to every frame with no origin check.

### Console message capture (Phase 18)

`WebChromeClient.onConsoleMessage` is overridden in `InoWebChromeClient`.
Maps `ConsoleMessage.MessageLevel` ordinal → `EInoConsoleMessageLevel`
(Tip=0, Log=1, Warning=2, Error=3, Debug=4 — matches Android's enum
ordering so the JNI bridge passes the level through as a plain `jint`).

Fires the `OnConsoleMessage(Level, Message, SourceID, Line)` BP delegate.
Also mirrored to `LogInoWebUI` at matching verbosity (`Error` → UE Error,
`Warning` → Warning, `Log` / `Tip` → Log, `Debug` → Verbose) so developers
see page logs in their UE console without binding the delegate. Returning
`false` from `onConsoleMessage` keeps WebView's own logcat mirror intact
for `adb logcat | grep chromium` workflows.

Android-only today. Windows / iOS impls don't surface console messages
(WebView2 has no equivalent event; WKWebView routes only to Safari Web
Inspector).

### Explicit security defaults (Phase 18)

Three new `FInoWebViewConfig` bool fields, all default false:

| Field | Setting | Why explicit |
|---|---|---|
| `bAllowMixedContent` | `WebSettings.setMixedContentMode(NEVER_ALLOW / ALWAYS_ALLOW)` | API 21+ default is NEVER_ALLOW; setting explicit removes reliance on future framework default changes. Turn on only for dev flows mixing schemes. |
| `bAllowFileURLs` | `setAllowFileAccess(bool)` | Default changed from `true` → `false` at API 30. Now consistent across our minSdk 28..targetSdk 35 range. Game UI served from a vhost never needs `file://`. |
| `bAllowContentURIs` | `setAllowContentAccess(bool)` | Same defense-in-depth motivation. Game UI doesn't need `content://` cross-app file sharing. |

Wired through a Java `configureSecurity(id, mixed, file, content)` static
method, called from C++ Initialize right after `configureDialogs`.

### Renderer priority (Phase 14)

`wv.setRendererPriorityPolicy(WebView.RENDERER_PRIORITY_BOUND, true)` at
WebView creation. Framework default is `IMPORTANT` (pinned at higher than
host activity) — appropriate for a primary-content browser, not a game-UI
overlay. `BOUND` + `waivedWhenNotVisible=true` lets Android reclaim
renderer memory the moment our overlay is hidden, and re-bound on next
show. If Android terminates the waived renderer under memory pressure,
`onRenderProcessGone` fires → auto-recover takes over.

### Cleartext URL warning (Phase 14)

`WarnIfCleartextHttpURL(URL, CallSite)` runs at `Navigate`, `Initialize`
(for `InitialURL`), and `LoadURLWithHeaders`. Logs a loud warning when the
URL starts `http://` (case-insensitive). On API 28+ the default
`NetworkSecurityConfig` blocks cleartext silently with
`ERR_CLEARTEXT_NOT_PERMITTED` — devs spelunking through logcat for the
cause is a recurring trap. The warning points them at
`usesCleartextTraffic="true"` or a per-host `network_security_config`.

---

## iOS implementation (Phase 13)

The iOS impl mirrors the Android architecture in shape — the same `IInoWebViewImpl` contract, the same registry-lock pattern for delegate-thread re-entry, the same lockdown rules, the same `bridge.js` payload — but implemented through `WKWebView` instead of `android.webkit.WebView`.

### Architecture

```
UE game thread (C++)
     │   dispatch_async(dispatch_get_main_queue(), ^{ ... })
     ▼
iOS main thread (Objective-C++)
     ▼
WKWebView
     • sibling subview of [IOSAppDelegate GetDelegate].RootView
     • added ABOVE FIOSView (the Metal-layer game surface)
     • transparent background → UE scene shows through
```

### Files

```
Plugins/InoWebUI/Source/InoWebUI/
├── Private/Impl/iOS/
│   ├── InoWebViewImpl_iOS.h          NO Apple types — pimpl boundary
│   └── InoWebViewImpl_iOS.mm         All WebKit/UIKit Obj-C++ here
└── Private/Generated/
    └── InoWebUIScripts_iOS.generated.h   NSString @"..." constants
                                         (regenerated by GenerateJSConstants.ps1)
```

### Threading rule

`WKWebView` is main-thread-only. Public C++ entry points run on the game thread (asserted via `check(IsInGameThread())`); each one marshals onto main via `dispatch_async`. Initialize uses `dispatch_sync` so `IsReady()` is true by the time the call returns. Delegate callbacks fire on main and marshal back to game thread via `AsyncTask(ENamedThreads::GameThread, ...)` — same `DispatchOnGameThread<Lambda>` shape as Android.

### Pimpl boundary

`InoWebViewImpl_iOS.h` includes ONLY `CoreMinimal.h` and `IInoWebViewImpl.h`. All `WKWebView*` / `UIView*` / `NSString*` types live behind a `void* InternalPtr` to a struct defined inside the `.mm`. Apple types never leak into the rest of the build.

### Virtual host divergence

WKWebView does not expose a `SetVirtualHostNameToFolderMapping` equivalent and does NOT permit intercepting `https://` requests. The iOS impl registers a `WKURLSchemeHandler` for the custom scheme **`inoweb`** and translates `inoweb://<VirtualHostName>/<path>` into a local-folder read.

For consumer code this means: on iOS, set `Config.InitialURL = TEXT("inoweb://<your-host>/index.html")` — NOT `https://<your-host>/...`. The `bLockToVirtualHost` rule is extended to whole-host match the `inoweb:` scheme as well.

If you ship the same `FInoWebViewConfig` to both Android (https) and iOS (inoweb), you'll need to swap the `InitialURL` per platform — there's no transparent translation. (A consumer-side `#if PLATFORM_IOS` is the simplest fix.)

### Feature matrix (iOS)

| API | iOS | Notes |
|---|:-:|---|
| Create / Destroy | ✔ | |
| Navigate / Reload / Show / Hide | ✔ | |
| SyncBounds (manual mode) | ✔ | UE pixels → UIKit points via `[UIScreen mainScreen].scale` |
| Auto bounds | ✔ | Tracks parent's bounds via `UIViewAutoresizingFlexible{Width,Height}` |
| Transparent background | ✔ | `webView.opaque = NO` + `clearColor` |
| Virtual-host mapping | ✔ | Custom `inoweb` scheme via `WKURLSchemeHandler` (see above) |
| Web Bundle assets | ✔ | Cross-platform runtime logic |
| Two-way messaging | ✔ | `WKScriptMessageHandler` in `WKContentWorld.defaultClientWorld` (iOS 14+) + `pageWorld` shim via DOM `CustomEvent` so page JS can't intercept the bridge; UE→JS uses `evaluateJavaScript:in:inContentWorld:` targeting `defaultClientWorld` |
| Navigation events | ✔ | `decidePolicyForNavigationAction:preferences:` (iOS 13+ variant) with per-nav `WKWebpagePreferences.allowsContentJavaScript` from `Config.bAllowJavaScript` + `didStart/didFinish/didFail*Navigation` |
| Lockdown | ✔ | `decidePolicyForNavigationAction` (whole-host + wildcard list) |
| JS dialog suppression | ✔ | `runJavaScriptAlert/Confirm/TextInputPanel` immediate-cancel completion |
| `window.open` blocking | ✔ | `createWebViewWithConfiguration` returns `nil`, fires callback |
| Crash event | ✔ | `webViewWebContentProcessDidTerminate:` |
| `ExecuteJavaScript` / UA override | ✔ | `evaluateJavaScript:` / `customUserAgent` |
| `SetCookie` / `ClearAllCookies` | ✔ | `WKWebsiteDataStore.httpCookieStore` / `removeDataOfTypes:` |
| `ClearAllData` | ✔ | `removeDataOfTypes:[WKWebsiteDataStore allWebsiteDataTypes]` |
| `LoadHTMLString(html, baseURI)` | ✔ | `loadHTMLString:baseURL:` (baseURI honoured natively) |
| `LoadURLWithHeaders` | ✔ | `NSMutableURLRequest setValue:forHTTPHeaderField:` |
| `CapturePreview` (PNG/JPEG) | ✔ | `takeSnapshotWithConfiguration:` + UIImagePNG/JPEGRepresentation |
| `SetZoomFactor` / `GetZoomFactor` | ✔ | Native `WKWebView.pageZoom` (iOS 14+); persists across navigations, no JS injection. Cached value returned by getter. |
| `OpenDevTools` (programmatic) | ✔ (iOS 16.4+) | No in-process panel — Safari Web Inspector only. `bEnableDevTools` sets `WKWebView.inspectable = YES` so the WebView is attachable from the Mac's Safari → Develop menu in TestFlight / Release builds too; `OpenDevTools()` logs the workflow. Pre-16.4: debug builds were always inspectable, release builds never were. |
| `SetMuted` / `bStartMuted` | — | WKWebView has no native mute API; the call logs a warning and no-ops (parity with Android). Mute media from page JS instead. |
| `bEnableContextMenus` | — | Not enforced on iOS — the long-press/context UI is not overridden by the impl. |
| `bEnableAcceleratorKeys` | N/A | F5/F12/Ctrl+F are desktop-only concepts |
| `bShowStatusBar` | N/A | Windows-only feature |

### Debugging an iOS build

- Connect the device to a Mac via USB.
- Enable **Develop** menu in Safari (Safari → Settings → Advanced → "Show features for web developers").
- In Safari, pick **Develop → \<DeviceName\> → \<Your WebView\>** to open the Web Inspector against the live page.
- iOS device console output (`os_log`) goes to **Console.app** on the connected Mac — filter by your app's bundle id.

---

## File layout

```
Plugins/InoWebUI/
├── InoWebUI.uplugin
├── CLAUDE.md                                     (you are here)
├── README.md
├── Scripts/
│   └── AcquireWebView2SDK.ps1                    downloads WebView2 from NuGet
├── Content/
│   ├── web/index.html                            self-contained showcase / test harness
│   └── DA_IW_Main.uasset                          sample UInoWebBundle asset
├── Source/
│   ├── ThirdParty/WebView2/                      headers + static lib
│   │   ├── include/{WebView2.h, WebView2EnvironmentOptions.h}
│   │   ├── lib/Win64/WebView2LoaderStatic.lib
│   │   └── VERSION                               SDK version stamp
│   ├── InoWebUI/
│   │   ├── InoWebUI.Build.cs                     links static loader + system libs
│   │   ├── InoWebUI_UPL.xml                      Android Unreal Plugin Language config
│   │   ├── Java/src/net/inoland/webui/           InoWebViewAndroid.java + InoWebUIScripts.java
│   │   ├── JS/                                   bridge.js, bridge_ios.js, dev_overlay.js (source of truth)
│   │   ├── Public/
│   │   │   ├── InoWebUI.h                        module
│   │   │   ├── InoWebUILog.h                     LogInoWebUI category
│   │   │   ├── InoWebUITypes.h                   FInoWebViewConfig + FInoWebViewSettings (View)
│   │   │   ├── InoWebUISubsystem.h               UGameInstanceSubsystem API
│   │   │   ├── InoWebView.h                      UObject handle API
│   │   │   ├── InoWebBundle.h                    UInoWebBundle + FInoWebBundleFile
│   │   │   └── IInoWebViewImpl.h                 pimpl contract (no platform headers)
│   │   └── Private/
│   │       ├── InoWebUI.cpp
│   │       ├── InoWebUISubsystem.cpp
│   │       ├── InoWebView.cpp
│   │       ├── InoWebBundle.cpp
│   │       ├── Generated/
│   │       │   ├── InoWebUIScripts.generated.h        (C++)
│   │       │   └── InoWebUIScripts_iOS.generated.h    (Obj-C++ NSString*)
│   │       └── Impl/
│   │           ├── InoWebViewFactory.cpp         platform-dispatches
│   │           ├── Windows/
│   │           │   ├── InoWebViewImpl_Windows.{h,cpp}                child HWND
│   │           │   └── InoWebViewImpl_Windows_Composition.{h,cpp}    DirectComposition (PIE)
│   │           ├── Android/
│   │           │   └── InoWebViewImpl_Android.{h,cpp}
│   │           └── iOS/
│   │               └── InoWebViewImpl_iOS.{h,mm}
│   └── InoWebUIEditor/                           editor-only module (UInoWebBundle factory + actions)
```

---

## Naming convention

Every public type in this plugin starts with **`Ino`**. No exceptions. That
includes structs, enums, UObject classes, and plain C++ classes exposed from
a public header.

---

## Build

1. Run `Scripts/AcquireWebView2SDK.ps1` once (downloads ~5 MB from NuGet).
   Idempotent — re-running is a no-op if the version stamp matches.
2. Regenerate UE project files.
3. Build. `Source/ThirdParty/WebView2/` is gitignored; the script produces it.

`Build.cs` links `WebView2LoaderStatic.lib` (the static loader shim). No
extra DLL is shipped — the shim finds the system Edge/WebView2 runtime at
launch. Required system libs: `shlwapi.lib`, `version.lib`, `ole32.lib`,
plus `dcomp.lib` (DirectComposition, used only by the PIE composition-hosting
impl; ships on Win8+ so no new minimum-platform requirement).

If the WebView2 Runtime is missing (rare on Win10+), `Initialize()` logs a
clear error pointing to the Evergreen installer rather than crashing.

---

## Usage (Phase 1)

```cpp
// C++ — typical BeginPlay:
UInoWebUISubsystem* WebUI = GetGameInstance()->GetSubsystem<UInoWebUISubsystem>();

FInoWebViewConfig Config;
Config.InitialURL                  = TEXT("http://localhost:5173");
Config.View.bTransparentBackground = true;   // view-appearance toggles live in Config.View
Config.View.bVisibleOnCreate       = true;

UInoWebView* View = WebUI->CreateWebView(TEXT("MainUI"), Config);
```

All three functions are BlueprintCallable — typical usage is three BP nodes
on `BeginPlay`.

Later:
```cpp
View->LoadURL(TEXT("https://example.com"));  // safe even before ready
View->Hide();
View->Show();
View->Reload();
WebUI->DestroyWebView(TEXT("MainUI"));
```

### Loading local content

**Recommended (Phase 4): virtual-host mapping.** Serve a folder via a fake
https origin. Fixes every `file://` pitfall (React Router, relative imports,
fetch CORS, service workers, ES modules).

```cpp
FInoWebViewConfig Config;
Config.VirtualHostName   = TEXT("inoweb.local");
Config.VirtualHostFolder = TEXT("WebUI/dist");       // <project>/Content/WebUI/dist
Config.InitialURL        = TEXT("https://inoweb.local/index.html");
Subsystem->CreateWebView(TEXT("MainUI"), Config);
```

Or, if your Vite/Webpack config sets `base: './'` (relative assets), even
simpler — just point the InitialURL at `index.html` and every subresource
resolves from the mapped folder automatically.

**Legacy: direct file path.** Works for single-file HTML with no imports:

```cpp
View->LoadLocalFile(TEXT("WebUI/dist/index.html"));
// resolves to  file:///<ProjectContentDir>/WebUI/dist/index.html
```

---

## Two-way messaging (Phase 2)

### Wire format

All messages — both directions — are a fixed JSON envelope:

```
{ "channel": "<string>", "payload": <any JSON value> }
```

The JS bridge (`window.InoWebUI`) and the C++ `UInoWebView` both produce
and consume that shape. The channel is a logical dispatch key; the payload
is whatever JSON your message needs.

### UE -> JS

```cpp
// C++
FJsonObjectWrapper Payload;
Payload.JsonObject->SetNumberField(TEXT("hp"), 80);
Payload.JsonObject->SetNumberField(TEXT("ammo"), 24);
View->PostMessage(TEXT("playerState"), Payload);
```

In Blueprint, construct a JsonObject using the **JsonBlueprintUtilities**
nodes (auto-enabled by this plugin):
  - `Load Json from String` — parse `{"hp":80,"ammo":24}` into a wrapper
  - `Set Field` — add/replace individual fields (typed: int/float/string/...)
  - `Post Message` — send it

Empty / invalid wrappers send the literal `null`. Safe to call before
`IsReady()` — messages are queued and replayed once the WebView's async
construction finishes.

### JS -> UE

```cpp
// C++
View->OnMessageReceived.AddDynamic(this, &AMyActor::HandleWebMessage);

void AMyActor::HandleWebMessage(FName Channel, const FJsonObjectWrapper& Payload)
{
    if (Channel == TEXT("startMission") && Payload.JsonObject.IsValid())
    {
        const FString Id = Payload.JsonObject->GetStringField(TEXT("id"));
        // ...
    }
}
```

In Blueprint the delegate appears as a red event pin on the `UInoWebView`
with `Channel` (FName) and `Payload` (JsonObject) outputs. Use the
JsonBlueprintUtilities nodes — `Get Field`, `Has Field`, `Get Field Names`,
`Get Json String` — to read fields.

Non-object JS payloads (scalars, arrays, null) are auto-wrapped in
`{"value": <payload>}` so the BP side always receives a usable JsonObject.

### JS-side API

A `window.InoWebUI` object is auto-injected on every page load via
`ICoreWebView2::AddScriptToExecuteOnDocumentCreated`, so it's always
available before any page script runs.

```js
// Send UE -> receive in C++ OnMessageReceived
window.InoWebUI.send('startMission', { id: 'tutorial', difficulty: 'hard' });

// Subscribe to C++ PostMessage
window.InoWebUI.on('playerState', (data) => {
  hpBar.setWidth(data.hp);
  ammoLabel.textContent = data.ammo;
});

// Unsubscribe
window.InoWebUI.off('playerState', handlerRef);
```

The bridge is ES5-compatible (no `const`, no `Map`, no arrow functions) so
it works on arbitrary pages regardless of transpile target.

### Threading

All messaging runs on the game thread. WebView2 fires callbacks on the
thread that created the environment (us: game thread). Blueprint dynamic
multicast broadcasts also happen on the calling thread. End-to-end
single-threaded — no `AsyncTask(ENamedThreads::GameThread, ...)` needed.

---

## Runtime polish (Phase 3)

### Config toggles

All opt-in on `FInoWebViewConfig`, applied once at `CreateWebView` time:

| Field | Default | What it does |
|---|---|---|
| `bEnableDevTools` | `false` | Enables the Chromium DevTools window. F12 opens it if `bEnableAcceleratorKeys` is also true; otherwise open programmatically with `UInoWebView::OpenDevTools()`. |
| `bEnableContextMenus` | `false` | When true, right-clicking the WebView shows the browser context menu. Usually off for game UI. |
| `bEnableAcceleratorKeys` | `false` | When true, browser shortcuts (F5, F12, Ctrl+F, Ctrl+P, …) are active. Usually off for game UI so those keys go to the game. |
| `bStartMuted` | `false` | Audio is muted on creation. Call `SetMuted(false)` later to unmute. |
| `UserAgentOverride` | empty | Overrides `navigator.userAgent` inside the WebView. |

All defaults are "locked down for game UI." A dev build typically wants
`bEnableDevTools=true` and leaves the rest false.

### Runtime methods on `UInoWebView`

| Method | Purpose |
|---|---|
| `OpenDevTools()` | Open the DevTools panel. No-op if `bEnableDevTools` wasn't set at construction (logs a warning). There's no `CloseDevTools` — WebView2 has no such API; the user closes it themselves. |
| `ExecuteJavaScript(Code)` | Run arbitrary JS in the page. Fire-and-forget — if you need a result back to UE, have the JS side `window.InoWebUI.send(...)` instead. Queued if pre-ready. |
| `SetMuted(bool)` | Mute/unmute audio. Queued if pre-ready. |

### Implementation notes

- Config is applied in `FInoWebViewImpl_Windows::OnControllerReady` right
  after the transparent-background setup. Each non-base COM interface
  (`ICoreWebView2Settings2`, `Settings3`, `ICoreWebView2_8`) is obtained via
  `QueryInterface` and silently skipped if unavailable (tolerates older Edge).
- `ExecuteJavaScript` takes a required handler callback per the WebView2 API;
  we pass a no-op handler (returns `S_OK`) since UE gets any response back
  via the messaging channel, not a script return value.
- Pre-ready queue semantics mirror Phase 1/2: `PendingScripts` array,
  `PendingMute` `TOptional<bool>`. Replayed from `ApplyPendingOperations()`
  on controller-ready.

---

## Game-UI hardening (Phase 5)

Every setting here is default-ON for "locked-down game UI" behavior. Flip
off individually for dev ergonomics.

### Navigation lockdown

```
bLockToVirtualHost      bool             = true          (locked by default)
AllowedURIPatterns      TArray<FString>  = []            (UE wildcard list)
```

A URI is allowed iff any of:
1. Internal scheme — `about:`, `data:`, `blob:` (always)
2. `bLockToVirtualHost == false`
3. Host matches `VirtualHostName` (case-insensitive whole-host)
4. Full URI matches any entry in `AllowedURIPatterns` via `FString::MatchesWildcard`

Otherwise the navigation is cancelled with a warning log. Use for defense
against accidental navigation away from your UI (bad links, injected third-
party scripts, auth redirects).

Common pattern examples:
```
"https://*.api.company.com/*"    # subdomains of your API
"http://localhost:*\/*"          # any localhost port (dev only)
"https://cdn.jsdelivr.net/*"     # specific CDN
```

### Other hardening toggles

| Field | Default | Behavior when off (default) |
|---|---|---|
| `bAllowScriptDialogs`  | `false` | JS `alert()` / `confirm()` / `prompt()` / `onbeforeunload` are suppressed — `confirm` returns false, `prompt` returns null, `alert` fires no dialog. `OnScriptDialog` still broadcasts for logging. |
| `bAllowNewWindows`     | `false` | `window.open()` and `target="_blank"` are blocked — no popup Chromium window ever appears over the game. `OnNewWindowRequested` fires so BP can `LoadURL(URI)` to redirect into the same frame. |
| (from Phase 3) `bEnableContextMenus`    | `false` | Right-click is a no-op inside the WebView. |
| (from Phase 3) `bEnableAcceleratorKeys` | `false` | F5/F12/Ctrl+F/Ctrl+P/etc. pass through to the game. |

### Navigation events (BP delegates on `UInoWebView`)

| Delegate | Signature | When |
|---|---|---|
| `OnNavigationStarting`     | `(URI: String)`                   | Before every navigation (observation only — lockdown handles cancellation internally). |
| `OnNavigationCompleted`    | `(bSuccess: bool, URI: String)`   | Page finished loading or failed. Use for splash dismissal / loading indicators. |
| `OnDocumentTitleChanged`   | `(Title: String)`                 | Page called `document.title = …`. |
| `OnScriptDialog`           | `(Kind: EInoScriptDialogKind, Message: String)` | Any alert/confirm/prompt/beforeunload. |
| `OnNewWindowRequested`     | `(URI: String)`                   | `window.open`/`_blank` (blocked by default). |
| `OnGotFocus` / `OnLostFocus` | (no params) | WebView gained/lost keyboard focus. |
| `OnProcessFailed`          | `(Description: String)`           | Chromium subprocess crashed. Consider `Reload()` or recreate. |

### Runtime methods (BP-callable on `UInoWebView`)

| Method | Purpose |
|---|---|
| `FocusWebView()`         | Move keyboard focus into the WebView (for HTML input fields). |
| `SetZoomFactor(Factor)`  | 1.0 = 100%, 1.5 = 150%. |
| `GetZoomFactor()`        | Current zoom (returns 1.0 if not ready). |
| `SetBackgroundTransparent(bool)` | Runtime equivalent of `bTransparentBackground` — switch see-through ↔ opaque white. No-ops if unchanged; feeds auto engine-idle (`RefreshCoveringState`). |
| `IsBackgroundTransparent()` | BlueprintPure — current background mode. |
| `ClearAllCookies()`      | Delete all cookies in this WebView's isolated profile. Useful for logout. |

### Ready signal

`UInoWebView::OnReady` (BP-assignable, no params) fires exactly once,
on the game thread, on a tick AFTER `CreateWebView` returned — regardless
of whether the underlying native construction was async (Windows) or
sync (Android). Bind it right after `CreateWebView` to run code as soon
as the WebView is ready for operations. If you bind after ready has
already fired, use `IsReady()` as a fallback.

### Engine idle (performance)

When a full-screen **opaque** web UI is up, the 3D scene is completely
hidden — rendering it is wasted GPU / CPU / battery. The subsystem can
"idle" the engine with three switches:

1. `UGameViewportClient::bDisableWorldRendering = true` — stop 3D render
2. `GEngine->SetMaxFPS(8)` — throttle the frame loop
3. `UGameplayStatics::SetGamePaused(true)` — freeze gameplay

The pre-idle Max FPS and pause state are snapshot on the way in and
restored **exactly** on the way out (idempotent, no drift).

The controls live entirely on the **subsystem** (there is no per-view
config flag):

| Surface | What |
|---|---|
| `UInoWebUISubsystem::SetAutoEngineIdle(bool)` | **The main toggle.** When on, the engine idles whenever ANY visible WebView is opaque (covering the scene) and resumes when none are. Default off. |
| `UInoWebUISubsystem::IsAutoEngineIdle()` | BlueprintPure — is auto mode on. |
| `UInoWebUISubsystem::SetEngineIdle(bool)` | Manual override of the same switches, independent of auto. |
| `UInoWebUISubsystem::IsEngineIdle()` | BlueprintPure — is the engine currently idled. |

Resolved idle = `SetEngineIdle(true)` **OR** (`SetAutoEngineIdle` on
**AND** a covering view exists). Each `UInoWebView` tracks its own
visibility + opacity (seeded from config, updated on Show/Hide and on
`SetBackgroundTransparent` / the dev-overlay transparency toggle) and
reports a raw "covering" (opaque && visible) bool to the subsystem via
`SetViewCovering`. The
subsystem keeps an auto-pruned weak-ref `CoveringViews` set + the manual
& auto flags, and only touches the engine when the resolved state flips.
`Deinitialize` force-restores so a torn-down subsystem never leaves the
game paused/throttled. The WebView is OS-composited independently of the
UE loop, so the page stays smooth while Unreal idles.

Note this is the one place the plugin reaches into engine-wide state —
strictly opt-in (auto default off / explicit manual call).

### Dev-tools floating overlay

Gated by `FInoWebViewConfig::bEnableDevTools`. When enabled, a circular
**⚙** button appears in the WebView's bottom-right corner. Clicking it
expands a vertical toolbar of four action buttons. An always-visible
**FPS chip** sits just left of the gear, measuring the WebView layer's
own frame rate (UI jank, independent of UE's render thread):

| Button | Action | Routing |
|---|---|---|
| ↻ Refresh | Reload the page | JS → `_devtools.refresh` → `Impl->Reload` |
| ⌥ DevTools | Open Chromium DevTools (Windows native panel / Android `chrome://inspect` hint / iOS Safari Web Inspector hint) | JS → `_devtools.openDevTools` → `Impl->OpenDevTools` |
| ⓘ Info | Show a modal with URL, title, platform, viewport, UA, bridge status, etc. | JS-only, no UE hop |
| ◆ Dev Callback | Fire the `OnDevCallback` BP delegate (your project-specific hook) | JS → `_devtools.devCallback` → `FOnInoWebDevCallback` broadcast |

(Earlier revisions also shipped Clear Data / Transparency / Hide WebUI
buttons; those were removed from `dev_overlay.js`. The C++ interceptors
for their `_devtools.clearData` / `_devtools.toggleTransparency` /
`_devtools.hideWebUI` channels still exist in `UInoWebView` but are no
longer reachable from the overlay.)

`_devtools.*` channels are intercepted inside
`UInoWebView::DispatchIncomingEnvelope` before the user's
`OnMessageReceived` delegate runs. User code never sees them.

Overlay is injected at document-start on every navigation (Windows:
`AddScriptToExecuteOnDocumentCreated`; iOS: `WKUserScript` at
`InjectionTimeAtDocumentStart`; Android: `WebViewCompat.
addDocumentStartJavaScript`, falling back to `WebViewClient.onPageStarted`
only on pre-2020 System WebView — see "Bridge injection timing" above).
The JS source-of-truth lives in
`Source/InoWebUI/JS/dev_overlay.js`; `Scripts/GenerateJSConstants.ps1`
emits a C++ header (`Private/Generated/InoWebUIScripts.generated.h`,
auto-chunked under MSVC's 16380-char string-literal limit) and a Java
constant (`Java/.../InoWebUIScripts.java`). Edit the `.js` file then
re-run the script — both platforms stay in sync automatically. The
generated outputs are committed so a fresh checkout builds without
the script.

---

## Web Bundle assets (Phase 7)

Shipping a `Content/web/` folder as loose files works but has two problems:
it clutters the packaging settings (`Additional Non-Asset Directories to
Copy`), and those files sit on the user's disk in plain sight.

`UInoWebBundle` is an alternative: a standard UE asset that holds the
built web content inside itself and extracts on demand at runtime. Ships
inside the `.pak` (encrypted if pak encryption is enabled), shows up in
the Content Browser under a custom "Ino" category, re-bundles with a
right-click Reimport.

### Create one

```
Right-click in Content Browser → Ino → Web Bundle
  → name it, e.g., "MainUI"
  → set fields in the details panel:
       Source Folder       = Content/WebUI/dist      (relative to Content/)
       Initial URL         = https://ui.local/index.html
       Virtual Host Name   = ui.local
  → right-click the asset → Reimport Source Folder
       Files[] populated, ContentHash set, asset marked dirty
  → Ctrl+S to save
```

### Use it from C++/BP

```cpp
UInoWebUISubsystem* Subsystem = GI->GetSubsystem<UInoWebUISubsystem>();
UInoWebView* View = Subsystem->CreateWebViewFromAsset(TEXT("MainUI"), MyBundle);
```

In Blueprint: the node is "Create Web View From Bundle" off the subsystem.

### What happens at runtime

| Build | Behavior |
|---|---|
| Editor / non-cooked | Serves loose files directly from `SourceFolder` — hot-iteration friendly, no extraction latency |
| Packaged / cooked | Extracts `Files[]` to `<ProjectSavedDir>/InoWebBundles/<AssetName>/` on first use. Hash-sidecar (`.inowebbundle.hash`) prevents re-extraction unless the asset's `ContentHash` changes across patches |

### Layout in the plugin

```
Source/
├── InoWebUI/                          (runtime — Win64 + Android)
│   ├── Public/InoWebBundle.h         the UObject + FInoWebBundleFile USTRUCT
│   └── Private/InoWebBundle.cpp      BundleFromFolder / ExtractToDirectory / ComputeHash
└── InoWebUIEditor/                    (new editor-only module)
    └── Private/
        ├── InoWebBundleFactory.cpp   UFactory → "New Asset → Ino → Web Bundle"
        └── InoWebBundleActions.cpp   Content Browser integration + Reimport menu
```

### Storage model

`Files[]` is `TArray<FInoWebBundleFile>`. Each entry holds
`RelativePath` (POSIX-style) and `Bytes` (uncompressed — the `.pak` layer
compresses). `ContentHash` is MD5 hex over the sorted `(path, bytes)`
tuples. No compression inside the asset itself.

---

## Adding features

### A new simple option (user agent, context menus, etc.)
1. Add field to `FInoWebViewConfig` in `InoWebUITypes.h`.
2. Use it in `FInoWebViewImpl_Windows::OnControllerReady` when configuring
   the controller.

### A new runtime operation (e.g., `ExecuteJavaScript`)
1. Add pure virtual method to `IInoWebViewImpl`.
2. Implement in **all three** platform impls:
   - `FInoWebViewImpl_Windows` (child-HWND, used in standalone + packaged)
   - `FInoWebViewImpl_Windows_Composition` (DirectComposition, used in PIE)
   - `FInoWebViewImpl_Android` (JNI → Java helper)
   If it can be called before ready, add a pending-ops slot in `FInternal`
   on each Windows impl and replay in `ApplyPendingOperations()`. On
   Android the Java side serializes via `runOnUiThread`, so an early-return
   on `bDestroyed` is usually enough.
3. Add thin wrapper on `UInoWebView` as `UFUNCTION(BlueprintCallable, ...)`.

### A new event / BP delegate (e.g., `OnConsoleMessage`)
1. Add a `TFunction<void(...)>` callback slot to `IInoWebViewImpl` — the
   plain `OnXxxCallback` shape (NOT pure virtual). The impl populates it
   from the native event source.
2. Add a `DECLARE_DYNAMIC_MULTICAST_DELEGATE[_NParams]` typedef in
   `InoWebView.h` and a `UPROPERTY(BlueprintAssignable)` member.
3. **Wire it in `UInoWebView::WireImplCallbacks`** — NOT inline in `Init`.
   The wiring is extracted into that helper precisely so the auto-recover
   path (`RecreateImpl`) can re-bind callbacks on the fresh impl without
   duplicating the wiring code. Forgetting this means the new event fires
   correctly on the original WebView but stops firing after a renderer
   process recovery.
4. Per platform:
   - **Windows**: capture the event in `OnControllerReady` (or wherever the
     event source lives), marshal to the game thread if needed, call the
     stored TFunction.
   - **Android**: add a new `nativeOnXxx` JNI export that uses
     `DispatchOnGameThread` + the registry lock, then a corresponding
     Java-side hook (typically inside `InoWebChromeClient` or
     `InoWebViewClient`).
   - **iOS**: same pattern via the Obj-C++ delegate methods, marshaling
     back to game thread with `AsyncTask`.

### A new bit of injected JS
1. Add (or edit) a file under `Source/InoWebUI/JS/`.
2. Run `Plugins/InoWebUI/Scripts/GenerateJSConstants.ps1`.
3. Reference the generated constant — `GInoWebUI<Name>Script` from the
   C++ side, `InoWebUIScripts.<NAME>_JS` from Java.
Don't edit the inline copies in `InoWebViewImpl_Windows.cpp`,
`InoWebViewImpl_Windows_Composition.cpp`, or `InoWebViewAndroid.java`
directly — those are generated and will be overwritten.

### A new platform (macOS, Android)
1. New folder under `Private/Impl/<Platform>/` with a new `FInoWebViewImpl_<Platform>`.
2. Update `InoWebViewFactory.cpp` with another `#elif PLATFORM_<X>` branch.
3. Update `InoWebUI.Build.cs` with platform-specific third-party setup.

---

## Roadmap

| Phase | Scope | Status |
|---|---|---|
| 1 | Overlay: create, load URL, show/hide, resize tracking | ✔ done |
| 2 | Two-way messaging (`PostMessage`, `OnMessageReceived`, `window.InoWebUI`) | ✔ done |
| 3 | DevTools / ExecuteJS / mute / context-menu & accelerator toggles / UA override | ✔ done |
| 4 | Virtual-host mapping (serve local content as `https://`) | ✔ done |
| 5 | Game-UI hardening: lockdown + dialog/popup blocking + nav events + zoom/focus/cookies/crash | ✔ done |
| 6 | Android MVP — overlay + lifecycle + URL/show/hide/reload | ✔ done |
| 7 | UInoWebBundle asset — bundle web content into a UE asset, extract on demand | ✔ done |
| 8 | Android parity pass — messaging, hardening, virtual host, runtime polish | ✔ done |
| 9 | DirectComposition hosting (fixes PIE transparency on Windows) | ✔ done |
| 10 | JS source dedup — single `bridge.js` / `dev_overlay.js` + build-time codegen | ✔ done |
| 11 | Bridge hardening — `once`, iteration safety, `Object.create(null)`, U+2028 fix | ✔ done |
| 12 | Browser API completeness — back/forward, state getters, capture, headers, sub-region bounds | ✔ done |
| 13 | iOS implementation (`WKWebView`) | ✔ done |
| 14 | Android docs-review hardening — UTF-16 JNI, `PixelCopy` capture, `ExceptionCheck` discipline, renderer priority, AndroidX webkit 1.16, cleartext URL warning | ✔ done |
| 15 | Auto-recover on renderer process failure — `onRenderProcessGone` → detach + recreate w/ same Config + reload to snapshotted URL, retry budget (3 per 60s) | ✔ done |
| 16 | Hung-renderer detection + auto-terminate — `WebViewRenderProcessClient`, `OnRenderProcessUnresponsive` / `Responsive` BP delegates, `UnresponsiveTimeoutMs` config | ✔ done |
| 17 | JS bridge migration to `addWebMessageListener` — origin allowlist, `JavaScriptReplyProxy` for UE→JS, legacy `addJavascriptInterface` fallback for pre-2020 System WebView | ✔ done |
| 18 | Console capture + explicit security defaults — `OnConsoleMessage` BP delegate + `LogInoWebUI` mirror, `bAllowMixedContent` / `bAllowFileURLs` / `bAllowContentURIs` defaults off | ✔ done |
| 19 | iOS hardening — bridge into `WKContentWorld.defaultClientWorld` (page can't intercept `_InoWebUIHost`) + `pageWorld` shim via DOM `CustomEvent` so `window.InoWebUI` stays page-accessible, native `WKWebView.pageZoom` (iOS 14+) replacing the old "not supported" log, `WKWebView.inspectable` (iOS 16.4+) gated on `bEnableDevTools` so Safari Web Inspector attaches to TestFlight / Release builds | ✔ done |
| 20 | iOS preferences modernization — `decidePolicyForNavigationAction:preferences:decisionHandler:` (iOS 13+) replaces legacy 2-arg variant, with per-nav `allowsContentJavaScript` from `Config.bAllowJavaScript`. `WKPreferences` hardening defaults (`fraudulentWebsiteWarningEnabled` explicit, `isElementFullscreenEnabled` / `isTextInteractionEnabled` gated on Settings). `WKWebViewConfiguration.upgradeKnownHostsToHTTPS` (iOS 15+) as inline HSTS-style hardening. Additive `applicationNameForUserAgent` (preserves WebKit version in UA) alongside the full-replace `UserAgentOverride`. `WKWebView.themeColor` KVO → new `FOnInoWebThemeColorChanged` BP delegate. `WKWebView.underPageBackgroundColor` synced to background opacity at create + `SetBackgroundOpaque` time so the overscroll gutter never leaks system grey through a transparent overlay. | ✔ done |
| 21 | macOS implementation (`WKWebView`) | — |

Don't stub future phases — add them when they're needed.

---

## Common pitfalls

- **"WebView works in editor, invisible in packaged build"** → exclusive
  fullscreen. Force borderless windowed.
- **"WebView2 Runtime not found" log** → install Edge / the Evergreen
  WebView2 Runtime. Almost always present on Win10 1803+ and all Win11.
- **Crash on editor shutdown / live coding reload** → a WebView outlived the
  parent HWND. Verify `Shutdown()` is being called. `UInoWebView::BeginDestroy`
  covers orderly cases; disorderly teardown needs subsystem `Deinitialize`
  to run first.
- **Bounds are wrong after DPI change** → `FViewport::ViewportResizedEvent`
  fires on DPI changes; make sure the subsystem's subscription is live.
- **Pre-ready calls seem to "disappear"** → they didn't; they're queued.
  Check `FInternal::PendingNavigate/Visible/Bounds` if you suspect replay
  isn't happening.
- **Bundle extracts but page fails to load with `ERR_INVALID_RESPONSE`
  (Android) or `0x80070003` / path-not-found (Windows, packaged)** →
  path resolution mismatch between UE's virtual "`../../../Project/...`"
  form and what native code (JNI / WebView2) can open.
  `FPaths::ConvertRelativePathToFull` does NOT fully resolve these on
  Android. Use
  `IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(...)`
  whenever you hand a path to a non-UE loader. Already done for the
  bundle extract dir in `UInoWebUISubsystem::ResolveBundleContentFolder`.

- **Android WebView covers ~1/3 of the screen instead of fullscreen** →
  UE's `SWindow::GetClientRectInScreen` reports in a coord system that
  doesn't match Android `FrameLayout.LayoutParams`' physical-pixel
  contract. In auto-bounds mode (the default), `InoWebViewAndroid.syncBounds`
  ignores the incoming values and forces `MATCH_PARENT`. In manual mode
  (after `UInoWebView::SetBounds`), the Java side scales UE-pixel
  coords by the activity's display density to get correct physical-pixel
  layout params + margins.

- **Transparent WebView in PIE** → historically this leaked the desktop
  through "empty" areas because PIE uses Slate-chromed windows with
  `DWMWA_NCRENDERING_POLICY = DWMNCRP_DISABLED` plus a rounded-rect
  `SetWindowRgn`, under which DWM composited transparent child-HWND pixels
  against the desktop instead of the parent's swap chain. **Fixed in
  Phase 9:** the factory now auto-selects `FInoWebViewImpl_Windows_Composition`
  (which uses `CreateCoreWebView2CompositionController` + DirectComposition
  visual hosting) whenever running under the editor (`GIsEditor`, i.e. PIE),
  and the child-HWND impl for Standalone / packaged builds. If you ever see
  desktop bleed-through again, confirm the factory is picking the composition
  impl for the editor branch (`InoWebViewFactory.cpp`).

---

## Debugging

- Log category: `LogInoWebUI`. Set to `Verbose` in `Engine.ini` for trace:
  ```ini
  [Core.Log]
  LogInoWebUI=Verbose
  ```
- The self-contained showcase / test harness at `Content/web/index.html`
  auto-detects the bridge and reports its status; point a `UInoWebBundle`
  or `VirtualHostFolder` at `Content/web/` and load `index.html`.
- For HTML-side issues, enable `View.bEnableDevTools` and use the floating
  dev-overlay (or F12 on Windows when `View.bEnableAcceleratorKeys` is on).
