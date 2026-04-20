// Copyright Inoksan. All Rights Reserved.

#include "InoWebView.h"
#include "InoWebUILog.h"
#include "Misc/Paths.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// IInoWebViewImpl is already pulled in via InoWebView.h — no extra include here.

// ─────────────────────────────────────────────────────────────────────────────
//  Envelope helpers  (UE <-> JS message wire format)
//
//  Wire format is a fixed JSON object:
//      { "channel": "<string>", "payload": <any JSON value> }
//
//  The JS-side bridge (injected by FInoWebViewImpl_Windows) produces and
//  consumes the same shape, so both directions are symmetric.
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
    /** JSON-escape the small subset of characters that can appear in FName channel strings. */
    void AppendJsonEscapedString(FString& Out, const FString& In)
    {
        for (TCHAR C : In)
        {
            switch (C)
            {
                case TEXT('\\'): Out.Append(TEXT("\\\\")); break;
                case TEXT('"'):  Out.Append(TEXT("\\\""));  break;
                case TEXT('\n'): Out.Append(TEXT("\\n"));   break;
                case TEXT('\r'): Out.Append(TEXT("\\r"));   break;
                case TEXT('\t'): Out.Append(TEXT("\\t"));   break;
                default:         Out.AppendChar(C);         break;
            }
        }
    }

    /**
     * Build an envelope JSON string from a channel + raw-JSON payload. The
     * payload is inserted verbatim (not re-parsed), so the caller must
     * supply a valid JSON value. Empty input is treated as null.
     */
    FString BuildEnvelope(FName Channel, const FString& PayloadJson)
    {
        const FString SafePayload = PayloadJson.IsEmpty() ? TEXT("null") : PayloadJson;

        FString Out;
        Out.Reserve(SafePayload.Len() + 64);
        Out = TEXT("{\"channel\":\"");
        AppendJsonEscapedString(Out, Channel.ToString());
        Out += TEXT("\",\"payload\":");
        Out += SafePayload;
        Out += TEXT("}");
        return Out;
    }

}

UInoWebView::UInoWebView() = default;

// ─────────────────────────────────────────────────────────────────────────────
//  Initialization (called by the subsystem only)
// ─────────────────────────────────────────────────────────────────────────────
void UInoWebView::Init(FName InName, TUniquePtr<IInoWebViewImpl>&& InImpl,
                       void* ParentNativeHandle, const FInoWebViewConfig& Config)
{
    check(IsInGameThread());

    WebViewName = InName;
    Impl        = MoveTemp(InImpl);

    if (!Impl.IsValid())
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("UInoWebView[%s]: no platform implementation; operations will no-op."),
            *WebViewName.ToString());
        return;
    }

    // Wire callbacks BEFORE Initialize — the impl may fire events synchronously
    // during startup (e.g., fast-path env creation). `this` capture is safe:
    // the impl is our own member, destroyed with us, so lambdas can never
    // outlive us.
    Impl->OnMessageReceivedJson = [this](const FString& EnvelopeJson)
    {
        DispatchIncomingEnvelope(EnvelopeJson);
    };

    Impl->OnNavigationStartingCallback = [this](const FString& URI)
    {
        OnNavigationStarting.Broadcast(URI);
    };

    Impl->OnNavigationCompletedCallback = [this](bool bSuccess, const FString& URI)
    {
        OnNavigationCompleted.Broadcast(bSuccess, URI);
    };

    Impl->OnDocumentTitleChangedCallback = [this](const FString& Title)
    {
        OnDocumentTitleChanged.Broadcast(Title);
    };

    Impl->OnScriptDialogCallback = [this](EInoScriptDialogKind Kind, const FString& Message)
    {
        OnScriptDialog.Broadcast(Kind, Message);
    };

    Impl->OnNewWindowRequestedCallback = [this](const FString& URI)
    {
        OnNewWindowRequested.Broadcast(URI);
    };

    Impl->OnGotFocusCallback     = [this] { OnGotFocus.Broadcast();     };
    Impl->OnLostFocusCallback    = [this] { OnLostFocus.Broadcast();    };
    Impl->OnProcessFailedCallback = [this](const FString& Description)
    {
        OnProcessFailed.Broadcast(Description);
    };

    // OnReady deferral: impls may fire this callback synchronously (Android)
    // or asynchronously (Windows). Either way we queue the BP broadcast to
    // the next game tick so CreateWebView's caller has time to bind before
    // it fires. Weak self keeps the task safe across unexpected teardown.
    TWeakObjectPtr<UInoWebView> WeakSelf(this);
    Impl->OnReadyCallback = [WeakSelf]()
    {
        AsyncTask(ENamedThreads::GameThread, [WeakSelf]()
        {
            if (UInoWebView* Self = WeakSelf.Get())
            {
                Self->OnReady.Broadcast();
            }
        });
    };

    const bool bOk = Impl->Initialize(ParentNativeHandle, Config);
    if (!bOk)
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("UInoWebView[%s]: native Initialize() failed."), *WebViewName.ToString());
        Impl.Reset();
        return;
    }

    UE_LOG(LogInoWebUI, Log, TEXT("UInoWebView[%s] created."), *WebViewName.ToString());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Navigation
