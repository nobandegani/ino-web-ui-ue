# InoWebUI

**A native WebView overlay for Unreal Engine 5.7.**

Host React, Vue, Svelte, or plain HTML/CSS/JS on top of your game with
pixel-perfect transparency and zero texture-copy overhead. The WebView is a
real OS window composited above the Unreal viewport — not a
render-to-texture, not a UMG widget. The OS compositor (DWM on Windows,
SurfaceFlinger on Android) blends them. Unreal renders the 3D scene; the
WebView renders your UI; transparent pixels in your HTML reveal the game
underneath.

Platforms: **Windows (WebView2)**, **Android (android.webkit.WebView)**,
**iOS (`WKWebView`)**. macOS (`WKWebView`) is planned.

---

## Why not the built-in WebBrowser plugin?

UE ships a `WebBrowser` plugin that textures the browser output into a UMG
widget. That path involves a full GPU readback per frame, imposes input
translation through Slate, and historically ships a very old Chromium.
InoWebUI skips all of that:

| | UE `WebBrowser` | **InoWebUI** |
|---|---|---|
| Rendering model | Render-to-texture (GPU copy per frame) | Native OS window, zero copy |
| Chromium version | Shipped with the engine (old) | System WebView2 / Android WebView / iOS WKWebView (evergreen) |
| Transparency | Via UMG alpha | True OS compositor alpha |
| Input | Slate → WebView shim | Native, direct |
| Cost | Steady per-frame GPU + CPU overhead | Effectively free |

If you want a HUD-in-browser style UI stacked on your game — health bars,
menus, chat, dev tools, storefront, onboarding — this plugin is the
direct route.

---

## The one hard constraint

**The game must run in borderless-windowed mode, not exclusive
fullscreen.** Exclusive fullscreen bypasses the OS compositor, so any
overlay window becomes invisible. Set `FullscreenMode=1` in
`DefaultGameUserSettings.ini` or launch with `-windowed`.

---

## Feature matrix

| Feature | Windows | Android | iOS |
|---|:-:|:-:|:-:|
| Overlay create / destroy | ✔ | ✔ | ✔ |
| URL / local file / virtual-host loading | ✔ | ✔ | ✔ (1) |
| Show / Hide / Reload / SyncBounds | ✔ | ✔ | ✔ |
| Transparent background (config + runtime `SetBackgroundTransparent`) | ✔ | ✔ | ✔ |
| Two-way messaging (`window.InoWebUI` + `once`) | ✔ | ✔ | ✔ |
| Navigation lockdown (whitelist) | ✔ | ✔ | ✔ |
| JS dialog suppression (`alert`/`confirm`/`prompt`) | ✔ | ✔ | ✔ |
| `window.open` blocking | ✔ | ✔ | ✔ |
| Navigation + title + focus events | ✔ | ✔ | ✔ |
| Browser-style nav (`GoBack` / `GoForward` / `CanGo*`) | ✔ | ✔ | ✔ |
| State getters (`GetURL` / `GetTitle` / `IsLoading`) | ✔ | ✔ | ✔ |
| `StopLoading` / `LoadHTMLString` / `LoadURLWithHeaders` | ✔ | ✔ | ✔ |
| `SetCookie`, `ClearAllCookies`, `ClearAllData`, crash event | ✔ | ✔ | ✔ |
| Screenshot to file (`CapturePreview`, PNG / JPEG) | ✔ | ✔ | ✔ |
| Manual sub-region bounds (`SetBounds` / `SetBoundsAuto`) | ✔ | ✔ | ✔ |
| Zoom factor (`SetZoomFactor`) | ✔ | ✔ | ✔ (CSS zoom) |
| DevTools | In-process panel (F12) | Remote via `chrome://inspect` | Remote via Safari Web Inspector |
| ExecuteJavaScript, UA override, context-menu + accelerator toggles | ✔ | ✔ | ✔ (UA + JS) |
| Audio mute (`SetMuted`) | ✔ | — | partial (best-effort JS walk) |
| Web Bundle asset type | ✔ | ✔ | ✔ |
| Dev-tools floating overlay | ✔ | ✔ | ✔ |

(1) iOS uses a custom `inoweb://` scheme for the virtual host instead of
`https://` — see [`docs/iOS.md`](docs/iOS.md) for the divergence.

---

## Quick start

### 1. Install

Drop the `InoWebUI` folder into your project's `Plugins/` directory.

### 2. Acquire the WebView2 SDK (Windows)

```powershell
./Plugins/InoWebUI/Scripts/AcquireWebView2SDK.ps1
```

Idempotent — downloads roughly 5 MB from NuGet the first time, no-ops on
subsequent runs. The SDK folder is gitignored; re-run the script on any
fresh clone.

### 3. Regenerate project files and build

Right-click your `.uproject` → **Generate Visual Studio project files**,
then build. The plugin links `WebView2LoaderStatic.lib` — no extra DLL
ships with your game. Android needs no extra step; the plugin's UPL
script pulls its Java helper into the APK automatically.

