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

The raw string literal is duplicated in two places — one per platform:

- Windows: `InoWebViewImpl_Windows.cpp`,
  symbol `GInoWebUIDevToolsOverlayScript`.
- Android: `InoWebViewAndroid.java`,
  constant `DEVTOOLS_OVERLAY_JS`.

A comment at each copy tells you to update both if you modify the JS.

MSVC has a 16380-character limit on a single string literal, so the
Windows copy is split into two adjacent `TEXT(R"JS(...)JS")` chunks —
the preprocessor concatenates them at compile time. Keep that
arrangement if the overlay grows.

## Disabling at ship time

For a shipping build, flip `bEnableDevTools = false` in your view
config — the overlay is injection is gated on the same flag that gates
the native DevTools panel, so both disappear together.
