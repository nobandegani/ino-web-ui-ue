# Dev-tools floating overlay

When `FInoWebViewConfig::bEnableDevTools = true`, InoWebUI injects a
small in-page UI: a circular gear button in the bottom-right corner
that, when clicked, fans out seven actions on a quarter-circle arc.

The overlay is injected on every page load — Windows via
`AddScriptToExecuteOnDocumentCreated`, Android via
`WebViewClient.onPageStarted`.

## Button reference

| Icon | Name | Action | Routing |
|---|---|---|---|
| refresh | Refresh | Reload the page | JS -> `_devtools.refresh` -> `Impl::Reload` |
| devtools | DevTools | Open the Chromium DevTools panel (Windows) / show the `chrome://inspect` hint (Android) | JS -> `_devtools.openDevTools` -> `Impl::OpenDevTools` |
| clear | Clear Data | `ClearAllCookies` | JS -> `_devtools.clearData` -> `Impl::ClearAllCookies` |
| info | Info | Modal showing URL, platform, viewport size, user-agent, bridge status | JS-only, no UE round-trip |
| transparency | Transparency | Flip between transparent and opaque white backgrounds; the plugin tracks state so the button reflects the current mode | JS -> `_devtools.toggleTransparency` -> `Impl::SetBackgroundOpaque` |
| hide | Hide WebUI | Calls `Hide()` — you own the re-show trigger | JS -> `_devtools.hideWebUI` -> `UInoWebView::Hide` |
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
  Java constant `InoWebUIScripts.DEVTOOLS_OVERLAY_JS`, used by
  `InoWebViewAndroid.onPageStarted`.

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
