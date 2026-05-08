# Two-way messaging

InoWebUI exposes a single messaging API, identical on every platform,
reachable from both C++ and Blueprint on the UE side, and from any
JavaScript on the page side.

## Wire format

Every message in either direction is a JSON envelope:

```json
{ "channel": "<string>", "payload": <any JSON value> }
```

`channel` is a logical dispatch key. `payload` is whatever JSON your
message needs — an object, an array, a number, a string, even `null`.

## UE to JS

### C++

```cpp
FJsonObjectWrapper Payload;
Payload.JsonObject->SetNumberField(TEXT("hp"), 80);
Payload.JsonObject->SetNumberField(TEXT("ammo"), 24);
View->PostMessage(TEXT("playerState"), Payload);
```

### Blueprint

Use the **JsonBlueprintUtilities** plugin nodes (auto-enabled as a
dependency):

- `Load Json from String` — parses a literal into a wrapper.
- `Set Field` — adds or replaces typed fields
  (int / float / string / bool / object / array).
- `Post Message` — sends the envelope.

Empty or invalid wrappers are transmitted as JSON `null`. Calls made
before `IsReady()` are queued and replayed once the view is ready — the
same contract as every other runtime call in this plugin.

## JS to UE

### C++

```cpp
View->OnMessageReceived.AddDynamic(this, &AMyActor::HandleWebMessage);

void AMyActor::HandleWebMessage(FName Channel,
                                const FJsonObjectWrapper& Payload)
{
    if (Channel == TEXT("startMission") && Payload.JsonObject.IsValid())
    {
        const FString Id =
            Payload.JsonObject->GetStringField(TEXT("id"));
        // ...
    }
}
```

### Blueprint

`OnMessageReceived` appears as a red event pin on the `UInoWebView`,
outputs `Channel` (FName) and `Payload` (JsonObject). Read fields with
`Get Field`, `Has Field`, `Get Field Names`, `Get Json String`.

Non-object JS payloads (scalar, array, null) are auto-wrapped by the
plugin into `{ "value": <payload> }` so the BP side always gets a
usable JsonObject.

## JS side: `window.InoWebUI`

A small shim is injected into every page load, **before any user
script runs**:

- Windows: via `ICoreWebView2::AddScriptToExecuteOnDocumentCreated`.
- Android: via `WebViewClient.onPageStarted` injection.
- iOS: via `WKUserScript` with `WKUserScriptInjectionTimeAtDocumentStart`.

The shim source-of-truth is
[`Source/InoWebUI/JS/bridge.js`](../Source/InoWebUI/JS/bridge.js).
`Scripts/GenerateJSConstants.ps1` regenerates the platform-specific
constants from it — never edit the generated copies directly. Three
outputs are emitted: a C++ TCHAR header (Windows), a Java class
(Android), and an Objective-C++ NSString header (iOS).

```js
// Send UE <- JS
window.InoWebUI.send('startMission', { id: 'tutorial', difficulty: 'hard' });

// Subscribe to UE -> JS
window.InoWebUI.on('playerState', (data) => {
    hpBar.setWidth(data.hp);
    ammoLabel.textContent = data.ammo;
});

// Subscribe once — auto-unsubscribes after the first call.
window.InoWebUI.once('readyToken', (data) => { initWith(data.token); });

// Unsubscribe (pass the same handler reference)
window.InoWebUI.off('playerState', playerStateHandler);

// Bridge version string. Bumped on incompatible API changes; current = '1.1'.
console.log(window.InoWebUI.version);
```

Behaviour notes (v1.1):
- Listener storage uses `Object.create(null)`, so channel names like
  `"toString"` or `"hasOwnProperty"` work normally instead of colliding
  with `Object.prototype`.
- The dispatcher snapshots the listener array before iterating, so a
  handler that calls `on()` / `off()` mid-dispatch can't skip the next
  handler or run a removed one.
- `send()` validates that `channel` is a non-empty string; invalid input
  logs an error and short-circuits.
- If neither host transport is present (e.g., the page is opened in
  plain Chrome for testing), the bridge prints a single `console.warn`
  and `window.InoWebUI` is left undefined — feature-detection works.

The bridge is intentionally ES5-compatible: no `const`, no `Map`, no
arrow functions. It runs on arbitrary pages regardless of their
transpile target. You can build your own app in any modern flavour; the
shim just needs to survive the page's loader.

## Threading

All messaging runs on the **game thread**.

- WebView2 fires `MessageReceived` callbacks on the thread that created
  the environment. The plugin creates the environment on the game
  thread, so callbacks arrive on the game thread.
- Android marshals incoming messages onto the game thread via
  `AsyncTask(ENamedThreads::GameThread, ...)` before invoking the user's
  delegate, so the user-facing contract is identical.
- iOS: `WKScriptMessageHandler` callbacks fire on the iOS main thread
  (which is NOT UE's game thread on iOS); the plugin marshals onto the
  game thread via `AsyncTask` before invoking the user's delegate, same
  shape as Android.
- UE Blueprint dynamic multicast delegates broadcast on the calling
  thread, so user BP graphs run on the game thread with no surprises.

Net result: you never need `AsyncTask(GameThread, ...)` in your
`OnMessageReceived` handler. Just treat it like any other game-thread
callback.

## Reserved channels

The dev-tools floating overlay (see `DevToolsOverlay.md`) uses channels
prefixed `_devtools.`. They are intercepted inside
`UInoWebView::DispatchIncomingEnvelope` **before** the user's
`OnMessageReceived` delegate runs — your code never sees them. Do not
use `_devtools.*` as an application channel.
