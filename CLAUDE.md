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

**Current status: Phase 1 (Win64 only, no messaging).**

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
│       │   └── InoWebView.h                      UObject handle API
│       └── Private/
│           ├── InoWebUI.cpp
│           ├── InoWebUISubsystem.cpp
│           ├── InoWebView.cpp
│           └── Impl/
│               ├── IInoWebViewImpl.h
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

## Roadmap (not in Phase 1)

| Phase | Scope |
|---|---|
| 2 | Two-way messaging (`PostMessage`, `OnMessageReceived` delegate, JS bridge injection) |
| 3 | DevTools toggle, UserAgent override, context-menu & accelerator toggles |
| 4 | Pluggable local-content server (so shipped builds don't need `file://`) |
| 5 | macOS implementation (`WKWebView`) |
| 6 | Android implementation (`android.webkit.WebView`) |

None of these exist yet. Don't stub them in — add them when they're needed.

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