### 4. Create your first WebView

```cpp
#include "InoWebUISubsystem.h"
#include "InoWebView.h"

void AMyPlayerController::BeginPlay()
{
    Super::BeginPlay();

    UInoWebUISubsystem* WebUI =
        GetGameInstance()->GetSubsystem<UInoWebUISubsystem>();

    FInoWebViewConfig Config;
    Config.InitialURL             = TEXT("https://inoweb.local/index.html");
    Config.VirtualHostName        = TEXT("inoweb.local");
    Config.VirtualHostFolder      = TEXT("WebUI/dist"); // under Content/
    Config.bTransparentBackground = true;
    Config.bVisibleOnCreate       = true;
    Config.bEnableDevTools        = true;   // dev builds only

    UInoWebView* View = WebUI->CreateWebView(TEXT("MainUI"), Config);
}
```

All three calls also exist as Blueprint nodes (`Create Web View`,
`Get Subsystem`, `Load URL`, etc.).

### 5. Talk to it from the page

```html
<script>
  // Receive from UE
  window.InoWebUI.on('playerState', (data) => {
      hpBar.style.width = data.hp + '%';
  });

  // Send to UE
  startButton.onclick = () =>
      window.InoWebUI.send('startMission', { id: 'tutorial' });
</script>
```

Both directions use a fixed JSON envelope:
`{ channel: string, payload: any }`. See
[`docs/Messaging.md`](docs/Messaging.md) for the full contract.

### 6. Recommended HTML defaults for game UI

A handful of "browser feels" defaults are controlled by the page itself,
not by any native API the plugin can call into. Add these to your
`index.html` for the full game-UI overlay experience — they kill pinch
zoom, input-focus auto-zoom (iOS), drag-to-select text, the long-press
peek-and-share popup (iOS), and the blue tap-flash (Android / iOS).

In `<head>`:

```html
<meta name="viewport"
      content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
```

In your CSS reset:

```css
* {
  -webkit-user-select: none;  user-select: none;
  -webkit-touch-callout: none;
  -webkit-tap-highlight-color: transparent;
}
/* Re-enable selection AND the long-press callout for form fields, so
   typing *and* the iOS Cut/Copy/Paste edit menu still work. Restoring
   only user-select isn't always enough: on some iOS versions a field
   left at `-webkit-touch-callout: none` won't raise the long-press
   "Paste" menu — which is the only way to paste into an empty password
   input (no text to double-tap-select first). */
input, textarea, [contenteditable] {
  -webkit-user-select: text;  user-select: text;
  -webkit-touch-callout: default;
}
```

Why HTML-side and not a `Config.View` flag: WebKit (iOS) handles pinch
zoom, double-tap zoom, and input auto-zoom internally — *above* the
UIKit gesture-recognizer chain. The plugin already disables every
native pinch path it can reach (`scrollView.minimumZoomScale = 1`,
`pinchGestureRecognizer.enabled = NO`, exhaustive recognizer scan), but
WebKit's internal mechanism only listens to the page's viewport meta.
Same story for text selection — `-webkit-user-select` is the standard
cross-platform off-switch and there's no native equivalent.

The demo pages under `Content/web/` already ship with these defaults —
`Content/web/index.html` is a usable starting template.

### Demo / showcase app

`Content/web/index.html` is a single self-contained showcase app — a
persistent shell with hash-routed sections (Overview, Buttons, Inputs,
Toggles, Containers, Data &amp; Charts, Motion, Effects, UE Bridge,
Chat). No external/CDN dependencies, so it works under lockdown,
offline, and on every platform. Point a `UInoWebBundle` (or
`VirtualHostFolder`) at `Content/web/` and load `index.html`.

It has a persistent **Hide UI** button. The plugin can't hide itself
from JS, so the button sends a bridge message you wire up in
Blueprint / C++:

```cpp
// On the UInoWebView returned by CreateWebView / CreateWebViewFromAsset:
View->OnMessageReceived.AddDynamic(this, &AMyHud::HandleWebMessage);

void AMyHud::HandleWebMessage(FName Channel, const FJsonObjectWrapper& Payload)
{
    if (Channel == TEXT("app.hide"))
    {
        View->Hide();          // overlay disappears; 3D scene stays
        // You own the re-show trigger — e.g. an input action:
        //   View->Show();  (and View->FocusWebView() if it has inputs)
    }
}
```

In Blueprint: bind the `On Message Received` red event pin, branch on
`Channel == "app.hide"`, call `Hide` on the WebView. Provide your own
key/button that calls `Show` to bring it back. If the page is opened
without the bridge (plain browser), the Hide button shows a toast
instead of silently doing nothing.

### Engine idle for full-screen web menus

A full-screen **opaque** web UI (main menu, settings, loading screen)
completely hides the 3D scene — rendering it is wasted GPU / CPU /
battery. The subsystem can idle Unreal with three switches: disable
world rendering, throttle Max FPS to a trickle, and pause the game. The
WebView is OS-composited independently of the UE loop, so the page stays
perfectly smooth while Unreal idles.

