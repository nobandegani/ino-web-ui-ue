// Copyright Inoland. All Rights Reserved.

#include "InoWebView.h"
#include "InoWebUILog.h"
#include "InoWebUISubsystem.h"
#include "Engine/GameInstance.h"
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

    // Save the inputs so RecreateImpl can replay them after a renderer-
    // process failure without the caller having to pass anything back in.
    SavedConfig              = Config;
    SavedParentNativeHandle  = ParentNativeHandle;

    if (!Impl.IsValid())
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("UInoWebView[%s]: no platform implementation; operations will no-op."),
            *WebViewName.ToString());
        return;
    }

    // Wire callbacks BEFORE Initialize — the impl may fire events synchronously
    // during startup (e.g., fast-path env creation). The wiring is factored
    // out so RecreateImpl can reuse it on the fresh impl after a process
    // failure.
    WireImplCallbacks();

    // Seed the "covering" inputs from config. Opacity mirrors the
    // configured transparency (the dev overlay's "Toggle transparency"
    // can flip it later); the visible flag mirrors bVisibleOnCreate
    // (Show/Hide keep it current).
    bBackgroundCurrentlyOpaque = !Config.View.bTransparentBackground;
    bViewVisible               = Config.View.bVisibleOnCreate;

    const bool bOk = Impl->Initialize(ParentNativeHandle, Config);
    if (!bOk)
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("UInoWebView[%s]: native Initialize() failed."), *WebViewName.ToString());
        Impl.Reset();
        return;
    }

    UE_LOG(LogInoWebUI, Log, TEXT("UInoWebView[%s] created."), *WebViewName.ToString());

    // Report initial covering state (opaque + visible). The subsystem only
    // acts on it if its auto engine-idle mode is enabled.
    RefreshCoveringState();
}

