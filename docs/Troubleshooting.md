# Troubleshooting

A catalogue of the things that have actually gone wrong, and what
fixed them.

---

## "WebView works in editor, invisible in packaged build"

**Cause.** The packaged game is running in exclusive fullscreen.
Exclusive fullscreen bypasses the OS compositor, so the overlay HWND
has nothing to composite against.

**Fix.** Force borderless windowed:

```ini
; DefaultGameUserSettings.ini
[/Script/Engine.GameUserSettings]
FullscreenMode=1
```

...or launch with `-windowed`.

---

## "`WebView2 Runtime not found`" in the log

**Cause.** The evergreen WebView2 Runtime is not installed on the
machine. Rare on Windows 10 1803+ and effectively never on Windows 11,
but still possible on stripped-down images.

**Fix.** Install the
[Evergreen WebView2 Runtime installer](https://developer.microsoft.com/en-us/microsoft-edge/webview2/).
Plugin will log a clear error pointing to the installer instead of
crashing.

---

## Crash on editor shutdown or after a live-coding reload

**Cause.** A `UInoWebView` outlived the parent HWND. The controller
must be `Close()`'d while the window it lives under still exists;
otherwise COM leaks and sometimes crashes.

**Fix.** Verify `Shutdown()` is being called. `UInoWebView::BeginDestroy`
covers orderly cases; disorderly teardown (exit during play-in-editor,
crash-while-reload) needs the subsystem's `Deinitialize` to run first.

---

## Bounds are wrong after a DPI change

**Cause.** `FViewport::ViewportResizedEvent` fires on DPI changes.
If the subsystem's subscription has been lost, the WebView does not
resync its size.

**Fix.** Make sure `UInoWebUISubsystem::Initialize` ran. The event
subscription is set up there; losing it usually indicates the
subsystem was not created (check `GetGameInstance()->GetSubsystem<...>`
returns non-null).

---

## "My pre-ready calls disappeared"

**Cause.** They did not disappear. They were queued.

All runtime calls on `UInoWebView` (`LoadURL`, `Navigate`,
`SetVisible`, `Reload`, `PostMessage`, `ExecuteJavaScript`,
`SetMuted`, `SetZoomFactor`) are queued in the impl if issued before
the native controller signals ready. They are replayed inside
`ApplyPendingOperations()` once ready.

**If something still looks missing**, check the relevant field in
`FInternal`: `PendingNavigate`, `PendingVisible`, `PendingBounds`,
`PendingScripts`, `PendingMute`, etc.

---

## Bundle extracts but the page fails with `ERR_INVALID_RESPONSE`
## (Android) or `0x80070003` / path-not-found (Windows, packaged)

**Cause.** Path resolution mismatch between UE's virtual
`../../../Project/...` form and what the native loader (JNI on
Android, WebView2 on Windows) can open.
`FPaths::ConvertRelativePathToFull` does not fully resolve these on
Android.

**Fix.** Use
`IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(...)`
whenever you hand a path to a non-UE loader. Already done for the
bundle extract directory in
`UInoWebUISubsystem::ResolveBundleContentFolder` — use that as the
reference implementation when you do the same thing elsewhere.

---

## Android WebView covers ~1/3 of the screen instead of fullscreen

**Cause.** UE's `SWindow::GetClientRectInScreen` reports in a
coordinate system that does not match `FrameLayout.LayoutParams`'
physical-pixel contract.

**Fix.** Shipped default:
`InoWebViewAndroid.syncBounds` ignores the incoming values and forces
`MATCH_PARENT` on both axes. Sub-region sizing on Android would need
explicit DP -> px conversion; add it only when a concrete use case
requires it.

---

## Transparent WebView shows the desktop through empty areas in PIE

**But works fine in Standalone Game and packaged builds.**

**Cause.** Known composition limitation. PIE uses Slate-chromed windows
with `DWMWA_NCRENDERING_POLICY = DWMNCRP_DISABLED` plus a rounded-rect
`SetWindowRgn`. Under that combination, DWM composites transparent
child-HWND pixels against the desktop instead of against the parent's
swap chain. Standalone Game and shipped builds use OS chrome and
composite correctly.

**Workarounds (today).**

1. Use **Standalone Game** play mode when you need to visually verify
   the overlay.
2. Set `bTransparentBackground = false` during PIE iteration; the
   WebView is opaque, but the messaging / bounds / hardening pipeline
   is fully testable.

**Proper fix (future phase).** Switch from
`CreateCoreWebView2Controller` (child HWND) to
`CreateCoreWebView2CompositionController` with DirectComposition
visual hosting. A meaningful chunk of new code; tracked as roadmap
Phase 9.

---

## DevTools does not open

**Cause.** `bEnableDevTools` was `false` on the config when the view
was created. Runtime `OpenDevTools()` is a no-op with a warning in
that case; there is no way to retroactively enable DevTools on an
already-constructed WebView2 instance.

**Fix.** Rebuild the view with `bEnableDevTools = true`, or set it in
your dev-build config by default.

---

## Android: "`InoWebViewAndroid Java class not found`" at runtime

See `Android.md` -> *If the Java helper is not found* for the
step-by-step checklist.

---

## Verbose logging

Set the log category to verbose in `Engine.ini`:

```ini
[Core.Log]
LogInoWebUI=Verbose
```

Gets you the full life-cycle trace — construction, queue, ready,
navigation, messages, shutdown.
