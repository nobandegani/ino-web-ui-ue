# Game-UI hardening

The defaults on `FInoWebViewConfig` are tuned for "locked-down game UI":
no alerts, no popups, no stray navigations, no browser shortcuts
stealing F5 from your game. Every toggle described here is individually
flippable for dev ergonomics.

## Navigation lockdown

```cpp
bLockToVirtualHost   = true                         // locked by default
AllowedURIPatterns   = []                           // UE wildcard list
```

A URI is allowed to load if any of the following holds:

1. It uses a safe internal scheme: `about:`, `data:`, `blob:`.
2. `bLockToVirtualHost == false`.
3. The host matches `VirtualHostName` (whole-host, case-insensitive).
4. The full URI matches any entry in `AllowedURIPatterns` via
   `FString::MatchesWildcard`.

Anything else is cancelled with a warning log — useful as defence
against accidental navigation away from your UI (bad links, injected
third-party scripts, auth redirects gone wrong).

### Pattern examples

```
https://*.api.company.com/*      subdomains of your API
http://localhost:*/*             any localhost port (dev only)
https://cdn.jsdelivr.net/*       a specific CDN
```

## Script-dialog, popup, context-menu, accelerator toggles

| Field | Default | Behaviour when off (default) |
|---|---|---|
| `bAllowScriptDialogs`    | `false` | JS `alert()` / `confirm()` / `prompt()` / `onbeforeunload` are suppressed. `confirm` returns `false`, `prompt` returns `null`, `alert` fires no dialog. The `OnScriptDialog` delegate still broadcasts so you can log them. |
| `bAllowNewWindows`       | `false` | `window.open()` and `target="_blank"` are blocked — no stray Chromium popup ever appears over the game. `OnNewWindowRequested` fires so your BP can `LoadURL(URI)` to redirect the current frame instead. |
| `bEnableContextMenus`    | `false` | Right-click is a no-op inside the WebView. |
| `bEnableAcceleratorKeys` | `false` | F5, F12, Ctrl+F, Ctrl+P, ... pass through to the game. |

### Per-platform implementation notes

- **Windows.** Lockdown applied through `ICoreWebView2.NavigationStarting` (cancel + log). Dialog suppression through `ScriptDialogOpening`. `window.open` through `NewWindowRequested.put_Handled(TRUE)`.
- **Android.** Lockdown applied through `WebViewClient.shouldOverrideUrlLoading`. Dialog suppression through the `WebChromeClient.onJsAlert/Confirm/Prompt/BeforeUnload` callbacks calling `result.cancel()`. `window.open` through `WebChromeClient.onCreateWindow` plus the transport-WebView trick to extract the URL.
- **iOS.** Lockdown applied through `WKNavigationDelegate.decidePolicyForNavigationAction:` (`.cancel`). Dialog suppression through `WKUIDelegate.runJavaScriptAlert/Confirm/TextInputPanel...:` calling completion immediately with cancel/null. `window.open` through `WKUIDelegate.createWebViewWithConfiguration:...` returning `nil`. The internal-scheme allowlist on iOS extends to the custom `inoweb:` scheme used for the virtual host (see `iOS.md`).

## Navigation events (BP delegates on `UInoWebView`)

| Delegate | Signature | Fires when |
|---|---|---|
| `OnReady`                  | (no params)                                    | Exactly once, on a tick after `CreateWebView` returned, regardless of whether the underlying construction was async. Bind immediately after create; fall back to `IsReady()` if you bind too late. |
| `OnNavigationStarting`     | `(URI: String)`                                | Before every navigation. Observation only — lockdown handles cancellation internally. |
| `OnNavigationCompleted`    | `(bSuccess: bool, URI: String)`                | Page finished loading, or failed. Good hook for splash-screen dismissal and loading indicators. |
| `OnDocumentTitleChanged`   | `(Title: String)`                              | Page called `document.title = ...`. |
| `OnScriptDialog`           | `(Kind: EInoScriptDialogKind, Message: String)` | Any alert / confirm / prompt / beforeunload. |
| `OnNewWindowRequested`     | `(URI: String)`                                | `window.open` / `_blank`. Blocked by default. |
| `OnGotFocus` / `OnLostFocus` | (no params)                                  | WebView gained / lost keyboard focus. |
| `OnProcessFailed`          | `(Description: String)`                        | Renderer subprocess crashed / was OS-killed. With `bAutoRecoverOnProcessFailed` (default true) the plugin recreates the view and reloads automatically after this fires. |
| `OnRenderProcessUnresponsive` / `OnRenderProcessResponsive` | `(URI: String)` | Renderer hung (~5s no event-loop response) / recovered. **Android only.** If `UnresponsiveTimeoutMs > 0` (default 10000) a hung renderer is auto-terminated, which routes through the `OnProcessFailed` recovery path. |
| `OnConsoleMessage`         | `(Level, Message, SourceID, Line)`             | Page `console.*` call. **Android only** (also mirrored to `LogInoWebUI`). WebView2 / WKWebView have no equivalent event. |
| `OnThemeColorChanged`      | `(Color: LinearColor)`                         | Page `<meta name="theme-color">` changed. **iOS only** (`WKWebView.themeColor` KVO). |
| `OnCapturePreviewComplete` | `(bSuccess: bool, FilePath: String)`           | A `CapturePreview()` screenshot finished writing to disk. |