// ─────────────────────────────────────────────────────────────────────────────
//  WireImplCallbacks
//
//  Sets every IInoWebViewImpl callback slot on the current Impl. Extracted
//  from Init so RecreateImpl can reuse it identically — the lambdas all
//  capture `this`, which is safe because they're stored on Impl which we own,
//  so a lambda firing implies our UObject is alive.
// ─────────────────────────────────────────────────────────────────────────────
void UInoWebView::WireImplCallbacks()
{
    check(IsInGameThread());
    if (!Impl.IsValid()) return;

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

    // Process failure goes through HandleProcessFailed: that path fires the
    // BP delegate AND drives the auto-recover state machine. Direct delegate
    // broadcast (the pre-Phase-15 behaviour) only happened from here; the
    // recovery hook is now centralized.
    Impl->OnProcessFailedCallback = [this](const FString& Description)
    {
        HandleProcessFailed(Description);
    };

    // Renderer-process responsiveness — distinct from "process died". The
    // renderer is alive but its main thread is stuck. Auto-terminate (if
    // configured) is driven from the Java side via Handler.postDelayed;
    // we just surface the begin / end events to BP for telemetry.
    Impl->OnRenderProcessUnresponsiveCallback = [this]
    {
        UE_LOG(LogInoWebUI, Warning,
            TEXT("UInoWebView[%s]: renderer became unresponsive (UI thread stuck)."),
            *WebViewName.ToString());
        OnRenderProcessUnresponsive.Broadcast();
    };
    Impl->OnRenderProcessResponsiveCallback = [this]
    {
        UE_LOG(LogInoWebUI, Log,
            TEXT("UInoWebView[%s]: renderer recovered (responsive again)."),
            *WebViewName.ToString());
        OnRenderProcessResponsive.Broadcast();
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

    // CapturePreview completion → BP delegate. The impl (Windows / Android)
    // already invokes this on the game thread, so a direct broadcast is fine.
    Impl->OnCapturePreviewCompleteCallback =
        [this](bool bSuccess, const FString& FilePath)
        {
            OnCapturePreviewComplete.Broadcast(bSuccess, FilePath);
        };
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
    bViewVisible = true;
    RefreshCoveringState();
}

void UInoWebView::Hide()
{
    if (Impl.IsValid()) Impl->SetVisible(false);
    bViewVisible = false;
    RefreshCoveringState();
}

void UInoWebView::RefreshCoveringState()
{
    // "Covering" = this view fully hides the 3D scene: opaque AND visible.
    // We just report it; the subsystem decides whether to idle the engine
    // (only when its auto mode is on, or aggregated with a manual call).
    if (UInoWebUISubsystem* Subsystem = Cast<UInoWebUISubsystem>(GetOuter()))
    {
        const bool bCovering = bViewVisible && bBackgroundCurrentlyOpaque;
        Subsystem->SetViewCovering(this, bCovering);
    }
}

void UInoWebView::SetBackgroundTransparent(bool bTransparent)
{
    check(IsInGameThread());

    const bool bOpaque = !bTransparent;
    if (bOpaque == bBackgroundCurrentlyOpaque)
    {
        return; // no change — don't churn the impl or re-evaluate idle
    }

    bBackgroundCurrentlyOpaque = bOpaque;
    if (Impl.IsValid()) Impl->SetBackgroundOpaque(bOpaque);

    UE_LOG(LogInoWebUI, Log,
        TEXT("UInoWebView[%s]: background set to %s"),
        *WebViewName.ToString(),
        bOpaque ? TEXT("OPAQUE (white)") : TEXT("TRANSPARENT"));

    // Opacity is half of the "covering" condition — re-evaluate so auto
    // engine-idle reacts immediately (mirrors the dev-overlay toggle).
    RefreshCoveringState();
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
        // Opacity is half of the "covering" condition — re-evaluate.
        RefreshCoveringState();
        return true;
    }
    // Note: "_devtools.info" never reaches UE — the dev overlay handles
    // Info entirely in JS with a modal that shows page/platform stats.
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
//  O(1) cached state queries
// ─────────────────────────────────────────────────────────────────────────────
FString UInoWebView::GetURL() const
{
    return Impl.IsValid() ? Impl->GetURL() : FString();
}

FString UInoWebView::GetTitle() const
{
    return Impl.IsValid() ? Impl->GetTitle() : FString();
}

bool UInoWebView::IsLoading() const
{
    return Impl.IsValid() && Impl->IsLoading();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Browser-style nav (history)
// ─────────────────────────────────────────────────────────────────────────────
void UInoWebView::GoBack()
{
    check(IsInGameThread());
    if (Impl.IsValid()) Impl->GoBack();
}

void UInoWebView::GoForward()
{
    check(IsInGameThread());
    if (Impl.IsValid()) Impl->GoForward();
}

bool UInoWebView::CanGoBack() const
{
    return Impl.IsValid() && Impl->CanGoBack();
}

bool UInoWebView::CanGoForward() const
{
    return Impl.IsValid() && Impl->CanGoForward();
}

void UInoWebView::StopLoading()
{
    check(IsInGameThread());
    if (Impl.IsValid()) Impl->StopLoading();
}

void UInoWebView::LoadHTMLString(const FString& HTML, const FString& BaseURI)
{
    check(IsInGameThread());
    if (Impl.IsValid()) Impl->LoadHTMLString(HTML, BaseURI);
}

void UInoWebView::SetCookie(const FString& URL, const FString& Cookie)
{
    check(IsInGameThread());
    if (Impl.IsValid()) Impl->SetCookie(URL, Cookie);
}

void UInoWebView::ClearAllData()
{
    check(IsInGameThread());
    if (Impl.IsValid()) Impl->ClearAllData();
}

bool UInoWebView::CapturePreview(EInoImageFormat Format, const FString& OutFilePath)
{
    check(IsInGameThread());
    if (!Impl.IsValid()) return false;
    return Impl->CapturePreview(Format, OutFilePath);
}

void UInoWebView::LoadURLWithHeaders(const FString& URL,
                                     const TMap<FString, FString>& Headers)
{
    check(IsInGameThread());
    if (Impl.IsValid()) Impl->LoadURLWithHeaders(URL, Headers);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Sub-region bounds — manual vs auto modes (item 9)
// ─────────────────────────────────────────────────────────────────────────────
void UInoWebView::SetBounds(int32 X, int32 Y, int32 W, int32 H)
{
    check(IsInGameThread());

    bManualBounds = true;
    ManualX = X; ManualY = Y; ManualW = W; ManualH = H;

    if (Impl.IsValid())
    {
        Impl->SetBoundsMode(true);
        Impl->SyncBounds(X, Y, W, H);
    }
}

void UInoWebView::SetBoundsAuto()
{
    check(IsInGameThread());

    bManualBounds = false;

    if (Impl.IsValid())
    {
        Impl->SetBoundsMode(false);
    }

    // Ask the subsystem to re-push the current parent rect to JUST this
    // WebView so auto sizing snaps back to the parent right now (instead of
    // waiting for the next viewport resize event).
    if (UInoWebUISubsystem* Subsystem = Cast<UInoWebUISubsystem>(GetOuter()))
    {
        Subsystem->BroadcastClientRectToOne(this);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Subsystem-driven lifecycle
// ─────────────────────────────────────────────────────────────────────────────
void UInoWebView::OnParentResized(int32 X, int32 Y, int32 Width, int32 Height)
{
    if (Impl.IsValid()) Impl->SyncBounds(X, Y, Width, Height);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Auto-recover on renderer process failure
// ─────────────────────────────────────────────────────────────────────────────

void UInoWebView::HandleProcessFailed(const FString& Description)
{
    check(IsInGameThread());

    UE_LOG(LogInoWebUI, Warning,
        TEXT("UInoWebView[%s]: renderer process failed (%s). Auto-recover is %s."),
        *WebViewName.ToString(), *Description,
        SavedConfig.bAutoRecoverOnProcessFailed ? TEXT("ON") : TEXT("OFF"));

    // Fire the BP delegate FIRST so the user's handler can react (telemetry,
    // toast, queue a reconnect, etc.) before we touch the impl.
    OnProcessFailed.Broadcast(Description);

    // If the handler called DestroyWebView, our Impl has been reset to null
    // — the user explicitly tore the view down and doesn't want a recreate.
    // Bail before scheduling anything.
    if (!Impl.IsValid())
    {
        UE_LOG(LogInoWebUI, Log,
            TEXT("UInoWebView[%s]: OnProcessFailed handler destroyed the view — "
                 "skipping auto-recover."),
            *WebViewName.ToString());
        return;
    }

    if (!SavedConfig.bAutoRecoverOnProcessFailed)
    {
        return;
    }

    if (!TryConsumeRecoveryBudget())
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("UInoWebView[%s]: auto-recover budget exhausted (3 attempts within 60s) — "
                 "leaving WebView dead. The page may be consistently crashing the renderer; "
                 "destroy + recreate manually from your OnProcessFailed handler."),
            *WebViewName.ToString());
        return;
    }

    // Defer to next tick so the BP handler's side effects (including a
    // potential DestroyWebView) settle before we recreate. Weak self keeps
    // the task safe across GC. bAwaitingRecreate is the kill-switch — if the
    // user destroyed us in the meantime, ShutdownImpl cleared it.
    bAwaitingRecreate = true;
    TWeakObjectPtr<UInoWebView> WeakSelf(this);
    AsyncTask(ENamedThreads::GameThread, [WeakSelf]()
    {
        if (UInoWebView* Self = WeakSelf.Get())
        {
            if (Self->bAwaitingRecreate)
            {
                Self->RecreateImpl();
            }
        }
    });
}

bool UInoWebView::TryConsumeRecoveryBudget()
{
    // Sliding window: at most N attempts within the last T seconds. Prevents
    // an infinite recreate loop when the page itself is what's crashing the
    // renderer (broken WebGL, broken video codec, OOM-on-load, ...).
    static constexpr int32  MaxRecoveriesPerWindow = 3;
    static constexpr double RecoveryWindowSec      = 60.0;

    const double Now = FPlatformTime::Seconds();
    RecentRecoveryAttempts.RemoveAll([Now](double T)
    {
        return Now - T > RecoveryWindowSec;
    });

    if (RecentRecoveryAttempts.Num() >= MaxRecoveriesPerWindow)
    {
        return false;
    }

    RecentRecoveryAttempts.Add(Now);
    return true;
}

void UInoWebView::RecreateImpl()
{
    check(IsInGameThread());
    bAwaitingRecreate = false;

    UE_LOG(LogInoWebUI, Log,
        TEXT("UInoWebView[%s]: auto-recovering after renderer process failure "
             "(attempt %d in current 60s window)."),
        *WebViewName.ToString(), RecentRecoveryAttempts.Num());

    // Drop the dead impl. Shutdown is idempotent against Java-side cleanup
    // that already happened in onRenderProcessGone — the Java destroyWebView
    // call is a no-op when the SparseArray entry is gone.
    if (Impl.IsValid())
    {
        Impl->Shutdown();
        Impl.Reset();
    }

    // Allocate a fresh platform impl. Same factory used by CreateWebView.
    TUniquePtr<IInoWebViewImpl> NewImpl = CreateInoWebViewImpl();
    if (!NewImpl.IsValid())
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("UInoWebView[%s]: RecreateImpl — factory returned null, "
                 "leaving WebView dead."),
            *WebViewName.ToString());
        return;
    }
    Impl = MoveTemp(NewImpl);

    // Wire callbacks BEFORE Initialize, same ordering as the original Init.
    WireImplCallbacks();

    const bool bOk = Impl->Initialize(SavedParentNativeHandle, SavedConfig);
    if (!bOk)
    {
        UE_LOG(LogInoWebUI, Error,
            TEXT("UInoWebView[%s]: RecreateImpl — native Initialize() failed; "
                 "WebView left dead."),
            *WebViewName.ToString());
        Impl.Reset();
        return;
    }

    // Re-assert the post-creation runtime state in case it diverged from
    // SavedConfig.View defaults (the user may have toggled visibility /
    // transparency / bounds at runtime — we want the recreated WebView to
    // come back in the SAME state, not in the original config state).
    const bool bConfigOpaque = !SavedConfig.View.bTransparentBackground;
    if (bBackgroundCurrentlyOpaque != bConfigOpaque)
    {
        Impl->SetBackgroundOpaque(bBackgroundCurrentlyOpaque);
    }
    const bool bConfigVisible = SavedConfig.View.bVisibleOnCreate;
    if (bViewVisible != bConfigVisible)
    {
        Impl->SetVisible(bViewVisible);
    }
    if (bManualBounds)
    {
        Impl->SetBoundsMode(true);
        Impl->SyncBounds(ManualX, ManualY, ManualW, ManualH);
    }

    // Re-engage covering state for the subsystem's auto engine-idle.
    RefreshCoveringState();

    UE_LOG(LogInoWebUI, Log,
        TEXT("UInoWebView[%s]: recreate complete — fresh native WebView in place, "
             "page reloading to InitialURL '%s'."),
        *WebViewName.ToString(), *SavedConfig.InitialURL);
}

void UInoWebView::ShutdownImpl()
{
    // Drop our "covering" entry first so a destroyed/opaque view can't
    // leave the engine throttled or paused. Idempotent (set Remove).
    if (UInoWebUISubsystem* Subsystem = Cast<UInoWebUISubsystem>(GetOuter()))
    {
        Subsystem->SetViewCovering(this, false);
    }

    // Kill any pending auto-recover; if the user is destroying us, they
    // don't want a fresh WebView to materialize on the next tick.
    bAwaitingRecreate = false;

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
