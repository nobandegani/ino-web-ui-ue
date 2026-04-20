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

```js
// Send UE <- JS
window.InoWebUI.send('startMission', { id: 'tutorial', difficulty: 'hard' });

// Subscribe to UE -> JS
window.InoWebUI.on('playerState', (data) => {
    hpBar.setWidth(data.hp);
    ammoLabel.textContent = data.ammo;
});

// Unsubscribe (pass the same handler reference)
window.InoWebUI.off('playerState', playerStateHandler);
```

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
