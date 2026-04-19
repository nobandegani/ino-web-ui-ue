// Copyright Inoksan. All Rights Reserved.

#include "InoWebView.h"
#include "InoWebUILog.h"
#include "Misc/Paths.h"
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

    /** Serialize a single JsonValue back to a condensed JSON string (for passing to BP). */
    FString JsonValueToString(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid() || Value->Type == EJson::Null)
        {
            return TEXT("null");
        }
        FString Out;
        auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
        FJsonSerializer::Serialize(Value, FString(), Writer);
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

    // Wire the JS -> UE pipe before Initialize so we don't miss any message
    // the impl produces during startup. `this` capture is safe: the impl is
    // our own member, destroyed with us, so the lambda can never outlive us.
    Impl->OnMessageReceivedJson = [this](const FString& EnvelopeJson)
    {
        DispatchIncomingEnvelope(EnvelopeJson);
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
void UInoWebView::PostMessage(FName Channel, const FString& PayloadJson)
{
    check(IsInGameThread());

    if (!Impl.IsValid())
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("UInoWebView[%s]::PostMessage: no implementation."),
            *WebViewName.ToString());
        return;
    }

    const FString Envelope = BuildEnvelope(Channel, PayloadJson);
    Impl->PostMessageJson(Envelope);
}

void UInoWebView::DispatchIncomingEnvelope(const FString& EnvelopeJson)
{
    check(IsInGameThread());

    // Parse the envelope. Anything non-conforming is dropped with a warning
    // rather than letting garbled input reach Blueprint handlers.
    TSharedPtr<FJsonObject> Obj;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(EnvelopeJson);
    if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("UInoWebView[%s]: dropped malformed JS message (not valid JSON object)."),
            *WebViewName.ToString());
        return;
    }

    FString ChannelStr;
    if (!Obj->TryGetStringField(TEXT("channel"), ChannelStr) || ChannelStr.IsEmpty())
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("UInoWebView[%s]: dropped JS message with missing/empty 'channel'."),
            *WebViewName.ToString());
        return;
    }

    // Payload is allowed to be any JSON value (object, array, scalar, null).
    // Re-serialize it to a string so BP handlers receive a self-contained blob
    // that's easy to pass through FJsonObjectConverter / Parse JSON nodes.
    const TSharedPtr<FJsonValue> PayloadVal = Obj->TryGetField(TEXT("payload"));
    const FString PayloadStr = JsonValueToString(PayloadVal);

    UE_LOG(LogInoWebUI, Verbose,
        TEXT("UInoWebView[%s] <- JS  {channel='%s', payload=%s}"),
        *WebViewName.ToString(), *ChannelStr, *PayloadStr);

    OnMessageReceived.Broadcast(FName(*ChannelStr), PayloadStr);
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
