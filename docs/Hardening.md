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
| `OnProcessFailed`          | `(Description: String)`                        | Chromium subprocess crashed. Consider `Reload()` or recreate the view. |

## Runtime methods (BP-callable on `UInoWebView`)

| Method | Purpose |
|---|---|
| `FocusWebView()`           | Move keyboard focus into the WebView — needed before HTML `<input>` can type. |
| `SetZoomFactor(Factor)`    | 1.0 = 100%, 1.5 = 150%. |
| `GetZoomFactor()`          | Current zoom, returns 1.0 if not ready. |
| `ClearAllCookies()`        | Delete all cookies in this view's isolated profile. Useful for logout. |
| `ExecuteJavaScript(Code)`  | Run arbitrary JS in the page. Fire-and-forget — if you need a result, have the JS `window.InoWebUI.send(...)` instead. Queued if pre-ready. |
| `SetMuted(bool)`           | Mute / unmute audio (Windows only; Android `WebView` has no audio-mute API and logs a warning). |
| `OpenDevTools()`           | Open the Chromium DevTools panel (Windows). On Android the same call logs the `chrome://inspect/#devices` instructions — see `Android.md`. |

## Config toggles (applied once at `CreateWebView`)

| Field | Default | Effect |
|---|---|---|
| `bEnableDevTools`          | `false` | Enables DevTools. F12 opens it if `bEnableAcceleratorKeys` is also true; otherwise call `OpenDevTools()`. Also gates the floating dev-tools overlay (see `DevToolsOverlay.md`). |
| `bEnableContextMenus`      | `false` | Right-click browser context menu. |
| `bEnableAcceleratorKeys`   | `false` | Browser shortcuts (F5, F12, Ctrl+F, Ctrl+P, ...). |
| `bStartMuted`              | `false` | Audio muted on creation. Call `SetMuted(false)` to unmute. |
| `UserAgentOverride`        | `""`    | Overrides `navigator.userAgent` inside the WebView. |

All defaults are chosen for shipping game UI. A dev build typically
wants `bEnableDevTools = true` and leaves the rest at their defaults.