Both controls are on the subsystem (no per-view config flag):

- **Automatic (recommended):** call
  `UInoWebUISubsystem::SetAutoEngineIdle(true)` once. From then on the
  engine idles whenever **any** visible WebView is opaque, and resumes
  the instant none are (a view going transparent, hidden, or destroyed
  all resume it). No per-view wiring — re-evaluated on Show/Hide and on
  every transparency switch.
- **Manual:** call `UInoWebUISubsystem::SetEngineIdle(true/false)`
  yourself (BP-callable). OR-combined with auto mode.

Prior Max FPS / pause state is snapshot on idle and restored exactly on
resume; subsystem teardown force-restores. This is the only place the
plugin touches engine-wide state — strictly opt-in (auto defaults off).

---

## Further reading

| Doc | What's inside |
|---|---|
| [`docs/Architecture.md`](docs/Architecture.md) | The two-layer (UObject / pimpl) design, threading rules, async-safe queueing, shutdown order |
| [`docs/Messaging.md`](docs/Messaging.md) | Wire format, `window.InoWebUI` API, Blueprint Json nodes |
| [`docs/Hardening.md`](docs/Hardening.md) | Navigation lockdown, dialog/popup suppression, focus/zoom/cookie/crash events |
| [`docs/WebBundle.md`](docs/WebBundle.md) | `UInoWebBundle` asset — bundling web content into a UE asset, extract-on-demand |
| [`docs/DevToolsOverlay.md`](docs/DevToolsOverlay.md) | The floating in-game dev-tools panel |
| [`docs/Android.md`](docs/Android.md) | Android specifics — JNI bridge, lifecycle, logcat filter, remote DevTools |
| [`docs/iOS.md`](docs/iOS.md) | iOS specifics — `WKWebView`, custom-scheme virtual host, Safari Web Inspector |
| [`docs/Troubleshooting.md`](docs/Troubleshooting.md) | Common pitfalls and their fixes |
| [`docs/Contributing.md`](docs/Contributing.md) | How to contribute, CLA, coding conventions |

---

## Roadmap

| Phase | Scope | Status |
|---|---|---|
| 1 | Overlay: create, load URL, show/hide, resize tracking | done |
| 2 | Two-way messaging (`PostMessage`, `OnMessageReceived`, `window.InoWebUI`) | done |
| 3 | DevTools / ExecuteJS / mute / context-menu & accelerator toggles / UA override | done |
| 4 | Virtual-host mapping (serve local content as `https://`) | done |
| 5 | Game-UI hardening: lockdown + dialog/popup blocking + nav events + zoom/focus/cookies/crash | done |
| 6 | Android MVP — overlay + lifecycle + URL/show/hide/reload | done |
| 7 | `UInoWebBundle` asset — bundle web content into a UE asset, extract on demand | done |
| 8 | Android parity pass — messaging, hardening, virtual host, runtime polish | done |
| 9 | DirectComposition hosting (PIE transparency on Windows) | done |
| 10 | JS source dedup — single `bridge.js` / `dev_overlay.js` + build-time codegen | done |
| 11 | Bridge hardening (`once`, iteration safety, `Object.create(null)`, U+2028 fix) | done |
| 12 | Browser API completeness (back/forward, state getters, capture, headers, sub-region bounds) | done |
| 13 | iOS implementation (`WKWebView`) | done |
| 14 | macOS implementation (`WKWebView`) | planned |

---

## License

**InoWebUI is licensed under the Mozilla Public License, version 2.0**
with an additional attribution requirement. The short version:

- **You can** use InoWebUI in any project, commercial or otherwise, free
  of charge.
- **You can** ship closed-source games that embed it — MPL 2.0 is
  file-level copyleft, and only the InoWebUI files themselves are covered.
  Your game code is not affected.
- **You must** release your modifications to the InoWebUI source files
  under MPL 2.0 (for example, as a public fork of this repository).
- **You must** credit InoWebUI in the credits / about screen of any game
  or application that ships with the plugin.

### Required attribution

Include something equivalent to the following in your app's credits:

> Native WebView overlay powered by InoWebUI
> https://github.com/nobandegani/InoWebUI (MPL 2.0)

See [`LICENSE`](LICENSE) for the full legal text.

---

## Commercial support and the Fab listing

The GitHub version is the canonical source and is free under the terms
above. A paid listing on **Epic's Fab marketplace** is also available for
studios who want:

- pre-built binaries against pinned engine versions,
- priority bug-fix turnaround,
- email support,
- a marketplace receipt for procurement / legal teams.

Both distributions track the same codebase. Buying the Fab version does
not waive the MPL 2.0 obligations on the source files; it is a
convenience / support package, not an alternate license.

---

## Credits and trademark

"InoWebUI" and "Ino*" are trademarks of Inoland. The *code* is open
under MPL 2.0; the *name* is not. Forks are welcome — please rename them
to something distinct from "InoWebUI" so users can tell the difference.
