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

**Current status: Phase 8 — Win64 (full) + Android (full parity) + Web Bundle asset type.**

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
    └── FInoWebViewImpl_Windows                  WebView2 COM impl
```

**Why pimpl?** WebView2.h drags Windows.h, wrl.h, dozens of COM headers. The
pimpl pattern keeps them inside exactly **one** .cpp file
(`InoWebViewImpl_Windows.cpp`). Nothing else in the plugin pays that compile
cost, and adding macOS/Android later doesn't leak platform types into the
UObject headers.

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
├── Java/src/com/inoksan/webui/
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

### Feature matrix (Phase 8 — full parity with Windows)

| API | Android | Notes |
|---|---|---|
| Create / Destroy | ✔ | |
| Navigate / Reload / Show / Hide | ✔ | |
| SyncBounds (margins + size) | ✔ | |
| Transparent background | ✔ | `bTransparentBackground` |
| Activity lifecycle hooks | ✔ | Pause / Resume / Destroy via UPL |
| **Virtual-host mapping** | ✔ | `WebViewClient.shouldInterceptRequest` serves `https://<host>/*` from a local folder |
| **Web Bundle assets** | ✔ | Runtime logic is cross-platform; works identically |
| **Two-way messaging** | ✔ | `addJavascriptInterface` + `evaluateJavascript`; same `window.InoWebUI` API as Windows |
| **Navigation events** | ✔ | `OnNavigationStarting` / `OnNavigationCompleted` / `OnDocumentTitleChanged` |
| **Lockdown** | ✔ | `shouldOverrideUrlLoading` returns true for non-whitelisted URIs; same wildcard rules |
| **JS dialog suppression** | ✔ | `WebChromeClient.onJs{Alert,Confirm,Prompt,BeforeUnload}` |
| **window.open blocking** | ✔ | `onCreateWindow` with transport-WebView trick to capture URL |
| **Focus events + FocusWebView** | ✔ | `setOnFocusChangeListener` + `requestFocus` |
| **SetZoomFactor** | ✔ | `setInitialScale(percent)` |
| **ClearAllCookies** | ✔ | `CookieManager.removeAllCookies` |
| **OnProcessFailed** | ✔ | `WebViewClient.onRenderProcessGone` (API 26+) |
| **DevTools (remote)** | ✔ | `setWebContentsDebuggingEnabled` — inspect via `chrome://inspect/#devices` on desktop Chrome |
| **ExecuteJavaScript** | ✔ | `webView.evaluateJavascript` |
| **UserAgentOverride** | ✔ | `WebSettings.setUserAgentString` |
| **bEnableContextMenus** | ✔ | `setOnLongClickListener` suppresses the browser context menu |
| `OpenDevTools` (programmatic) | — | Android has no in-process API; remote inspect only (log explains) |
| `SetMuted` / `bStartMuted` | — | `android.webkit.WebView` has no audio mute; log warns |
| `bEnableAcceleratorKeys` | N/A | F5/F12/Ctrl+F are desktop-only concepts |

### Android runtime architecture

One `InoWebViewClient` (handles `shouldInterceptRequest` for virtual host,
`shouldOverrideUrlLoading` for lockdown, `onPageStarted` for JS bridge
injection, `onPageFinished` / `onReceivedError` for nav completion,
`onRenderProcessGone` for crash) plus one `InoWebChromeClient` (handles
title changes, JS dialogs, `onCreateWindow`). Both are installed once in
`createWebView` and read per-WebView state from a `sConfigs:
SparseArray<Config>` that the various `configureXxx` JNI methods
populate between `createWebView` and the first `loadURL`.

JS bridge is the same `window.InoWebUI.send/on/off` API as Windows.
`window.chrome.webview.postMessage` doesn't exist on Android; the
injected bridge routes through `addJavascriptInterface` instead. User
JS is identical across platforms.

