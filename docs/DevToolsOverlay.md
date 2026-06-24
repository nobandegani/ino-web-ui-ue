# Dev-tools floating overlay

When `FInoWebViewConfig::bEnableDevTools = true`, InoWebUI injects a
small in-page UI: a circular gear button in the bottom-right corner
that, when clicked, expands a vertical toolbar of four actions. An
always-visible **FPS chip** is docked just left of the gear button — it
measures the WebView layer's own frame rate (UI jank), independent of
Unreal's render thread, and colour-codes green / amber / red.

The overlay is injected at document-start on every page load — Windows
via `AddScriptToExecuteOnDocumentCreated`, Android via AndroidX
`WebViewCompat.addDocumentStartJavaScript` (falling back to
`WebViewClient.onPageStarted` only on pre-2020 System WebView), iOS via
`WKUserScript` at `WKUserScriptInjectionTimeAtDocumentStart`.

## Button reference

| Icon | Name | Action | Routing |
|---|---|---|---|
| refresh | Refresh | Reload the page | JS -> `_devtools.refresh` -> `Impl::Reload` |
| devtools | DevTools | Open the Chromium DevTools panel (Windows) / log the `chrome://inspect` hint (Android) / log the Safari Web Inspector hint (iOS) | JS -> `_devtools.openDevTools` -> `Impl::OpenDevTools` |
| info | Info | Modal showing URL, title, platform, viewport / screen size, device pixel ratio, language, user-agent, bridge status | JS-only, no UE round-trip |
| devcallback | Dev Callback | Fires the `OnDevCallback` BP delegate — wire this to whatever project-specific dev trigger you like (cheat menu, spawn enemy, reload level, ...) | JS -> `_devtools.devCallback` -> `FOnInoWebDevCallback` broadcast |

## Channel routing

The `_devtools.*` channels are intercepted inside
`UInoWebView::DispatchIncomingEnvelope` **before** the user's
`OnMessageReceived` delegate runs. Your user code never sees them, and
choosing a channel named `_devtools.something` in your own messages is
unsupported (it will be swallowed).

## Where the JS lives

The single source of truth is
[`Source/InoWebUI/JS/dev_overlay.js`](../Source/InoWebUI/JS/dev_overlay.js).
`Scripts/GenerateJSConstants.ps1` reads it and emits two generated
files that the platform impls consume:

- `Source/InoWebUI/Private/Generated/InoWebUIScripts.generated.h` →
  C++ constant `GInoWebUIDevToolsOverlayScript`, used by both Windows
  impls (`InoWebViewImpl_Windows.cpp` and `InoWebViewImpl_Windows_Composition.cpp`).
- `Source/InoWebUI/Java/src/net/inoland/webui/InoWebUIScripts.java` →
  Java constant `InoWebUIScripts.DEVTOOLS_OVERLAY_JS`, registered via
  `WebViewCompat.addDocumentStartJavaScript` (or `onPageStarted` on the
  legacy fallback path).
- `Source/InoWebUI/Private/Generated/InoWebUIScripts_iOS.generated.h` →
  Obj-C++ `NSString*` constant, injected as a `WKUserScript` by the iOS
  impl.

Workflow when you change the overlay:

1. Edit `Source/InoWebUI/JS/dev_overlay.js`.
2. Run `Plugins/InoWebUI/Scripts/GenerateJSConstants.ps1`.
3. Commit both the source `.js` and the regenerated outputs.

The generator auto-chunks the C++ output under MSVC's 16380-character
single-string-literal limit (adjacent `TEXT(R"JS(...)JS")` chunks; the
preprocessor concatenates them at compile time), so the overlay can
grow without manual splitting.

## Disabling at ship time

For a shipping build, flip `bEnableDevTools = false` in your view
config — the overlay is injection is gated on the same flag that gates
the native DevTools panel, so both disappear together.
