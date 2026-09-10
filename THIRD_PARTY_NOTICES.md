# Third-Party Notices

InoWebUI is distributed under the [Apache License 2.0](LICENSE), Copyright 2026 Inoland.
Attribution obligations are in [NOTICE](NOTICE).

That grant covers Inoland's own code — `Source/InoWebUI/`, `Source/InoWebUIEditor/`,
and `Scripts/`. This repository also **commits Microsoft's WebView2 SDK loader**, which is
**not open source**. The table below is authoritative where it disagrees with any other
document here.

---

## Summary

| Path | Component | License |
|---|---|---|
| `Source/InoWebUI/`, `Source/InoWebUIEditor/`, `Scripts/` | InoWebUI | Apache-2.0 |
| `Source/ThirdParty/WebView2/include/*.h` | Microsoft Edge WebView2 SDK headers | **Microsoft proprietary** |
| `Source/ThirdParty/WebView2/lib/Win64/WebView2LoaderStatic.lib` | WebView2 static loader shim | **Microsoft proprietary** |
| `Source/ThirdParty/WebView2/lib/Win64/WebView2Loader.dll` | WebView2 dynamic loader | **Microsoft proprietary** — and **unused**, see §1 |
| *(not in repo)* WebView2 Runtime | Microsoft Edge WebView2 Runtime | Microsoft — end-user installed |
| *(not in repo)* `android.webkit.WebView` | Android System WebView | Google — OS component |
| *(not in repo)* `WKWebView` | Apple WebKit | Apple — OS component |
| *(not in repo)* `androidx.webkit:webkit` 1.16.0 | AndroidX WebKit | Apache-2.0 — Gradle dependency |

---

## 1. Microsoft Edge WebView2 SDK (Win64)

**Version:** see `Source/ThirdParty/WebView2/VERSION` (currently `1.0.3912.50`)
**Source:** `Microsoft.Web.WebView2` NuGet package, fetched by
`Scripts/AcquireWebView2SDK.ps1`
**Upstream:** https://developer.microsoft.com/microsoft-edge/webview2/

> **The WebView2 SDK is proprietary Microsoft software, not open source.** It is governed by
> the Microsoft Software License Terms accompanying the NuGet package, not by this
> repository's Apache-2.0 grant. Those terms do permit redistribution of the loader with an
> application, which is why it can be committed here — but read them against your own
> distribution before shipping.

### Note: `WebView2Loader.dll` is committed but not used

`InoWebUI.Build.cs` links **only** `WebView2LoaderStatic.lib`, the static loader shim. The
static shim locates the user's installed WebView2 Runtime at launch, so no loader DLL is
shipped or staged — `CLAUDE.md` says as much ("No extra DLL is shipped").

`WebView2Loader.dll` is therefore dead weight: nothing in `Build.cs` or any source file
references it; only the acquire script mentions it, because the NuGet package contains both
variants. **It can be deleted** without affecting the build. It is left in place here only
because removing files is the repository owner's call.

### Runtime dependency

The WebView2 **Runtime** is not redistributed. It ships with Windows 11 and is present on
essentially all Windows 10 1803+ installs via Edge. When missing, `Initialize()` logs a clear
error pointing at Microsoft's Evergreen installer rather than crashing.

### Trademarks

"Microsoft", "Microsoft Edge" and "WebView2" are trademarks of Microsoft Corporation.
InoWebUI is an independent project and is **not** affiliated with, sponsored by, or endorsed
by Microsoft.

---

## 2. Platform WebView engines (Android / iOS)

Neither is redistributed — both are operating-system components the plugin binds to at
runtime:

- **Android:** `android.webkit.WebView`, provided by Android System WebView / Chrome.
- **iOS:** `WKWebView`, part of Apple's WebKit framework.

Use of each is governed by the respective platform SDK and OS terms you already accept as an
Android or Apple developer.

### AndroidX WebKit

The Android build declares `androidx.webkit:webkit:1.16.0` via
`InoWebUI_UPL.xml`'s `buildGradleAdditions`. It is **Apache-2.0**, resolved by Gradle at build
time, and not vendored here. It supplies `addWebMessageListener`,
`addDocumentStartJavaScript`, and `setWebViewRenderProcessClient`.

"Android" is a trademark of Google LLC. "Apple", "iOS", "Safari" and "WebKit" are trademarks
of Apple Inc. InoWebUI is not affiliated with or endorsed by either.

---

## 3. Injected JavaScript

`Source/InoWebUI/JS/` (`bridge.js`, `bridge_ios.js`, `dev_overlay.js`,
`notify_overlay.js`) is Inoland-authored and Apache-2.0, as are the generated constants under
`Source/InoWebUI/Private/Generated/` and `Source/InoWebUI/Java/`. No third-party JavaScript
is bundled — no framework, no polyfill, no CDN fetch at runtime.

---

## 4. Web content you load

Content rendered inside the WebView — your own HTML/CSS/JS, anything a `UInoWebBundle`
carries, and anything fetched from the network — is **entirely yours** and carries whatever
licences you give it or inherit. This plugin is a host; it asserts nothing over what you
render in it.

---

## Reporting a problem with these notices

If a component is misattributed or a notice is missing, please open an issue at
https://github.com/nobandegani/ino-web-ui-ue/issues and it will be corrected.