Events from Java to C++ go through six `nativeOn...` JNI exports
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
  2. Java file got copied — check `Intermediate/Android/APK/src/com/inoksan/webui/`
  3. ProGuard isn't stripping the class (our `-keep` rule should prevent this)

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
│   └── webview_test.html                         manual test harness
├── Source/
│   ├── ThirdParty/WebView2/                      headers + static lib
│   │   ├── include/{WebView2.h, WebView2EnvironmentOptions.h}
│   │   ├── lib/Win64/WebView2LoaderStatic.lib
│   │   └── VERSION                               SDK version stamp
│   └── InoWebUI/
│       ├── InoWebUI.Build.cs                     links static loader + system libs
│       ├── Public/
│       │   ├── InoWebUI.h                        module
│       │   ├── InoWebUILog.h                     LogInoWebUI category
│       │   ├── InoWebUITypes.h                   FInoWebViewConfig
│       │   ├── InoWebUISubsystem.h               UGameInstanceSubsystem API
│       │   ├── InoWebView.h                      UObject handle API
│       │   └── IInoWebViewImpl.h                 pimpl contract (no platform headers)
│       └── Private/
│           ├── InoWebUI.cpp
│           ├── InoWebUISubsystem.cpp
│           ├── InoWebView.cpp
│           └── Impl/
│               ├── InoWebViewFactory.cpp         platform-dispatches
│               └── Windows/
│                   ├── InoWebViewImpl_Windows.h
│                   └── InoWebViewImpl_Windows.cpp
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
launch. Required system libs: `shlwapi.lib`, `version.lib`, `ole32.lib`.

If the WebView2 Runtime is missing (rare on Win10+), `Initialize()` logs a
clear error pointing to the Evergreen installer rather than crashing.

---

## Usage (Phase 1)

```cpp
// C++ — typical BeginPlay:
UInoWebUISubsystem* WebUI = GetGameInstance()->GetSubsystem<UInoWebUISubsystem>();

FInoWebViewConfig Config;
Config.InitialURL            = TEXT("http://localhost:5173");
Config.bTransparentBackground = true;
Config.bVisibleOnCreate       = true;

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
| `ClearAllCookies()`      | Delete all cookies in this WebView's isolated profile. Useful for logout. |

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
2. Implement in `FInoWebViewImpl_Windows`. If it can be called before ready,
   add a pending-ops slot in `FInternal` and replay in
   `ApplyPendingOperations()`.
3. Add thin wrapper on `UInoWebView` as `UFUNCTION(BlueprintCallable, ...)`.

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
| 9 | DirectComposition hosting (fixes PIE transparency on Windows) | — |
| 10 | macOS implementation (`WKWebView`) | — |

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
- **Transparent WebView shows DESKTOP through "empty" areas in PIE (but
  works fine in standalone)** → known composition limitation. PIE uses
  Slate-chromed windows with `DWMWA_NCRENDERING_POLICY = DWMNCRP_DISABLED`
  plus a rounded-rect `SetWindowRgn`. Under that combination, DWM
  composites transparent child-HWND pixels against the desktop instead of
  the parent's swap chain. Standalone Game and shipped builds use OS chrome
  and work correctly. Workarounds:
    1. Use **Standalone Game** play mode when visually testing the overlay.
    2. Set `bTransparentBackground = false` during PIE iteration (opaque
       WebView; messaging/bounds pipeline still fully testable).
  A proper fix would require switching from `CreateCoreWebView2Controller`
  (child HWND) to `CreateCoreWebView2CompositionController` with
  DirectComposition visual hosting — a meaningful chunk of new code; left
  as a future phase unless needed.

---

## Debugging

- Log category: `LogInoWebUI`. Set to `Verbose` in `Engine.ini` for trace:
  ```ini
  [Core.Log]
  LogInoWebUI=Verbose
  ```
- The test harness at `Content/webview_test.html` auto-detects WebView2 and
  reports bridge status in its footer.
- For HTML-side issues, DevTools is disabled by default in Phase 1. When
  enabled in a later phase, it opens on F12.
