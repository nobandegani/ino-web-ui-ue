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

**Current status: Phase 3 (Win64 only — overlay + two-way messaging + runtime polish).**

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
| 4 | Pluggable local-content server (so shipped builds don't need `file://`) | — |
| 5 | DirectComposition-based hosting (fixes PIE transparency) | — |
| 6 | macOS implementation (`WKWebView`) | — |
| 7 | Android implementation (`android.webkit.WebView`) | — |

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
