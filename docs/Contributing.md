# Contributing

Thanks for considering a contribution to InoWebUI. This document
explains how to submit changes, what's expected in terms of style, and
the legal bits around licensing.

## Licensing and the CLA

InoWebUI is distributed under the **Mozilla Public License, v. 2.0**
plus an attribution requirement — see [`../LICENSE`](../LICENSE).

Because the project is also offered commercially on Epic's Fab
marketplace, the maintainers need the right to relicense contributions
under those commercial terms. To that end, **every pull request must
be covered by a signed Contributor License Agreement (CLA)** before it
can be merged.

- Our CLA is managed by [CLA Assistant](https://cla-assistant.io/).
  A bot comment will appear on your first PR with a one-click link.
- The CLA grants Inoland a non-exclusive licence to redistribute your
  contribution under MPL 2.0 **and** under the Fab commercial terms.
  You retain copyright on your contribution.
- If you do not wish to sign the CLA, your change is still welcome —
  please open an issue describing it instead, and we will reimplement
  it in-house.

## Pull request flow

1. Open an issue first for anything non-trivial. "Is this a direction
   you want?" questions are cheap.
2. Fork, branch off `main`, keep the branch narrowly scoped.
3. Match the existing code style (see below).
4. Test on every platform you touched. Windows-only changes still
   need to at least *build* on Android, and vice versa.
5. Submit the PR. Sign the CLA on first contribution.
6. Address review comments; merges are typically squash-merges with a
   tidied commit message.

## Code style

### C++

- Follow **Epic's Unreal Engine coding standard**. If in doubt, look
  at the existing files and match them.
- **Every public type starts with `Ino`** — UObjects, USTRUCTs,
  UENUMs, plain C++ classes in public headers. No exceptions.
- Headers in `Public/` must not include any platform headers
  (no `Windows.h`, no `jni.h`, etc.). Platform code goes under
  `Private/Impl/<Platform>/`.
- Every public method on `UInoWebView` / `UInoWebUISubsystem` asserts
  `check(IsInGameThread())`.
- Every new runtime operation that can be called before the native
  controller is ready must be **queued** in `FInternal` and replayed
  from `ApplyPendingOperations()`.
- Async callbacks capture `TWeakPtr<int>` to `LifetimeToken`; never
  capture raw `this`. See [`Architecture.md`](Architecture.md).

### Java (Android)

- All public entry points are **static**, called from C++ via JNI.
- Every entry point marshals onto the UI thread via
  `Activity.runOnUiThread` if it touches `WebView` state.
- Log with the `"InoWebUI"` tag so the logcat filter in
  [`Android.md`](Android.md) continues to work.

### JavaScript (bridge + dev-tools overlay)

- ES5-compatible only. No `const`, no `let`, no arrow functions,
  no `Map` / `Set` / `Promise` — assume the page's transpile target
  is unknown.
- Source-of-truth lives in `Source/InoWebUI/JS/`
  (`bridge.js`, `bridge_ios.js`, and `dev_overlay.js`). Edit those
  files, then run `Plugins/InoWebUI/Scripts/GenerateJSConstants.ps1` to
  regenerate the C++ header, the Obj-C++ (iOS) header, and the Java
  constants the platform impls consume. Don't edit the generated copies
  directly — they'll be overwritten on the next regen.

## Commits

- Conventional-style subject lines are nice but not enforced
  (`feat:`, `fix:`, `docs:`, `refactor:`, ...).
- Keep the subject under ~72 characters. Put the *why* in the body.
- One logical change per commit when practical.

## Testing

- `Content/web/index.html` is the self-contained showcase / manual
  test harness. It auto-detects the bridge and reports status. Point a
  dev-mode view (`VirtualHostFolder` or a `UInoWebBundle`) at
  `Content/web/` and smoke-test whatever you changed.
- For messaging / hardening work, drive the harness from a Blueprint
  in the demo project's root — it has wired-up `OnMessageReceived`
  logs and a set of test buttons.

## Scope of what's welcome

- **Bug fixes** across any platform — always welcome.
- **Android parity** when gaps appear — always welcome.
- **New platform ports** (macOS / Linux — Windows, Android and iOS are
  already shipped) — very welcome, please open an issue first so we can
  coordinate the pimpl split.
- **New features** — please open an issue first. Some features get
  deferred to later roadmap phases; the plugin is intentionally
  minimalist at its public surface.

## Scope of what's out of scope

- Generalising InoWebUI into a "cross-framework embedding library" —
  the plugin is opinionated about Unreal specifically.
- Shipping our own Chromium — we rely on system WebView2 / Android
  WebView by design.
- Anything that breaks the "game-UI hardening defaults are locked
  down" promise. New toggles with secure defaults are fine; changing
  a default from secure to open is not.