// ─────────────────────────────────────────────────────────────────────────────
void UInoWebView::LoadURL(const FString& URL)
{
    if (!Impl.IsValid())
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("UInoWebView[%s]::LoadURL: no implementation."), *WebViewName.ToString());
        return;
    }
    Impl->Navigate(URL);
}

void UInoWebView::LoadLocalFile(const FString& RelativeContentPath)
{
    const FString Absolute = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectContentDir() / RelativeContentPath);

    // file:/// URIs use forward slashes.
    FString URI = FString::Printf(TEXT("file:///%s"), *Absolute);
    URI.ReplaceInline(TEXT("\\"), TEXT("/"));

    LoadURL(URI);
}

void UInoWebView::Reload()
{
    if (Impl.IsValid()) Impl->Reload();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Visibility
// ─────────────────────────────────────────────────────────────────────────────
void UInoWebView::Show()
{
    if (Impl.IsValid()) Impl->SetVisible(true);
}

void UInoWebView::Hide()
{
    if (Impl.IsValid()) Impl->SetVisible(false);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Messaging (Phase 2)
// ─────────────────────────────────────────────────────────────────────────────
void UInoWebView::PostMessage(FName Channel, const FJsonObjectWrapper& Payload)
{
    check(IsInGameThread());

    if (!Impl.IsValid())
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("UInoWebView[%s]::PostMessage: no implementation."),
            *WebViewName.ToString());
        return;
    }

    // Serialize the wrapper to its compact JSON form. An empty or invalid
    // wrapper becomes the literal "null" so the JS bridge still sees a
    // well-formed envelope.
    FString PayloadStr;
    if (!Payload.JsonObject.IsValid() || !Payload.JsonObjectToString(PayloadStr))
    {
        PayloadStr = TEXT("null");
    }

    const FString Envelope = BuildEnvelope(Channel, PayloadStr);
    Impl->PostMessageJson(Envelope);
}

bool UInoWebView::HandleDevToolsAction(const FString& Channel)
{
    check(IsInGameThread());

    if (Channel == TEXT("_devtools.refresh"))
    {
        Reload();
        return true;
    }
    if (Channel == TEXT("_devtools.openDevTools"))
    {
        if (Impl.IsValid()) Impl->OpenDevTools();
        return true;
    }
    if (Channel == TEXT("_devtools.clearData"))
    {
        if (Impl.IsValid()) Impl->ClearAllCookies();
        return true;
    }
    if (Channel == TEXT("_devtools.hideWebUI"))
    {
        Hide();
        return true;
    }
    if (Channel == TEXT("_devtools.toggleTransparency"))
    {
        bBackgroundCurrentlyOpaque = !bBackgroundCurrentlyOpaque;
        if (Impl.IsValid()) Impl->SetBackgroundOpaque(bBackgroundCurrentlyOpaque);
        UE_LOG(LogInoWebUI, Log,
            TEXT("UInoWebView[%s]: background is now %s"),
            *WebViewName.ToString(),
            bBackgroundCurrentlyOpaque ? TEXT("OPAQUE (white)") : TEXT("TRANSPARENT"));
        return true;
    }
    if (Channel == TEXT("_devtools.info"))
    {
        OnDevInfo.Broadcast();
        return true;
    }
    if (Channel == TEXT("_devtools.devCallback"))
    {
        OnDevCallback.Broadcast();
        return true;
    }

    UE_LOG(LogInoWebUI, Warning,
        TEXT("UInoWebView[%s]: unknown dev-tools channel: %s"),
        *WebViewName.ToString(), *Channel);
    return false;
}