## Runtime methods (BP-callable on `UInoWebView`)

| Method | Purpose |
|---|---|
| `FocusWebView()`           | Move keyboard focus into the WebView — needed before HTML `<input>` can type. |
| `SetZoomFactor(Factor)`    | 1.0 = 100%, 1.5 = 150%. |
| `GetZoomFactor()`          | Current zoom, returns 1.0 if not ready. |
| `ClearAllCookies()`        | Delete all cookies in this view's isolated profile. Useful for logout. |
| `ExecuteJavaScript(Code)`  | Run arbitrary JS in the page. Fire-and-forget — if you need a result, have the JS `window.InoWebUI.send(...)` instead. Queued if pre-ready. |
| `SetMuted(bool)`           | Mute / unmute audio. Windows only — Android `WebView` and iOS `WKWebView` have no native audio-mute API, so the call logs a warning and does nothing on those platforms (mute from your own page JS instead). |
| `OpenDevTools()`           | Open the Chromium DevTools panel (Windows). On Android the same call logs the `chrome://inspect/#devices` instructions — see `Android.md`. |

## Config toggles (applied once at `CreateWebView`)

| Field | Default | Effect |
|---|---|---|
| `View.bEnableDevTools`     | `false` | Enables DevTools. F12 opens it if `bEnableAcceleratorKeys` is also true; otherwise call `OpenDevTools()`. Also gates the floating dev-tools overlay (see `DevToolsOverlay.md`). |
| `View.bEnableContextMenus` | `false` | Right-click browser context menu (Windows / Android; not enforced on iOS). |
| `View.bEnableAcceleratorKeys` | `false` | Browser shortcuts (F5, F12, Ctrl+F, Ctrl+P, ...). Windows-only. |
| `View.bStartMuted`         | `false` | Audio muted on creation (Windows; no-op on Android / iOS). |
| `View.UserAgentOverride`   | `""`    | Replaces `navigator.userAgent`. (iOS also has `View.ApplicationName` for an *additive* UA tag that keeps the WebKit version.) |
| `bAutoRecoverOnProcessFailed` | `true` | Recreate + reload the view automatically when the renderer dies (retry budget: 3 per 60s). |
| `UnresponsiveTimeoutMs`    | `10000` | Auto-terminate a hung renderer after this many ms (0 = observe only). **Android only.** |
| `bAllowMixedContent` / `bAllowFileURLs` / `bAllowContentURIs` | `false` | Explicit Android security defaults — block `http` subresources on `https`, `file://` URLs, and `content://` URIs respectively. |
| `bUpgradeHTTPToHTTPS`      | `false` | Inline HSTS-style upgrade of known hosts. **iOS only** (`upgradeKnownHostsToHTTPS`). |
| `bAllowJavaScript`         | `true`  | Per-navigation JS enable (`allowsContentJavaScript`). **iOS only.** |

> **C++ note:** the view-appearance toggles above live in the nested
> `FInoWebViewSettings` struct — in code you write
> `Config.View.bEnableDevTools`, not `Config.bEnableDevTools`. They show
> up flat in the editor Details panel (`ShowOnlyInnerProperties`). The
> lockdown / security / recovery fields are top-level on
> `FInoWebViewConfig`. Each field carries a full doc-comment in
> `InoWebUITypes.h`; platform-specific behaviour is detailed in
> `Android.md` / `iOS.md`.

All defaults are chosen for shipping game UI. A dev build typically
wants `View.bEnableDevTools = true` and leaves the rest at their defaults.
