# Architecture

InoWebUI is two layers, deliberately separated.

```
UObject / Blueprint layer  (platform-agnostic, never sees Windows.h)
--------------------------
UInoWebUISubsystem      UGameInstanceSubsystem      creates and tracks views
UInoWebView             UObject                     one overlay handle
        |
        | holds TUniquePtr<IInoWebViewImpl>
        |
Native implementation layer  (platform-specific)
--------------------------
IInoWebViewImpl                                   pure virtual interface
   |-- FInoWebViewImpl_Windows                    WebView2 + COM
   `-- FInoWebViewImpl_Android                    JNI + android.webkit.WebView
```

## Why a pimpl split

`WebView2.h` drags in `Windows.h`, `wrl.h`, and a fleet of COM headers.
The pimpl pattern keeps them inside exactly **one** `.cpp` file
(`InoWebViewImpl_Windows.cpp`). Nothing else in the plugin pays that
compile cost, and adding macOS / iOS later will not leak platform types
into the UObject headers.

`IInoWebViewImpl.h` is intentionally a **public** header even though it
is morally "internal," because `UInoWebView` holds a
`TUniquePtr<IInoWebViewImpl>` and UHT-generated code
(`UInoWebView.gen.cpp`'s FVTableHelper) needs the complete type at
compile time. The header itself contains zero platform code — the
platform code hides in `Private/Impl/<Platform>/`.

## The four rules

1. **Game thread only.** All public methods on `UInoWebView` /
   `UInoWebUISubsystem` assert `check(IsInGameThread())`. WebView2
   callbacks fire back on the thread that created the environment
   (the game thread). Do not cross threads.

2. **Async-safe.** WebView2 construction is a two-step async operation.
   Any call (`Navigate`, `SetVisible`, `SyncBounds`, `Reload`,
   `ExecuteJavaScript`, `SetMuted`, `PostMessage`, ...) issued before
   the controller is ready is **queued** inside the impl and replayed
   from `ApplyPendingOperations()` when the controller signals ready.
   If you add a new operation, extend the queue.

3. **Lifetime tokens.** Async callbacks capture a `TWeakPtr<int>` to
   the impl's `LifetimeToken`. `Shutdown()` resets the token, so any
   still-in-flight callback sees an invalid weak pointer and silently
   no-ops. Never capture raw `this` in a WebView2 callback without
   this guard.

4. **Shutdown order is fixed.** The controller must be `Close()`'d
   **while the parent HWND still exists**. Order:
   `LifetimeToken.Reset()` -> `Controller->Close()` -> release
   ComPtrs. Changing the order leaks COM objects.

## Message flow

```
Page JS  ->  window.InoWebUI.send(channel, payload)
             |
             v  (Windows) window.chrome.webview.postMessage(JSON)
             v  (Android) addJavascriptInterface bridge
             |
         Impl OnWebMessageReceived
             |
             v
         UInoWebView::DispatchIncomingEnvelope
             |        (intercepts built-in _devtools.* channels)
             |
             v
         OnMessageReceived BP multicast   ->   your game code
```

```
UE game code   ->   UInoWebView::PostMessage(channel, payload)
                    |
                    v
                Impl->PostWebMessageAsJson (serialised envelope)
                    |
                    v
                page JS: window.InoWebUI invokes registered handlers
```

## File layout

```
Plugins/InoWebUI/
|-- InoWebUI.uplugin
|-- LICENSE
|-- README.md
|-- CLAUDE.md                                              (dev notes)
|-- Scripts/
|   `-- AcquireWebView2SDK.ps1                             WebView2 from NuGet
|-- Content/
|   `-- web/                                               test harness
|-- Source/
|   |-- ThirdParty/WebView2/                               SDK drop
|   |   |-- include/{WebView2.h, WebView2EnvironmentOptions.h}
|   |   |-- lib/Win64/WebView2LoaderStatic.lib
|   |   `-- VERSION                                        version stamp
|   |-- InoWebUI/                                          runtime module
|   |   |-- InoWebUI.Build.cs
|   |   |-- InoWebUI_UPL.xml                               Android UPL
|   |   |-- Java/src/com/inoksan/webui/InoWebViewAndroid.java
|   |   |-- Public/
|   |   |   |-- InoWebUI.h
|   |   |   |-- InoWebUILog.h
|   |   |   |-- InoWebUITypes.h
|   |   |   |-- InoWebUISubsystem.h
|   |   |   |-- InoWebView.h
|   |   |   |-- IInoWebViewImpl.h
|   |   |   `-- InoWebBundle.h
|   |   `-- Private/
|   |       |-- InoWebUI.cpp
|   |       |-- InoWebUISubsystem.cpp
|   |       |-- InoWebView.cpp
|   |       |-- InoWebBundle.cpp
|   |       `-- Impl/
|   |           |-- InoWebViewFactory.cpp                  platform dispatch
|   |           |-- Windows/
|   |           |   |-- InoWebViewImpl_Windows.h
|   |           |   `-- InoWebViewImpl_Windows.cpp
|   |           `-- Android/
|   |               |-- InoWebViewImpl_Android.h
|   |               `-- InoWebViewImpl_Android.cpp
|   `-- InoWebUIEditor/                                    editor-only module
|       `-- Private/
|           |-- InoWebBundleFactory.cpp
|           `-- InoWebBundleActions.cpp
`-- docs/
```

## Naming convention

Every public type in this plugin starts with **`Ino`** — no exceptions.
That includes structs, enums, UObject classes, and plain C++ classes
exposed from a public header. Keeps symbol collisions out of the
project and makes grep-driven navigation trivial.

## Adding a feature

### A new config option

1. Add the field to `FInoWebViewConfig` in `InoWebUITypes.h`.
2. Consume it in `FInoWebViewImpl_<Platform>::OnControllerReady`.
3. If it should be Blueprint-visible, tag the field `UPROPERTY(EditAnywhere, BlueprintReadWrite)`.

### A new runtime operation (e.g. `ExecuteJavaScript`)

1. Add a pure virtual method to `IInoWebViewImpl`.
2. Implement it in every `FInoWebViewImpl_<Platform>`.
3. If it may be called before ready, add a pending slot in
   `FInternal`/equivalent and replay it from `ApplyPendingOperations()`.
4. Add the thin `UFUNCTION(BlueprintCallable)` wrapper on
   `UInoWebView`.

### A new platform

1. Create `Private/Impl/<Platform>/FInoWebViewImpl_<Platform>.{h,cpp}`.
2. Add the `#elif PLATFORM_<X>` branch to `InoWebViewFactory.cpp`.
3. Add the platform's third-party setup to `InoWebUI.Build.cs`.