void UInoWebView::DispatchIncomingEnvelope(const FString& EnvelopeJson)
{
    check(IsInGameThread());

    // Parse the envelope. Anything non-conforming is dropped with a warning
    // rather than letting garbled input reach Blueprint handlers.
    TSharedPtr<FJsonObject> EnvObj;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(EnvelopeJson);
    if (!FJsonSerializer::Deserialize(Reader, EnvObj) || !EnvObj.IsValid())
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("UInoWebView[%s]: dropped malformed JS message (not valid JSON object)."),
            *WebViewName.ToString());
        return;
    }

    FString ChannelStr;
    if (!EnvObj->TryGetStringField(TEXT("channel"), ChannelStr) || ChannelStr.IsEmpty())
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("UInoWebView[%s]: dropped JS message with missing/empty 'channel'."),
            *WebViewName.ToString());
        return;
    }

    // Internal channels ("_devtools.*") are handled by the plugin, never
    // forwarded to user code. Keeps the dev overlay's wiring invisible.
    if (ChannelStr.StartsWith(TEXT("_devtools.")))
    {
        HandleDevToolsAction(ChannelStr);
        return;
    }

    // Repackage the payload into an FJsonObjectWrapper for Blueprint.
    // • object    → passed through verbatim
    // • non-obj   → wrapped in { "value": <payload> } so BP always sees an object
    // • null/none → empty wrapper (default-constructed JsonObject)
    FJsonObjectWrapper Wrapper;
    const TSharedPtr<FJsonValue> PayloadVal = EnvObj->TryGetField(TEXT("payload"));

    if (PayloadVal.IsValid() && PayloadVal->Type == EJson::Object)
    {
        Wrapper.JsonObject = PayloadVal->AsObject();
    }
    else if (PayloadVal.IsValid() && PayloadVal->Type != EJson::Null)
    {
        const TSharedRef<FJsonObject> Wrap = MakeShared<FJsonObject>();
        Wrap->SetField(TEXT("value"), PayloadVal);
        Wrapper.JsonObject = Wrap;
    }
    // else: Wrapper keeps its default-constructed (empty) JsonObject

    // Populate JsonString so the wrapper is BP-Details-panel friendly and
    // round-trip serialization (PostSerialize / ExportTextItem) works.
    Wrapper.JsonObjectToString(Wrapper.JsonString);

    UE_LOG(LogInoWebUI, Verbose,
        TEXT("UInoWebView[%s] <- JS  {channel='%s', payload=%s}"),
        *WebViewName.ToString(), *ChannelStr, *Wrapper.JsonString);

    OnMessageReceived.Broadcast(FName(*ChannelStr), Wrapper);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Runtime polish (Phase 3)
// ─────────────────────────────────────────────────────────────────────────────
void UInoWebView::OpenDevTools()
{
    if (Impl.IsValid()) Impl->OpenDevTools();
}

void UInoWebView::ExecuteJavaScript(const FString& Code)
{
    if (Impl.IsValid()) Impl->ExecuteJavaScript(Code);
}

void UInoWebView::SetMuted(bool bMuted)
{
    if (Impl.IsValid()) Impl->SetMuted(bMuted);
}

void UInoWebView::FocusWebView()
{
    if (Impl.IsValid()) Impl->FocusWebView();
}

void UInoWebView::SetZoomFactor(float Factor)
{
    if (Impl.IsValid()) Impl->SetZoomFactor(Factor);
}

float UInoWebView::GetZoomFactor() const
{
    return Impl.IsValid() ? Impl->GetZoomFactor() : 1.0f;
}

void UInoWebView::ClearAllCookies()
{
    if (Impl.IsValid()) Impl->ClearAllCookies();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Queries
// ─────────────────────────────────────────────────────────────────────────────
bool UInoWebView::IsReady() const
{
    return Impl.IsValid() && Impl->IsReady();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Subsystem-driven lifecycle
// ─────────────────────────────────────────────────────────────────────────────
void UInoWebView::OnParentResized(int32 X, int32 Y, int32 Width, int32 Height)
{
    if (Impl.IsValid()) Impl->SyncBounds(X, Y, Width, Height);
}

void UInoWebView::ShutdownImpl()
{
    if (Impl.IsValid())
    {
        Impl->Shutdown();
        Impl.Reset();
        UE_LOG(LogInoWebUI, Log, TEXT("UInoWebView[%s] destroyed."), *WebViewName.ToString());
    }
}

void UInoWebView::BeginDestroy()
{
    // GC is tearing us down — make sure the native WebView is closed while
    // the parent HWND still exists (this mostly matters when the object
    // outlives the subsystem teardown, e.g. in editor reloads).
    ShutdownImpl();
    Super::BeginDestroy();
}
