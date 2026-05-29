// Copyright Inoland. All Rights Reserved.

#include "InoWebViewImpl_iOS.h"

#if PLATFORM_IOS

#include "InoWebUILog.h"
#include "HAL/ThreadSafeCounter.h"
#include "HAL/CriticalSection.h"
#include "Async/Async.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Generated/InoWebUIScripts_iOS.generated.h"

#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#import <Foundation/Foundation.h>

#import "IOS/IOSAppDelegate.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Process-unique instance id generator. Crosses into Obj-C as a plain int
//  and keys our impl registry. Atomic so CreateWebView is safe even if it
//  ever moves off the game thread in the future.
// ─────────────────────────────────────────────────────────────────────────────
static FThreadSafeCounter GInstanceIdGenerator(0);

// ─────────────────────────────────────────────────────────────────────────────
//  Impl registry — lets Obj-C delegate callbacks (which fire on the iOS main
//  thread) find the right C++ impl by ID. Registered in Initialize,
//  unregistered in Shutdown. Held under a lock because callbacks may queue
//  game-thread tasks that race with Shutdown.
// ─────────────────────────────────────────────────────────────────────────────
static FCriticalSection                       GRegistryLock;
static TMap<int32, FInoWebViewImpl_iOS*>      GImplRegistry;

// Custom scheme used to bridge VirtualHost → folder. WKWebView does NOT allow
// intercepting https:// (security), so we register our own scheme handler and
// translate inoweb://<host>/<path> requests into local-file reads.
//
// Documented divergence: on iOS the user's Config.VirtualHostName is reachable
// via  inoweb://<VirtualHostName>/...  not  https://<VirtualHostName>/...
// Lockdown matching has to whitelist the `inoweb` scheme.
static NSString* const kInoVirtualScheme = @"inoweb";

// JS-message handler name (matches the JS bridge: window.webkit.messageHandlers._InoWebUIHost).
static NSString* const kInoMessageHandler = @"_InoWebUIHost";

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers — string conversion + game-thread dispatch (mirrors Android)
// ─────────────────────────────────────────────────────────────────────────────
static FString FStringFromNSString(NSString* S)
{
    if (S == nil) return FString();
    return FString(UTF8_TO_TCHAR([S UTF8String]));
}

static NSString* NSStringFromFString(const FString& S)
{
    return [NSString stringWithUTF8String:TCHAR_TO_UTF8(*S)];
}

template <typename FLambda>
static void DispatchOnGameThread(int32 Id, FLambda&& Action)
{
    const int32 LocalId = Id;
    AsyncTask(ENamedThreads::GameThread,
        [LocalId, Action = Forward<FLambda>(Action)]() mutable
        {
            FScopeLock Lock(&GRegistryLock);
            if (FInoWebViewImpl_iOS** Found = GImplRegistry.Find(LocalId))
            {
                if (FInoWebViewImpl_iOS* Impl = *Found)
                {
                    Action(Impl);
                }
            }
        });
}

// Encode an FString as a JS string literal (with surrounding quotes) safe to
// inline into a JS expression. Same shape as the Java side's jsStringLiteral —
// keeps both transports symmetric so PostMessageJson lands as JSON.parse(...)
// without breakage from U+2028/U+2029 or other oddities.
static NSString* InoJSStringLiteral(const FString& S)
{
    NSMutableString* Out = [NSMutableString stringWithCapacity:S.Len() + 16];
    [Out appendString:@"\""];
    for (int32 i = 0; i < S.Len(); ++i)
    {
        const TCHAR C = S[i];
        switch (C)
        {
            case TEXT('\\'): [Out appendString:@"\\\\"]; break;
            case TEXT('"'):  [Out appendString:@"\\\""]; break;
            case TEXT('\n'): [Out appendString:@"\\n"];  break;
            case TEXT('\r'): [Out appendString:@"\\r"];  break;
            case TEXT('\t'): [Out appendString:@"\\t"];  break;
            case TEXT('\b'): [Out appendString:@"\\b"];  break;
            case TEXT('\f'): [Out appendString:@"\\f"];  break;
            default:
            {
                if (C == 0x2028)      { [Out appendString:@"\\u2028"]; }
                else if (C == 0x2029) { [Out appendString:@"\\u2029"]; }
                else if (C < 0x20)    { [Out appendFormat:@"\\u%04x", (unsigned)C]; }
                else
                {
                    unichar Ch = (unichar)C;
                    [Out appendString:[NSString stringWithCharacters:&Ch length:1]];
                }
                break;
            }
        }
    }
    [Out appendString:@"\""];
    return Out;
}

// UE-style wildcard match: "*" = any run, "?" = single char.
// Same semantics as FString::MatchesWildcard / Java matchesWildcard.
static bool InoMatchesWildcard(const FString& S, const FString& Pattern)
{
    return S.MatchesWildcard(Pattern);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Forward decls of the Obj-C classes — defined further down the file.
// ─────────────────────────────────────────────────────────────────────────────
@class InoWebViewBridge_iOS;
@class InoWebViewSchemeHandler_iOS;

// ─────────────────────────────────────────────────────────────────────────────
//  FInternal — opaque state owned by FInoWebViewImpl_iOS. Lives inside the
//  .mm so WebKit/UIKit types stay out of the public header.
// ─────────────────────────────────────────────────────────────────────────────
struct FInoWebViewImpl_iOS_Internal
{
    // Explicit __strong qualifiers because ARC requires ownership annotations
    // on Obj-C pointer members of a C++ struct. Members are released when
    // FInoWebViewImpl_iOS deletes the struct, freeing every WK* + bridge.
    __strong WKWebView*                       WebView          = nil;
    __strong WKWebViewConfiguration*          Configuration    = nil;
    __strong InoWebViewBridge_iOS*            Bridge           = nil;  // delegates + script handler
    __strong InoWebViewSchemeHandler_iOS*     SchemeHandler    = nil;  // optional, only when vhost set

    /** Resolved absolute folder for the virtual host, if configured. */
    __strong NSString*                        VirtualHostFolder = nil;
};

// ─────────────────────────────────────────────────────────────────────────────
//  InoWebViewSchemeHandler_iOS
//
//  Implements WKURLSchemeHandler for the custom "inoweb" scheme. Handles any
//  inoweb://<host>/<path> request by mapping it under the resolved folder.
//
//  This is the iOS equivalent of WebView2's SetVirtualHostNameToFolderMapping
//  and Android's WebViewClient.shouldInterceptRequest. Unlike those, we can't
//  intercept https:// in WKWebView (Apple won't allow it), hence the custom
//  scheme. Symmetry with the other platforms lives at the bridge.js layer —
//  user code uses window.InoWebUI the same way regardless of how the page
//  was actually served.
// ─────────────────────────────────────────────────────────────────────────────
@interface InoWebViewSchemeHandler_iOS : NSObject <WKURLSchemeHandler>
@property (nonatomic, copy) NSString* RootFolder; // absolute, no trailing slash
@property (nonatomic, copy) NSString* HostName;   // case-folded virtual host
@end

@implementation InoWebViewSchemeHandler_iOS

- (void)webView:(WKWebView*)webView startURLSchemeTask:(id<WKURLSchemeTask>)task
{
    NSURL* URL = task.request.URL;
    NSString* Host = [URL.host lowercaseString];
    if (self.HostName.length > 0
        && Host != nil
        && ![Host isEqualToString:self.HostName])
    {
        // Host doesn't match — 404, not a security error.
        [self failTask:task withCode:404 reason:@"host mismatch"];
        return;
    }

    NSString* Path = URL.path;
    if (Path.length == 0 || [Path isEqualToString:@"/"]) { Path = @"/index.html"; }
    // URL path is already percent-decoded for componentsWithURL — but iOS keeps
    // it as the encoded form, so decode here.
    NSString* DecodedPath = [Path stringByRemovingPercentEncoding];
    if (DecodedPath == nil) { DecodedPath = Path; }

    // Reject path traversal.
    if ([DecodedPath rangeOfString:@".."].location != NSNotFound)
    {
        [self failTask:task withCode:403 reason:@"path traversal rejected"];
        return;
    }

    NSString* AbsPath = [self.RootFolder stringByAppendingString:DecodedPath];
    NSData* Data = [NSData dataWithContentsOfFile:AbsPath];
    if (Data == nil)
    {
        [self failTask:task withCode:404 reason:@"file not found"];
        return;
    }

    NSString* MimeType = [self mimeTypeForPath:AbsPath];
    NSURLResponse* Response = [[NSHTTPURLResponse alloc]
        initWithURL:URL
        statusCode:200
        HTTPVersion:@"HTTP/1.1"
        headerFields:@{
            @"Content-Type":   MimeType,
            @"Content-Length": [NSString stringWithFormat:@"%lu", (unsigned long)Data.length],
            @"Cache-Control":  @"no-cache"
        }];

    [task didReceiveResponse:Response];
    [task didReceiveData:Data];
    [task didFinish];
}

- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task
{
    // No-op. WKWebView calls this to signal cancellation; since our reads are
    // synchronous (NSData dataWithContentsOfFile), there's nothing to abort.
}

- (void)failTask:(id<WKURLSchemeTask>)task withCode:(NSInteger)code reason:(NSString*)reason
{
    NSError* Err = [NSError errorWithDomain:@"InoWebUIScheme"
                                       code:code
                                   userInfo:@{NSLocalizedDescriptionKey: reason}];
    [task didFailWithError:Err];
}

- (NSString*)mimeTypeForPath:(NSString*)Path
{
    NSString* Ext = [[Path pathExtension] lowercaseString];
    if ([Ext isEqualToString:@"html"] || [Ext isEqualToString:@"htm"]) return @"text/html";
    if ([Ext isEqualToString:@"js"]   || [Ext isEqualToString:@"mjs"]) return @"application/javascript";
    if ([Ext isEqualToString:@"css"])    return @"text/css";
    if ([Ext isEqualToString:@"json"])   return @"application/json";
    if ([Ext isEqualToString:@"svg"])    return @"image/svg+xml";
    if ([Ext isEqualToString:@"png"])    return @"image/png";
    if ([Ext isEqualToString:@"jpg"]
     || [Ext isEqualToString:@"jpeg"])   return @"image/jpeg";
    if ([Ext isEqualToString:@"gif"])    return @"image/gif";
    if ([Ext isEqualToString:@"webp"])   return @"image/webp";
    if ([Ext isEqualToString:@"woff"])   return @"font/woff";
    if ([Ext isEqualToString:@"woff2"])  return @"font/woff2";
    if ([Ext isEqualToString:@"wasm"])   return @"application/wasm";
    return @"application/octet-stream";
}

@end

// ─────────────────────────────────────────────────────────────────────────────
//  InoWebViewBridge_iOS
//
//  One Obj-C delegate object per WKWebView, wired up at create time. Hosts
//  every WebKit callback we need:
//   • <WKScriptMessageHandler>    — JS → native bridge
//   • <WKNavigationDelegate>      — nav events + lockdown + crash
//   • <WKUIDelegate>              — JS dialogs + window.open
//
//  Owns the Instance ID for re-entry into C++ via DispatchOnGameThread.
// ─────────────────────────────────────────────────────────────────────────────
@interface InoWebViewBridge_iOS : NSObject <
    WKScriptMessageHandler,
    WKNavigationDelegate,
    WKUIDelegate>
@property (nonatomic, assign) int32 InstanceId;
/** Mirrors !FInoWebViewSettings::bAllowZoom from Initialize. When YES, the
 *  navigation delegate re-applies the scrollView zoom clamp + pinch
 *  recognizer disable after every page load — iOS otherwise re-derives the
 *  zoom range from the page's viewport meta and silently re-enables pinch. */
@property (nonatomic, assign) BOOL ShouldLockZoom;
@end

@implementation InoWebViewBridge_iOS

// ── WKScriptMessageHandler ───────────────────────────────────────────────────
- (void)userContentController:(WKUserContentController*)userContentController
      didReceiveScriptMessage:(WKScriptMessage*)message
{
    if (![message.name isEqualToString:kInoMessageHandler]) return;
    if (![message.body isKindOfClass:[NSString class]])     return;
    NSString* Body = (NSString*)message.body;
    const FString Envelope = FStringFromNSString(Body);
    DispatchOnGameThread(self.InstanceId, [Envelope](FInoWebViewImpl_iOS* Impl)
    {
        if (Impl->OnMessageReceivedJson) Impl->OnMessageReceivedJson(Envelope);
    });
}

// ── WKNavigationDelegate ─────────────────────────────────────────────────────
// iOS 13+ richer variant of decidePolicyForNavigationAction — the WKWebpagePreferences
// parameter lets us set per-navigation `allowsContentJavaScript` (iOS 14+), the
// Apple-recommended replacement for the deprecated global
// WKPreferences.javaScriptEnabled. WebKit calls THIS method instead of the
// 2-arg legacy variant on iOS 13+, so we only implement this one.
- (void)webView:(WKWebView*)webView
        decidePolicyForNavigationAction:(WKNavigationAction*)navigationAction
                            preferences:(WKWebpagePreferences*)preferences
                        decisionHandler:(void (^)(WKNavigationActionPolicy, WKWebpagePreferences*))decisionHandler
{
    NSString* URLStr = navigationAction.request.URL.absoluteString;
    if (URLStr == nil) URLStr = @"";
    const FString URI = FStringFromNSString(URLStr);

    // Lockdown + per-nav config — same rule as the other platforms.
    // ShouldAllowURI runs on the game thread normally; here we read the
    // cached config fields synchronously off the impl, which is safe
    // because they're only mutated during Initialize.
    bool bAllowed = true;
    bool bAllowJS = true;
    {
        FScopeLock Lock(&GRegistryLock);
        if (FInoWebViewImpl_iOS** Found = GImplRegistry.Find(self.InstanceId))
        {
            if (FInoWebViewImpl_iOS* Impl = *Found)
            {
                bAllowed = Impl->ShouldAllowURI(URI);
                bAllowJS = Impl->bAllowJavaScript;
            }
        }
    }

    DispatchOnGameThread(self.InstanceId, [URI](FInoWebViewImpl_iOS* Impl)
    {
        if (Impl->OnNavigationStartingCallback) Impl->OnNavigationStartingCallback(URI);
        Impl->SetCachedLoading(true);
    });

    // Push per-nav prefs. allowsContentJavaScript is iOS 14+; on iOS 13 the
    // property doesn't exist, so we silently skip (Config.bAllowJavaScript
    // is then governed by WKPreferences.javaScriptEnabled if anyone ever
    // sets that — we deliberately don't, to avoid the deprecation warning).
    if (@available(iOS 14.0, *))
    {
        preferences.allowsContentJavaScript = bAllowJS ? YES : NO;
    }

    if (!bAllowed)
    {
        UE_LOG(LogInoWebUI, Warning, TEXT("Navigation blocked by lockdown: %s"), *URI);
        decisionHandler(WKNavigationActionPolicyCancel, preferences);
        return;
    }
    decisionHandler(WKNavigationActionPolicyAllow, preferences);
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation
{
    // Re-assert the zoom-lock if the user disabled zoom in Config. iOS
    // re-derives scrollView.minimum/maximumZoomScale from the page's
    // viewport meta tag during navigation, and may also re-enable
    // pinchGestureRecognizer. Without this re-application, pinch quietly
    // works again the moment the first page finishes loading.
    if (self.ShouldLockZoom)
    {
        const BOOL bWasPinchEnabled = webView.scrollView.pinchGestureRecognizer.enabled;
        const CGFloat WasMinZoom    = webView.scrollView.minimumZoomScale;
        const CGFloat WasMaxZoom    = webView.scrollView.maximumZoomScale;
        const CGFloat WasZoomScale  = webView.scrollView.zoomScale;

        webView.scrollView.minimumZoomScale = 1.0;
        webView.scrollView.maximumZoomScale = 1.0;
        webView.scrollView.bouncesZoom      = NO;
        webView.scrollView.pinchGestureRecognizer.enabled = NO;
        // If iOS already moved the zoom scale, snap it back.
        if (webView.scrollView.zoomScale != 1.0)
        {
            [webView.scrollView setZoomScale:1.0 animated:NO];
        }

        // Aggressive pass: WKWebView attaches its OWN gesture recognizers to
        // itself (not just its scrollView), and on some iOS versions one of
        // those handles pinch independently of scrollView.pinchGestureRecognizer.
        // Walk both lists, find every UIPinchGestureRecognizer, and disable it.
        int32 DisabledOnWebView = 0;
        int32 DisabledOnScroll  = 0;
        for (UIGestureRecognizer* GR in webView.gestureRecognizers)
        {
            if ([GR isKindOfClass:[UIPinchGestureRecognizer class]] && GR.enabled)
            {
                GR.enabled = NO;
                ++DisabledOnWebView;
            }
        }
        for (UIGestureRecognizer* GR in webView.scrollView.gestureRecognizers)
        {
            if ([GR isKindOfClass:[UIPinchGestureRecognizer class]] && GR.enabled)
            {
                GR.enabled = NO;
                ++DisabledOnScroll;
            }
        }

        UE_LOG(LogInoWebUI, Log,
            TEXT("FInoWebViewImpl_iOS[%d] didFinishNavigation: re-applied zoom lock "
                 "(was: pinch=%d min=%.3f max=%.3f scale=%.3f -> now all clamped to 1.0; "
                 "extra pinch recognizers disabled on webView=%d, scrollView=%d)"),
            (int32)self.InstanceId,
            (int)bWasPinchEnabled, (double)WasMinZoom, (double)WasMaxZoom, (double)WasZoomScale,
            DisabledOnWebView, DisabledOnScroll);
    }

    NSString* URLStr = webView.URL.absoluteString;
    if (URLStr == nil) URLStr = @"";
    const FString URI = FStringFromNSString(URLStr);
    NSString* TitleStr = webView.title;
    const FString Title = FStringFromNSString(TitleStr ?: @"");
    const bool bBack    = webView.canGoBack;
    const bool bForward = webView.canGoForward;

    DispatchOnGameThread(self.InstanceId,
        [URI, Title, bBack, bForward](FInoWebViewImpl_iOS* Impl)
    {
        Impl->SetCachedURL(URI);
        if (!Title.IsEmpty()) Impl->SetCachedTitle(Title);
        Impl->SetCachedLoading(false);
        Impl->SetCachedNavState(bBack, bForward);
        if (Impl->OnNavigationCompletedCallback) Impl->OnNavigationCompletedCallback(true, URI);
    });
}

- (void)webView:(WKWebView*)webView didFailNavigation:(WKNavigation*)navigation
       withError:(NSError*)error
{
    NSString* URLStr = webView.URL.absoluteString;
    if (URLStr == nil) URLStr = @"";
    const FString URI = FStringFromNSString(URLStr);
    DispatchOnGameThread(self.InstanceId, [URI](FInoWebViewImpl_iOS* Impl)
    {
        Impl->SetCachedLoading(false);
        if (Impl->OnNavigationCompletedCallback) Impl->OnNavigationCompletedCallback(false, URI);
    });
}

- (void)webView:(WKWebView*)webView didFailProvisionalNavigation:(WKNavigation*)navigation
       withError:(NSError*)error
{
    NSString* URLStr = (error.userInfo[NSURLErrorFailingURLStringErrorKey]
                        ?: webView.URL.absoluteString) ?: @"";
    const FString URI = FStringFromNSString(URLStr);
    DispatchOnGameThread(self.InstanceId, [URI](FInoWebViewImpl_iOS* Impl)
    {
        Impl->SetCachedLoading(false);
        if (Impl->OnNavigationCompletedCallback) Impl->OnNavigationCompletedCallback(false, URI);
    });
}

- (void)webView:(WKWebView*)webView
      didCommitNavigation:(WKNavigation*)navigation
{
    // Title may have changed mid-navigation; broadcast asynchronously. The
    // canonical "title set" event is tracked via KVO below.
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView*)webView
{
    DispatchOnGameThread(self.InstanceId, [](FInoWebViewImpl_iOS* Impl)
    {
        if (Impl->OnProcessFailedCallback)
            Impl->OnProcessFailedCallback(TEXT("WKWebView content process terminated"));
    });
}

// ── WKUIDelegate ─────────────────────────────────────────────────────────────
- (void)webView:(WKWebView*)webView
       runJavaScriptAlertPanelWithMessage:(NSString*)message
                          initiatedByFrame:(WKFrameInfo*)frame
                          completionHandler:(void (^)(void))completionHandler
{
    const FString Msg = FStringFromNSString(message ?: @"");
    DispatchOnGameThread(self.InstanceId, [Msg](FInoWebViewImpl_iOS* Impl)
    {
        if (Impl->OnScriptDialogCallback)
            Impl->OnScriptDialogCallback(EInoScriptDialogKind::Alert, Msg);
    });
    // alert() has no return value — the only difference between
    // bAllowScriptDialogs on/off is whether the user briefly sees a real
    // UIAlertController. We suppress in both cases since iOS doesn't have a
    // standardized in-game presentation surface; the OnScriptDialog observer
    // lets consumer code show its own alert if needed.
    completionHandler();
}

- (void)webView:(WKWebView*)webView
       runJavaScriptConfirmPanelWithMessage:(NSString*)message
                            initiatedByFrame:(WKFrameInfo*)frame
                            completionHandler:(void (^)(BOOL result))completionHandler
{
    const FString Msg = FStringFromNSString(message ?: @"");
    bool bAllow = false;
    {
        FScopeLock Lock(&GRegistryLock);
        if (FInoWebViewImpl_iOS** Found = GImplRegistry.Find(self.InstanceId))
        {
            if (FInoWebViewImpl_iOS* Impl = *Found)
            {
                // Mirror Android: bAllowScriptDialogs=true makes confirm() return
                // true (i.e. user pretends to click OK) without showing UI.
                // bAllowScriptDialogs=false makes confirm() return false (cancel).
                bAllow = (Impl->bAllowScriptDialogs);
            }
        }
    }
    DispatchOnGameThread(self.InstanceId, [Msg](FInoWebViewImpl_iOS* Impl)
    {
        if (Impl->OnScriptDialogCallback)
            Impl->OnScriptDialogCallback(EInoScriptDialogKind::Confirm, Msg);
    });
    completionHandler(bAllow ? YES : NO);
}

- (void)webView:(WKWebView*)webView
       runJavaScriptTextInputPanelWithPrompt:(NSString*)prompt
                                  defaultText:(NSString*)defaultText
                              initiatedByFrame:(WKFrameInfo*)frame
                              completionHandler:(void (^)(NSString* result))completionHandler
{
    const FString Msg = FStringFromNSString(prompt ?: @"");
    bool bAllow = false;
    {
        FScopeLock Lock(&GRegistryLock);
        if (FInoWebViewImpl_iOS** Found = GImplRegistry.Find(self.InstanceId))
        {
            if (FInoWebViewImpl_iOS* Impl = *Found)
            {
                bAllow = (Impl->bAllowScriptDialogs);
            }
        }
    }
    DispatchOnGameThread(self.InstanceId, [Msg](FInoWebViewImpl_iOS* Impl)
    {
        if (Impl->OnScriptDialogCallback)
            Impl->OnScriptDialogCallback(EInoScriptDialogKind::Prompt, Msg);
    });
    if (bAllow)
    {
        // Mirror Android: prompt() returns the default text instead of null
        // when bAllowScriptDialogs is true (no UI shown either way).
        completionHandler(defaultText ?: @"");
    }
    else
    {
        completionHandler(nil);
    }
}

- (WKWebView*)webView:(WKWebView*)webView
        createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
                  forNavigationAction:(WKNavigationAction*)navigationAction
                       windowFeatures:(WKWindowFeatures*)windowFeatures
{
    NSString* URLStr = navigationAction.request.URL.absoluteString ?: @"";
    const FString URI = FStringFromNSString(URLStr);
    bool bAllow = false;
    {
        FScopeLock Lock(&GRegistryLock);
        if (FInoWebViewImpl_iOS** Found = GImplRegistry.Find(self.InstanceId))
        {
            if (FInoWebViewImpl_iOS* Impl = *Found)
            {
                bAllow = (Impl->bAllowNewWindows);
            }
        }
    }
    DispatchOnGameThread(self.InstanceId, [URI](FInoWebViewImpl_iOS* Impl)
    {
        if (Impl->OnNewWindowRequestedCallback) Impl->OnNewWindowRequestedCallback(URI);
    });
    // bAllowNewWindows=true: redirect into the same frame (parity with the
    // Windows / Android default-block behavior). bAllowNewWindows=false: just
    // block — the page can still observe via OnNewWindowRequested.
    if (bAllow && navigationAction.request != nil)
    {
        [webView loadRequest:navigationAction.request];
    }
    return nil;
}

// ── KVO ──────────────────────────────────────────────────────────────────────
// Observer for WKWebView.themeColor (iOS 15+). Registered in Initialize after
// the WebView is constructed; removed in Shutdown before the WebView is freed.
// Apple guarantees KVO change notifications fire on the main thread for
// WKWebView properties, so we marshal to the game thread via DispatchOnGameThread.
- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id>*)change
                       context:(void*)context
{
    if (![keyPath isEqualToString:@"themeColor"]) return;

    // change[NSKeyValueChangeNewKey] is NSNull when the page clears its
    // theme-color (or the page has none); UIColor when set. Treat both
    // cases as "no theme color" by broadcasting FLinearColor(0,0,0,0) —
    // alpha == 0 is the documented "cleared" signal in the BP delegate.
    id RawNew = change[NSKeyValueChangeNewKey];
    FLinearColor LC(0.0f, 0.0f, 0.0f, 0.0f);
    if ([RawNew isKindOfClass:[UIColor class]])
    {
        UIColor* Color = (UIColor*)RawNew;
        CGFloat R = 0, G = 0, B = 0, A = 0;
        // getRed:green:blue:alpha: returns NO if the UIColor isn't in an
        // RGB-compatible color space. Practically every theme-color from
        // a CSS string is sRGB; the few exotic display-P3 cases get reported
        // as their stored components (close enough for tinting UI).
        if ([Color getRed:&R green:&G blue:&B alpha:&A])
        {
            LC = FLinearColor((float)R, (float)G, (float)B, (float)A);
        }
    }

    DispatchOnGameThread(self.InstanceId, [LC](FInoWebViewImpl_iOS* Impl)
    {
        if (Impl->OnThemeColorChangedCallback) Impl->OnThemeColorChangedCallback(LC);
    });
}

@end

// ─────────────────────────────────────────────────────────────────────────────
//  Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────
FInoWebViewImpl_iOS::FInoWebViewImpl_iOS()
{
    InstanceId = GInstanceIdGenerator.Increment();
    InternalPtr = (void*)new FInoWebViewImpl_iOS_Internal();
}

FInoWebViewImpl_iOS::~FInoWebViewImpl_iOS()
{
    Shutdown();
    if (InternalPtr)
    {
        delete static_cast<FInoWebViewImpl_iOS_Internal*>(InternalPtr);
        InternalPtr = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Initialize
// ─────────────────────────────────────────────────────────────────────────────
bool FInoWebViewImpl_iOS::Initialize(void* /*ParentNativeHandle*/,
                                     const FInoWebViewConfig& Config)
{
    check(IsInGameThread());
    if (bDestroyed) return false;

    // Cache lockdown / hardening config so the Obj-C delegate can read it
    // without crossing the registry on every nav event.
    bLockToVirtualHost = Config.bLockToVirtualHost;
    bAllowScriptDialogs = Config.bAllowScriptDialogs;
    bAllowNewWindows    = Config.bAllowNewWindows;
    bAllowJavaScript    = Config.bAllowJavaScript;
    VirtualHostName     = Config.VirtualHostName.ToLower();
    AllowedURIPatterns  = Config.AllowedURIPatterns;

    // Resolve VirtualHostFolder to absolute (Project Content rooted) the same
    // way the Windows / Android impls do.
    FString AbsoluteFolder;
    if (!Config.VirtualHostName.IsEmpty() && !Config.VirtualHostFolder.IsEmpty())
    {
        AbsoluteFolder = FPaths::IsRelative(Config.VirtualHostFolder)
            ? FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / Config.VirtualHostFolder)
            : FPaths::ConvertRelativePathToFull(Config.VirtualHostFolder);
    }

    // Register in the global impl registry first so any callbacks that fire
    // before Initialize returns can find us. (Won't happen in practice — we
    // create the WebView synchronously below — but cheap insurance.)
    {
        FScopeLock Lock(&GRegistryLock);
        GImplRegistry.Add(InstanceId, this);
    }

    const int32 LocalId = InstanceId;
    auto* Internal = static_cast<FInoWebViewImpl_iOS_Internal*>(InternalPtr);
    if (!Internal) return false;

    NSString* AbsoluteFolderNS = AbsoluteFolder.IsEmpty()
        ? nil : NSStringFromFString(AbsoluteFolder);
    NSString* VirtualHostNS = Config.VirtualHostName.IsEmpty()
        ? nil : [NSStringFromFString(Config.VirtualHostName) lowercaseString];

    // Cross-platform UX: if the user passed https://<vhost>/... in the
    // config (the canonical form for Win64 / Android), translate to
    // inoweb://<vhost>/... here so it actually loads on iOS.
    const FString RewrittenInitialURL = RewriteForVHost(Config.InitialURL);
    NSString* InitialURLNS = RewrittenInitialURL.IsEmpty()
        ? nil : NSStringFromFString(RewrittenInitialURL);
    NSString* UserAgentNS = Config.View.UserAgentOverride.IsEmpty()
        ? nil : NSStringFromFString(Config.View.UserAgentOverride);

    const bool bTransparent      = Config.View.bTransparentBackground;
    const bool bVisibleOnCreate  = Config.View.bVisibleOnCreate;
    const bool bDevToolsEnabled  = Config.View.bEnableDevTools;
    const bool bMutedOnStart     = Config.View.bStartMuted;
    Internal->VirtualHostFolder  = AbsoluteFolderNS;

    // Build the WKWebView on the iOS main thread synchronously — we want IsReady
    // to be true by the time Initialize returns. dispatch_sync from the game
    // thread to main is safe (no deadlock risk: main is not the game thread).
    dispatch_sync(dispatch_get_main_queue(), ^{
        // Configuration
        WKWebViewConfiguration* Configuration = [[WKWebViewConfiguration alloc] init];

        // Inline media + autoplay — gated on the new view-settings flags.
        // Defaults match the previous hardcoded behaviour (inline=YES,
        // autoplay=allowed) so no observable change unless the user opts out.
        Configuration.allowsInlineMediaPlayback = Config.View.bAllowInlineMediaPlayback ? YES : NO;
        if (@available(iOS 10.0, *))
        {
            Configuration.mediaTypesRequiringUserActionForPlayback = Config.View.bAllowMediaAutoplay
                ? WKAudiovisualMediaTypeNone
                : WKAudiovisualMediaTypeAll;
        }

        // Per-WebView preferences (Phase 19+ hardening).
        // ────────────────────────────────────────────
        // WKPreferences governs page-wide behaviour. Most defaults are sane
        // but Apple's documented values have shifted across iOS versions —
        // we set the security-relevant ones explicitly so the contract is
        // stable across the supported range.
        WKPreferences* Preferences = Configuration.preferences;

        // Anti-phishing warning. iOS 14+. Default is YES; we set explicitly
        // so a future framework default change doesn't quietly weaken us.
        if (@available(iOS 14.0, *))
        {
            Preferences.fraudulentWebsiteWarningEnabled = YES;
        }

        // HTML5 fullscreen API (element.requestFullscreen). iOS 16+. Default
        // is NO in WKWebView, opt-in via Config.View.bAllowElementFullscreen.
        if (@available(iOS 16.0, *))
        {
            Preferences.elementFullscreenEnabled = Config.View.bAllowElementFullscreen ? YES : NO;
        }

        // System text-interaction UI (selection caret, magnifier loupe,
        // long-press callout). iOS 15+. Default YES; opt out for game UI
        // that doesn't want the iOS edit menu to appear over content.
        if (@available(iOS 15.0, *))
        {
            Preferences.textInteractionEnabled = Config.View.bAllowTextInteraction ? YES : NO;
        }

        // HTTPS upgrade for known-good hosts. iOS 15+. Default NO in WKWebView.
        // When YES, plaintext http:// loads to hosts WebKit knows support TLS
        // are silently retried as https:// — inline HSTS-style hardening.
        if (@available(iOS 15.0, *))
        {
            Configuration.upgradeKnownHostsToHTTPS = Config.bUpgradeHTTPToHTTPS ? YES : NO;
        }

        // ApplicationName for User-Agent (iOS 9+). Appends to the default
        // WebKit UA — preserves the WebKit version + platform info that
        // compatibility-detecting sites depend on. Mutually-non-exclusive
        // with View.UserAgentOverride below, but customUserAgent (set on
        // the WebView itself after creation) wins when both are present.
        if (!Config.View.ApplicationName.IsEmpty())
        {
            Configuration.applicationNameForUserAgent = NSStringFromFString(Config.View.ApplicationName);
        }

        // Bridge object — handles JS messages + nav delegate + UI delegate.
        InoWebViewBridge_iOS* Bridge = [[InoWebViewBridge_iOS alloc] init];
        Bridge.InstanceId    = LocalId;
        Bridge.ShouldLockZoom = Config.View.bAllowZoom ? NO : YES;

        // Custom scheme for virtual host. Must be set BEFORE the WKWebView is
        // created — WKWebViewConfiguration's scheme handlers are immutable
        // once a WebView has been initialized with it.
        if (AbsoluteFolderNS != nil && VirtualHostNS != nil)
        {
            InoWebViewSchemeHandler_iOS* SchemeHandler = [[InoWebViewSchemeHandler_iOS alloc] init];
            SchemeHandler.RootFolder = AbsoluteFolderNS;
            SchemeHandler.HostName   = VirtualHostNS;
            [Configuration setURLSchemeHandler:SchemeHandler forURLScheme:kInoVirtualScheme];
            Internal->SchemeHandler = SchemeHandler;
        }

        // User-content controller — one place to add scripts + message handlers.
        WKUserContentController* UCC = Configuration.userContentController;

        // Phase-19 hardening — content-world isolation.
        // ────────────────────────────────────────────
        // Install the WK message handler and bridge.js into
        // WKContentWorld.defaultClientWorld (iOS 14+) instead of the page's
        // main world. This makes `_InoWebUIHost` and `window.InoWebUI`
        // invisible to page scripts — a malicious or compromised page can
        // no longer monkey-patch the bridge to intercept UE↔JS traffic.
        //
        // The trade-off: page JS in pageWorld can't see window.InoWebUI
        // either. To preserve the cross-platform `window.InoWebUI.send /
        // on / off / once` API, bridge_ios.js is injected into BOTH worlds:
        //   - in defaultClientWorld it acts as a RELAY (forwards events
        //     between pageWorld DOM events and bridge.js's send/dispatch);
        //   - in pageWorld it acts as a SHIM (re-exposes window.InoWebUI
        //     as a thin facade that talks to the relay via DOM events).
        //
        // Project min target is iOS 16 so the iOS 14 APIs are unconditionally
        // available, but we keep the @available guard for documentation.
        WKContentWorld* BridgeWorld = nil;
        if (@available(iOS 14.0, *))
        {
            BridgeWorld = WKContentWorld.defaultClientWorld;
        }

        if (BridgeWorld != nil)
        {
            [UCC addScriptMessageHandler:Bridge contentWorld:BridgeWorld name:kInoMessageHandler];
        }
        else
        {
            // Pre-iOS 14 fallback: page world. (Unreachable with iOS 16 min.)
            [UCC addScriptMessageHandler:Bridge name:kInoMessageHandler];
        }

        // bridge.js → defaultClientWorld. Same source as Win64 / Android;
        // when it detects window.webkit.messageHandlers._InoWebUIHost (which
        // is only visible in this world), it sets up window.InoWebUI here.
        WKUserScript* BridgeScript = (BridgeWorld != nil)
            ? [[WKUserScript alloc] initWithSource:GInoWebUIBridgeScript
                                     injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                  forMainFrameOnly:YES
                                    inContentWorld:BridgeWorld]
            : [[WKUserScript alloc] initWithSource:GInoWebUIBridgeScript
                                     injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                  forMainFrameOnly:YES];
        [UCC addUserScript:BridgeScript];

        // bridge_ios.js → defaultClientWorld (acts as RELAY). MUST be added
        // AFTER bridge.js so the relay's wrap of _InoWebUIDispatch sees the
        // bridge.js-installed value.
        if (BridgeWorld != nil)
        {
            WKUserScript* WorldBridgeIsolated =
                [[WKUserScript alloc] initWithSource:GInoWebUIIOSWorldBridgeScript
                                       injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                    forMainFrameOnly:YES
                                      inContentWorld:BridgeWorld];
            [UCC addUserScript:WorldBridgeIsolated];

            // bridge_ios.js → pageWorld (acts as SHIM). _InoWebUIHost is
            // absent here, which is how the script knows it's in pageWorld.
            WKUserScript* WorldBridgePage =
                [[WKUserScript alloc] initWithSource:GInoWebUIIOSWorldBridgeScript
                                       injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                    forMainFrameOnly:YES
                                      inContentWorld:WKContentWorld.pageWorld];
            [UCC addUserScript:WorldBridgePage];
        }

        // Dev overlay (gated by the same flag as the other platforms).
        // Lives in defaultClientWorld so it uses bridge.js's window.InoWebUI
        // directly without going through the relay/shim DOM-event hop. The
        // overlay's DOM elements (FAB, modal) are visible from pageWorld
        // because the DOM is shared across content worlds — only the JS
        // namespaces are isolated.
        if (bDevToolsEnabled)
        {
            WKUserScript* DevOverlay = (BridgeWorld != nil)
                ? [[WKUserScript alloc] initWithSource:GInoWebUIDevToolsOverlayScript
                                         injectionTime:WKUserScriptInjectionTimeAtDocumentEnd
                                      forMainFrameOnly:YES
                                        inContentWorld:BridgeWorld]
                : [[WKUserScript alloc] initWithSource:GInoWebUIDevToolsOverlayScript
                                         injectionTime:WKUserScriptInjectionTimeAtDocumentEnd
                                      forMainFrameOnly:YES];
            [UCC addUserScript:DevOverlay];
        }

        // ── Initial cookies ─────────────────────────────────────────────
        // WKWebsiteDataStore.httpCookieStore is async; we fire-and-forget here.
        // Order is preserved (FIFO completion handlers) so the navigation
        // request below sees the cookies in place. If reliability becomes an
        // issue we can dispatch_group_wait, but Apple's contract is good enough
        // for game UI sessions.
        WKWebsiteDataStore* DataStore = WKWebsiteDataStore.defaultDataStore;
        for (const FInoInitialCookie& C : Config.InitialCookies)
        {
            // We can't call SetCookie here directly (impl is mid-init); inline
            // the parse to avoid a re-entrant dispatch.
            NSString* URLString    = NSStringFromFString(C.URL);
            NSString* CookieString = NSStringFromFString(C.Cookie);
            NSURL* URLObj = [NSURL URLWithString:URLString];
            if (URLObj == nil) continue;

            // Parse "name=value; Path=/; ..." into NSHTTPCookie properties.
            NSMutableDictionary* Props = [NSMutableDictionary dictionary];
            NSArray<NSString*>* Parts = [CookieString componentsSeparatedByString:@";"];
            BOOL bGotNameValue = NO;
            for (NSString* RawPart in Parts)
            {
                NSString* Part = [RawPart stringByTrimmingCharactersInSet:
                                  [NSCharacterSet whitespaceCharacterSet]];
                if (Part.length == 0) continue;
                NSRange Eq = [Part rangeOfString:@"="];
                NSString* K = (Eq.location == NSNotFound) ? Part : [Part substringToIndex:Eq.location];
                NSString* V = (Eq.location == NSNotFound) ? @""  : [Part substringFromIndex:Eq.location + 1];
                if (!bGotNameValue)
                {
                    Props[NSHTTPCookieName]   = K;
                    Props[NSHTTPCookieValue]  = V;
                    bGotNameValue = YES;
                }
                else
                {
                    NSString* KLow = [K lowercaseString];
                    if      ([KLow isEqualToString:@"path"])    Props[NSHTTPCookiePath]    = V;
                    else if ([KLow isEqualToString:@"domain"])  Props[NSHTTPCookieDomain]  = V;
                    else if ([KLow isEqualToString:@"expires"]) Props[NSHTTPCookieExpires] = V;
                    else if ([KLow isEqualToString:@"secure"])  Props[NSHTTPCookieSecure]  = @YES;
                }
            }
            if (Props[NSHTTPCookieDomain] == nil) Props[NSHTTPCookieDomain] = URLObj.host ?: @"";
            if (Props[NSHTTPCookiePath]   == nil) Props[NSHTTPCookiePath]   = @"/";
            NSHTTPCookie* Cookie = [NSHTTPCookie cookieWithProperties:Props];
            if (Cookie != nil)
            {
                [DataStore.httpCookieStore setCookie:Cookie completionHandler:nil];
            }
        }

        // Determine the parent view (auto-rotating UIView). Add the WKWebView
        // as a sibling subview ON TOP of it so the OS compositor blends.
        IOSAppDelegate* AppDelegate = [IOSAppDelegate GetDelegate];
        UIView* ParentView = AppDelegate.RootView;
        if (ParentView == nil)
        {
            // Fall back to the key window; very early-create case.
            ParentView = AppDelegate.Window;
        }
        const CGRect InitialFrame = (ParentView != nil)
            ? ParentView.bounds
            : [UIScreen mainScreen].bounds;

        WKWebView* WebView = [[WKWebView alloc] initWithFrame:InitialFrame
                                                configuration:Configuration];
        WebView.navigationDelegate = Bridge;
        WebView.UIDelegate         = Bridge;
        WebView.hidden             = !bVisibleOnCreate;
        WebView.allowsBackForwardNavigationGestures = NO; // game UI; we control nav

        // Safari Web Inspector attachability (iOS 16.4+). Defaults to NO on
        // every build configuration, so without this, Web Inspector silently
        // fails to attach to TestFlight / Ad-Hoc / Release builds even on a
        // dev device. Gated by bEnableDevTools so shipping builds don't
        // expose the page to anyone with a Mac and a cable.
        //
        // Pre-iOS 16.4 behaviour (unreachable with iOS 16 min if devs ship
        // 16.4+, but covers 16.0–16.3): debug builds are inspectable by
        // default, release builds never were. Nothing to do for those.
        if (@available(iOS 16.4, *))
        {
            WebView.inspectable = bDevToolsEnabled ? YES : NO;
        }

        // bExtendUnderSafeArea (default true) → contentInsetAdjustmentBehavior
        // = Never so content extends edge-to-edge under the notch / home
        // indicator. False → Automatic (the standard inset-by-safe-area).
        if (@available(iOS 11.0, *))
        {
            WebView.scrollView.contentInsetAdjustmentBehavior = Config.View.bExtendUnderSafeArea
                ? UIScrollViewContentInsetAdjustmentNever
                : UIScrollViewContentInsetAdjustmentAutomatic;
        }

        // bAllowBounceOnScroll (default false) → suppress the iOS rubber-band
        // over-scroll effect. Scrolling within the content range still works
        // normally; only the elastic past-the-edge bounce is gated.
        WebView.scrollView.bounces = Config.View.bAllowBounceOnScroll ? YES : NO;

        // bAllowZoom (default false) → kill pinch / double-tap zoom natively.
        //   • min/max=1.0 clamps the scrollView zoom range. This alone is
        //     fragile — iOS re-derives the range from the page's viewport
        //     meta tag on every navigation, so the clamp can quietly drift.
        //   • pinchGestureRecognizer.enabled=NO is the robust line: once
        //     disabled, iOS doesn't re-enable it across page loads.
        // Note: this does NOT prevent the iOS input-focus auto-zoom — that's
        // a separate WebKit viewport-scaling mechanism. The reliable fix for
        // input auto-zoom is HTML-side: either a viewport meta tag with
        // `maximum-scale=1, user-scalable=no`, or CSS `input { font-size:16px }`
        // (iOS only auto-zooms when the focused input's font-size < 16px).
        if (!Config.View.bAllowZoom)
        {
            WebView.scrollView.minimumZoomScale = 1.0;
            WebView.scrollView.maximumZoomScale = 1.0;
            WebView.scrollView.bouncesZoom      = NO;
            WebView.scrollView.pinchGestureRecognizer.enabled = NO;

            // Disable any other UIPinchGestureRecognizer on the WebView
            // itself or its scrollView. WKWebView sometimes attaches its
            // own pinch handlers independent of scrollView.pinch.
            for (UIGestureRecognizer* GR in WebView.gestureRecognizers)
            {
                if ([GR isKindOfClass:[UIPinchGestureRecognizer class]])
                    GR.enabled = NO;
            }
            for (UIGestureRecognizer* GR in WebView.scrollView.gestureRecognizers)
            {
                if ([GR isKindOfClass:[UIPinchGestureRecognizer class]])
                    GR.enabled = NO;
            }
        }

        // bShowScrollBars (default false) → hide the scroll indicators.
        // Scrolling itself stays enabled.
        WebView.scrollView.showsVerticalScrollIndicator   = Config.View.bShowScrollBars ? YES : NO;
        WebView.scrollView.showsHorizontalScrollIndicator = Config.View.bShowScrollBars ? YES : NO;

        // Background opacity — match the FInoWebViewConfig contract.
        if (bTransparent)
        {
            WebView.opaque = NO;
            WebView.backgroundColor = UIColor.clearColor;
            if ([WebView.scrollView respondsToSelector:@selector(setBackgroundColor:)])
            {
                WebView.scrollView.backgroundColor = UIColor.clearColor;
            }
        }
        else
        {
            WebView.opaque = YES;
            WebView.backgroundColor = UIColor.whiteColor;
        }

        // underPageBackgroundColor (iOS 15+) — the color shown in the
        // overscroll area when the user rubber-band scrolls past the page
        // edges. Without this, iOS paints a system-default light color
        // there, which leaks through the overlay's transparent gutter and
        // breaks the see-through illusion. Match the WebView's own
        // background opacity. SetBackgroundOpaque also re-syncs this at
        // runtime when the toggle flips.
        if (@available(iOS 15.0, *))
        {
            WebView.underPageBackgroundColor = bTransparent
                ? UIColor.clearColor
                : UIColor.whiteColor;
        }

        // KVO on themeColor (iOS 15+) — fires the OnThemeColorChanged BP
        // delegate when the page sets / updates / clears its <meta
        // name="theme-color">. Observer is the Bridge; cleanup happens in
        // Shutdown before the WebView is released. NSKeyValueObservingOptionInitial
        // is deliberately NOT used: the BP delegate may not be bound when
        // Initialize returns (UInoWebView defers OnReady to the next game
        // tick), so an initial broadcast would land before any subscriber
        // exists. Real page-side theme-color changes always fire later.
        if (@available(iOS 15.0, *))
        {
            [WebView addObserver:Bridge
                      forKeyPath:@"themeColor"
                         options:NSKeyValueObservingOptionNew
                         context:nullptr];
        }

        // User-Agent override.
        if (UserAgentNS != nil) { WebView.customUserAgent = UserAgentNS; }

        // bStartMuted: no-op on iOS. WKWebView has no first-class audio mute
        // API and we deliberately don't synthesise one via JS — keeps the
        // page free of plugin-injected scripts beyond the bridge. Same
        // posture as Android, which logs the same warning.
        if (bMutedOnStart)
        {
            UE_LOG(LogInoWebUI, Warning,
                TEXT("FInoWebViewImpl_iOS: bStartMuted is a no-op on iOS. "
                     "WKWebView has no native mute API; mute individual "
                     "<audio>/<video> elements from your page if needed."));
        }

        // Layout: autoresize to follow the parent's bounds in auto-mode. The
        // C++ side flips between auto/manual via SetBoundsMode + SyncBounds.
        WebView.autoresizingMask = UIViewAutoresizingFlexibleWidth
                                 | UIViewAutoresizingFlexibleHeight;

        // Add as a sibling subview ABOVE the FIOSView (the Metal-layer game
        // surface). The OS compositor blends the two; transparent pixels in
        // the WebView's background reveal the UE scene.
        if (ParentView != nil)
        {
            [ParentView addSubview:WebView];
        }
        else
        {
            UE_LOG(LogInoWebUI, Error, TEXT("FInoWebViewImpl_iOS::Initialize: no RootView available"));
        }

        Internal->Configuration = Configuration;
        Internal->Bridge        = Bridge;
        Internal->WebView       = WebView;

        // Initial navigation.
        if (InitialURLNS != nil)
        {
            // Use LoadURLWithHeaders if any configured — same shape as the
            // other platforms.
            if (Config.InitialHeaders.Num() > 0)
            {
                NSURL* URLObj = [NSURL URLWithString:InitialURLNS];
                if (URLObj != nil)
                {
                    NSMutableURLRequest* Req = [NSMutableURLRequest requestWithURL:URLObj];
                    for (const TPair<FString, FString>& KV : Config.InitialHeaders)
                    {
                        [Req setValue:NSStringFromFString(KV.Value)
                            forHTTPHeaderField:NSStringFromFString(KV.Key)];
                    }
                    [WebView loadRequest:Req];
                }
            }
            else
            {
                NSURL* URLObj = [NSURL URLWithString:InitialURLNS];
                if (URLObj != nil)
                {
                    [WebView loadRequest:[NSURLRequest requestWithURL:URLObj]];
                }
            }
        }
    });

    if (!Config.InitialURL.IsEmpty())
    {
        // Cache the URL we actually loaded — post-rewrite — so GetURL()
        // matches the WebView's location.href once the page is up.
        CachedURL = RewrittenInitialURL;
    }
    bReady = true;

    UE_LOG(LogInoWebUI, Log,
        TEXT("FInoWebViewImpl_iOS[%d] Initialize  url='%s'  transparent=%d  visible=%d  vhost='%s'  bAllowZoom=%d"),
        InstanceId, *Config.InitialURL,
        Config.View.bTransparentBackground ? 1 : 0,
        Config.View.bVisibleOnCreate ? 1 : 0,
        *VirtualHostName,
        Config.View.bAllowZoom ? 1 : 0);

    if (OnReadyCallback)
    {
        OnReadyCallback();
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Lockdown
// ─────────────────────────────────────────────────────────────────────────────
FString FInoWebViewImpl_iOS::RewriteForVHost(const FString& URL) const
{
    if (VirtualHostName.IsEmpty()) return URL;

    auto Try = [&](const TCHAR* Scheme) -> FString
    {
        const FString Prefix = FString(Scheme) + VirtualHostName;
        if (!URL.StartsWith(Prefix, ESearchCase::IgnoreCase)) return FString();
        // Boundary: end-of-string, '/', '?', '#', or ':' (port). Otherwise we'd
        // wrongly rewrite e.g. "https://ino.local.attacker.com/..." when the
        // configured vhost is "ino.local".
        const int32 N = Prefix.Len();
        if (URL.Len() != N)
        {
            const TCHAR C = URL[N];
            if (C != TEXT('/') && C != TEXT('?') && C != TEXT('#') && C != TEXT(':'))
                return FString();
        }
        return FString(TEXT("inoweb://")) + VirtualHostName + URL.Mid(N);
    };

    FString R = Try(TEXT("https://"));
    if (R.IsEmpty()) R = Try(TEXT("http://"));
    if (R.IsEmpty()) return URL;

    UE_LOG(LogInoWebUI, Verbose,
        TEXT("FInoWebViewImpl_iOS[%d]: rewrote '%s' -> '%s' (vhost auto-translation)"),
        InstanceId, *URL, *R);
    return R;
}

bool FInoWebViewImpl_iOS::ShouldAllowURI(const FString& URI) const
{
    if (URI.IsEmpty()) return true;
    if (URI.StartsWith(TEXT("about:"))
     || URI.StartsWith(TEXT("data:"))
     || URI.StartsWith(TEXT("blob:")))
    {
        return true;
    }
    // Custom virtual-host scheme is always allowed (parity with virtual-host-
    // whole-host on Windows/Android). Use the bare scheme prefix; the host
    // match below covers the same logical case for any other scheme.
    if (URI.StartsWith(TEXT("inoweb:"))) return true;

    if (!bLockToVirtualHost) return true;

    // Whole-host match against VirtualHostName, case-insensitive.
    if (!VirtualHostName.IsEmpty())
    {
        // Extract host from URI (same shape as Java extractHost / Windows IsURIAllowed).
        const int32 SchemeEnd = URI.Find(TEXT("://"));
        if (SchemeEnd != INDEX_NONE)
        {
            FString Rest = URI.Mid(SchemeEnd + 3);
            int32 At = INDEX_NONE;
            if (Rest.FindChar(TEXT('@'), At))
            {
                Rest = Rest.Mid(At + 1);
            }
            int32 End = Rest.Len();
            for (TCHAR Ch : { TEXT('/'), TEXT('?'), TEXT('#'), TEXT(':') })
            {
                int32 Idx = INDEX_NONE;
                if (Rest.FindChar(Ch, Idx) && Idx < End)
                {
                    End = Idx;
                }
            }
            const FString Host = Rest.Left(End).ToLower();
            if (Host == VirtualHostName) return true;
        }
    }

    for (const FString& Pattern : AllowedURIPatterns)
    {
        if (InoMatchesWildcard(URI, Pattern)) return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Navigation / visibility / bounds
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_iOS::Navigate(const FString& URL)
{
    check(IsInGameThread());
    if (!bReady || bDestroyed) return;

    auto* Internal = static_cast<FInoWebViewImpl_iOS_Internal*>(InternalPtr);
    if (!Internal) return;

    // Cross-platform UX: rewrite https://<vhost>/... → inoweb://<vhost>/...
    // so the same FString URL works across all three platforms.
    const FString Rewritten = RewriteForVHost(URL);
    NSString* URLStr = NSStringFromFString(Rewritten);
    dispatch_async(dispatch_get_main_queue(), ^{
        if (Internal->WebView == nil) return;
        NSURL* URLObj = [NSURL URLWithString:URLStr];
        if (URLObj == nil) return;
        [Internal->WebView loadRequest:[NSURLRequest requestWithURL:URLObj]];
    });
    bCachedLoading = true;
}

void FInoWebViewImpl_iOS::Reload()
{
    check(IsInGameThread());
    if (!bReady || bDestroyed) return;
    auto* Internal = static_cast<FInoWebViewImpl_iOS_Internal*>(InternalPtr);
    if (!Internal) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        if (Internal->WebView != nil) [Internal->WebView reload];
    });
}

void FInoWebViewImpl_iOS::SetVisible(bool bVisible)
{
    check(IsInGameThread());
    if (!bReady || bDestroyed) return;
    auto* Internal = static_cast<FInoWebViewImpl_iOS_Internal*>(InternalPtr);
    if (!Internal) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        if (Internal->WebView != nil) Internal->WebView.hidden = !bVisible;
    });
}

void FInoWebViewImpl_iOS::SyncBounds(int32 ScreenX, int32 ScreenY,
                                     int32 Width, int32 Height)
{
    check(IsInGameThread());
    if (!bReady || bDestroyed) return;
    auto* Internal = static_cast<FInoWebViewImpl_iOS_Internal*>(InternalPtr);
    if (!Internal) return;

    const bool bManual = bManualBounds;
    const int32 X = ScreenX, Y = ScreenY, W = Width, H = Height;

    dispatch_async(dispatch_get_main_queue(), ^{
        WKWebView* WebView = Internal->WebView;
        if (WebView == nil) return;

        if (!bManual)
        {
            // Auto mode — follow the parent's bounds. autoresizingMask handles
            // the per-frame case; here we re-sync explicitly so a config change
            // (orientation, split-view) propagates without waiting for a layout
            // pass.
            UIView* Parent = WebView.superview;
            if (Parent != nil)
            {
                WebView.frame = Parent.bounds;
            }
            return;
        }

        // Manual mode — UE pixels → UIKit points via the screen scale.
        const CGFloat Scale = [UIScreen mainScreen].scale;
        if (Scale <= 0.0)
        {
            WebView.frame = CGRectMake(X, Y, W, H);
            return;
        }
        WebView.frame = CGRectMake((CGFloat)X / Scale,
                                   (CGFloat)Y / Scale,
                                   (CGFloat)W / Scale,
                                   (CGFloat)H / Scale);
    });
}

void FInoWebViewImpl_iOS::SetBoundsMode(bool bManual)
{
    check(IsInGameThread());
    if (bDestroyed) return;
    bManualBounds = bManual;
    auto* Internal = static_cast<FInoWebViewImpl_iOS_Internal*>(InternalPtr);
    if (!Internal) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        WKWebView* WebView = Internal->WebView;
        if (WebView == nil) return;
        // In auto mode, restore autoresizing so the frame tracks the parent.
        // In manual mode, drop autoresizing so SyncBounds fully owns frame.
        WebView.autoresizingMask = bManual
            ? UIViewAutoresizingNone
            : (UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight);
        if (!bManual && WebView.superview != nil)
        {
            WebView.frame = WebView.superview.bounds;
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
//  Shutdown
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_iOS::Shutdown()
{
    check(IsInGameThread());
    if (bDestroyed) return;
    bDestroyed = true;
    bReady     = false;

    // Unregister BEFORE tearing down the WKWebView so any in-flight
    // delegate callback that lands on the game thread post-Shutdown sees
    // an empty registry and silently no-ops.
    {
        FScopeLock Lock(&GRegistryLock);
        GImplRegistry.Remove(InstanceId);
    }

    auto* Internal = static_cast<FInoWebViewImpl_iOS_Internal*>(InternalPtr);
    if (!Internal) return;

    // dispatch_sync (not _async) here for two reasons:
    //  1) Drain any earlier dispatch_async blocks that may still hold the
    //     pre-shutdown WebView pointer — the runloop is FIFO so by the time
    //     this sync block runs, every prior queued block has run.
    //  2) Once Shutdown returns to the game thread, the destructor will
    //     `delete InternalPtr`. We MUST be done with main-thread work before
    //     that or the freed struct gets dereferenced.
    // Game thread != main thread on iOS, so this can't deadlock.
    dispatch_sync(dispatch_get_main_queue(), ^{
        WKWebView* WebView = Internal->WebView;
        if (WebView != nil)
        {
            // KVO cleanup must precede delegate teardown so a final change
            // notification can't fire on a Bridge whose Impl pointer is
            // already in the process of being released. @try/@catch covers
            // the "not registered" exception (iOS 14 / 15-but-failed paths).
            if (@available(iOS 15.0, *))
            {
                @try
                {
                    [WebView removeObserver:Internal->Bridge forKeyPath:@"themeColor"];
                }
                @catch (NSException* /*Unused*/) {}
            }

            WebView.navigationDelegate = nil;
            WebView.UIDelegate         = nil;
            [WebView stopLoading];
            [WebView removeFromSuperview];

            // Drop the script-message handler so the bridge doesn't leak.
            // Use the contentWorld-aware removal on iOS 14+ because we
            // installed the handler into defaultClientWorld (see Initialize).
            // The legacy removeScriptMessageHandlerForName: only looks in
            // pageWorld and would leave the defaultClientWorld registration
            // dangling — Shutdown becomes a slow leak.
            if (Internal->Configuration.userContentController != nil)
            {
                if (@available(iOS 14.0, *))
                {
                    [Internal->Configuration.userContentController
                        removeScriptMessageHandlerForName:kInoMessageHandler
                                             contentWorld:WKContentWorld.defaultClientWorld];
                }
                else
                {
                    [Internal->Configuration.userContentController
                        removeScriptMessageHandlerForName:kInoMessageHandler];
                }
            }
        }
        Internal->WebView       = nil;
        Internal->Bridge        = nil;
        Internal->SchemeHandler = nil;
        Internal->Configuration = nil;
    });

    UE_LOG(LogInoWebUI, Log, TEXT("FInoWebViewImpl_iOS[%d] Shutdown"), InstanceId);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Phase 2 messaging
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_iOS::PostMessageJson(const FString& Json)
{
    check(IsInGameThread());
    if (bDestroyed) return;
    auto* Internal = static_cast<FInoWebViewImpl_iOS_Internal*>(InternalPtr);
    if (!Internal) return;

    NSString* JSLiteral = InoJSStringLiteral(Json);
    dispatch_async(dispatch_get_main_queue(), ^{
        WKWebView* WebView = Internal->WebView;
        if (WebView == nil) return;
        NSString* Script =
            [NSString stringWithFormat:@"window._InoWebUIDispatch && window._InoWebUIDispatch(%@)", JSLiteral];

        // Run in defaultClientWorld — that's where bridge.js installed
        // _InoWebUIDispatch (Phase-19 content-world isolation). The legacy
        // evaluateJavaScript:completionHandler: would run in pageWorld and
        // the call would silently no-op (the global doesn't exist there).
        // bridge_ios.js's relay then echoes the message to pageWorld via a
        // DOM CustomEvent so the pageWorld shim's listeners fire too.
        if (@available(iOS 14.0, *))
        {
            [WebView evaluateJavaScript:Script
                                     in:nil  // main frame
                         inContentWorld:WKContentWorld.defaultClientWorld
                      completionHandler:nil];
        }
        else
        {
            // Pre-iOS 14 fallback — runs in pageWorld where bridge.js's
            // legacy install path put _InoWebUIDispatch. Unreachable with
            // iOS 16 min target.
            [WebView evaluateJavaScript:Script completionHandler:nil];
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
//  Phase 3 polish
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_iOS::OpenDevTools()
{
    UE_LOG(LogInoWebUI, Log,
        TEXT("OpenDevTools on iOS: connect this device via USB to a Mac, open Safari, "
             "enable the Develop menu (Safari → Settings → Advanced → Show features for web developers), "
             "and pick this WKWebView from Develop → <DeviceName>. "
             "Inline DevTools is not available on iOS. "
             "NOTE: on iOS 16.4+, WKWebView is only attachable to Safari Web "
             "Inspector when WKWebView.inspectable=YES — gated here by "
             "FInoWebViewConfig::bEnableDevTools. Below iOS 16.4, debug builds "
             "were always inspectable and release builds never were."));
}

void FInoWebViewImpl_iOS::ExecuteJavaScript(const FString& Code)
{
    check(IsInGameThread());
    if (bDestroyed || Code.IsEmpty()) return;
    auto* Internal = static_cast<FInoWebViewImpl_iOS_Internal*>(InternalPtr);
    if (!Internal) return;

    NSString* CodeNS = NSStringFromFString(Code);
    dispatch_async(dispatch_get_main_queue(), ^{
        WKWebView* WebView = Internal->WebView;
        if (WebView == nil) return;
        [WebView evaluateJavaScript:CodeNS completionHandler:nil];
    });
}

void FInoWebViewImpl_iOS::SetMuted(bool /*bMuted*/)
{
    check(IsInGameThread());
    if (bDestroyed) return;

    // WKWebView has no first-class audio mute API. Earlier versions of this
    // impl walked <audio>/<video> elements via JS as a best-effort
    // approximation; that's been removed to keep the plugin from injecting
    // scripts into the page beyond the bridge. Same posture as Android.
    // Mute individual media elements from your own page JS if you need it.
    UE_LOG(LogInoWebUI, Warning,
        TEXT("SetMuted on iOS: not supported. WKWebView has no native mute API. "
             "Mute individual <audio>/<video> elements via your own page JS."));
}

void FInoWebViewImpl_iOS::FocusWebView()
{
    check(IsInGameThread());
    if (bDestroyed) return;
    auto* Internal = static_cast<FInoWebViewImpl_iOS_Internal*>(InternalPtr);
    if (!Internal) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        WKWebView* WebView = Internal->WebView;
        if (WebView != nil) [WebView becomeFirstResponder];
    });
}

void FInoWebViewImpl_iOS::SetZoomFactor(float Factor)
{
    check(IsInGameThread());
    if (bDestroyed) return;
    auto* Internal = static_cast<FInoWebViewImpl_iOS_Internal*>(InternalPtr);
    if (!Internal) return;

    // WKWebView.pageZoom is iOS 14+ (1.0 = 100%, persists across navigations).
    // Project min target is iOS 16, so the @available is just documentation.
    // Below iOS 14 the property doesn't exist and the call would silently
    // no-op via Obj-C's nil-receiver semantics — but we'd rather log it.
    CachedZoomFactor = Factor;
    const CGFloat Z = (CGFloat)Factor;
    dispatch_async(dispatch_get_main_queue(), ^{
        WKWebView* WebView = Internal->WebView;
        if (WebView == nil) return;
        if (@available(iOS 14.0, *))
        {
            WebView.pageZoom = Z;
        }
        else
        {
            UE_LOG(LogInoWebUI, Warning,
                TEXT("SetZoomFactor on iOS <14: not supported. "
                     "WKWebView.pageZoom was added in iOS 14."));
        }
    });
}

void FInoWebViewImpl_iOS::ClearAllCookies()
{
    check(IsInGameThread());
    if (bDestroyed) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSSet* Types = [NSSet setWithObject:WKWebsiteDataTypeCookies];
        [WKWebsiteDataStore.defaultDataStore
            removeDataOfTypes:Types
                modifiedSince:[NSDate distantPast]
            completionHandler:^{}];
    });
}

void FInoWebViewImpl_iOS::SetBackgroundOpaque(bool bOpaque)
{
    check(IsInGameThread());
    if (bDestroyed) return;
    auto* Internal = static_cast<FInoWebViewImpl_iOS_Internal*>(InternalPtr);
    if (!Internal) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        WKWebView* WebView = Internal->WebView;
        if (WebView == nil) return;
        if (bOpaque)
        {
            WebView.opaque = YES;
            WebView.backgroundColor = UIColor.whiteColor;
            if ([WebView.scrollView respondsToSelector:@selector(setBackgroundColor:)])
            {
                WebView.scrollView.backgroundColor = UIColor.whiteColor;
            }
        }
        else
        {
            WebView.opaque = NO;
            WebView.backgroundColor = UIColor.clearColor;
            if ([WebView.scrollView respondsToSelector:@selector(setBackgroundColor:)])
            {
                WebView.scrollView.backgroundColor = UIColor.clearColor;
            }
        }
        // Re-sync the overscroll-area color (iOS 15+) so the rubber-band
        // bounce reveals the same color as the WebView background instead
        // of the system default light grey.
        if (@available(iOS 15.0, *))
        {
            WebView.underPageBackgroundColor = bOpaque
                ? UIColor.whiteColor
                : UIColor.clearColor;
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
//  Browser-style nav
// ─────────────────────────────────────────────────────────────────────────────
void FInoWebViewImpl_iOS::GoBack()
{
    check(IsInGameThread());
    if (!bReady || bDestroyed) return;
    auto* Internal = static_cast<FInoWebViewImpl_iOS_Internal*>(InternalPtr);
    if (!Internal) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        WKWebView* WebView = Internal->WebView;
        if (WebView != nil && WebView.canGoBack) [WebView goBack];
    });
}

void FInoWebViewImpl_iOS::GoForward()
{
    check(IsInGameThread());
    if (!bReady || bDestroyed) return;
    auto* Internal = static_cast<FInoWebViewImpl_iOS_Internal*>(InternalPtr);
    if (!Internal) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        WKWebView* WebView = Internal->WebView;
        if (WebView != nil && WebView.canGoForward) [WebView goForward];
    });
}

bool FInoWebViewImpl_iOS::CanGoBack() const
{
    if (!bReady || bDestroyed) return false;
    return bCachedCanGoBack;
}

bool FInoWebViewImpl_iOS::CanGoForward() const
{
    if (!bReady || bDestroyed) return false;
    return bCachedCanGoForward;
}

void FInoWebViewImpl_iOS::StopLoading()
{
    check(IsInGameThread());
    if (!bReady || bDestroyed) return;
    auto* Internal = static_cast<FInoWebViewImpl_iOS_Internal*>(InternalPtr);
    if (!Internal) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (Internal->WebView != nil) [Internal->WebView stopLoading];
    });
}

void FInoWebViewImpl_iOS::LoadHTMLString(const FString& HTML, const FString& BaseURI)
{
    check(IsInGameThread());
    if (bDestroyed) return;
    auto* Internal = static_cast<FInoWebViewImpl_iOS_Internal*>(InternalPtr);
    if (!Internal) return;

    NSString* HtmlNS = NSStringFromFString(HTML);
    // Rewrite the base URI through the vhost helper too — if the user passes
    // https://<vhost>/, it becomes inoweb://<vhost>/ so relative links inside
    // the HTML resolve through the scheme handler.
    NSString* BaseNS = BaseURI.IsEmpty() ? nil : NSStringFromFString(RewriteForVHost(BaseURI));
    dispatch_async(dispatch_get_main_queue(), ^{
        WKWebView* WebView = Internal->WebView;
        if (WebView == nil) return;
        NSURL* BaseURL = (BaseNS != nil) ? [NSURL URLWithString:BaseNS] : nil;
        [WebView loadHTMLString:HtmlNS baseURL:BaseURL];
    });
    bCachedLoading = true;
}

void FInoWebViewImpl_iOS::SetCookie(const FString& URL, const FString& Cookie)
{
    check(IsInGameThread());
    if (bDestroyed) return;

    NSString* URLString    = NSStringFromFString(URL);
    NSString* CookieString = NSStringFromFString(Cookie);
    dispatch_async(dispatch_get_main_queue(), ^{
        NSURL* URLObj = [NSURL URLWithString:URLString];
        if (URLObj == nil) return;

        // Same parsing shape as Initialize's seed-cookie loop.
        NSMutableDictionary* Props = [NSMutableDictionary dictionary];
        NSArray<NSString*>* Parts = [CookieString componentsSeparatedByString:@";"];
        BOOL bGotNameValue = NO;
        for (NSString* RawPart in Parts)
        {
            NSString* Part = [RawPart stringByTrimmingCharactersInSet:
                              [NSCharacterSet whitespaceCharacterSet]];
            if (Part.length == 0) continue;
            NSRange Eq = [Part rangeOfString:@"="];
            NSString* K = (Eq.location == NSNotFound) ? Part : [Part substringToIndex:Eq.location];
            NSString* V = (Eq.location == NSNotFound) ? @""  : [Part substringFromIndex:Eq.location + 1];
            if (!bGotNameValue)
            {
                Props[NSHTTPCookieName]   = K;
                Props[NSHTTPCookieValue]  = V;
                bGotNameValue = YES;
            }
            else
            {
                NSString* KLow = [K lowercaseString];
                if      ([KLow isEqualToString:@"path"])    Props[NSHTTPCookiePath]    = V;
                else if ([KLow isEqualToString:@"domain"])  Props[NSHTTPCookieDomain]  = V;
                else if ([KLow isEqualToString:@"expires"]) Props[NSHTTPCookieExpires] = V;
                else if ([KLow isEqualToString:@"secure"])  Props[NSHTTPCookieSecure]  = @YES;
            }
        }
        if (Props[NSHTTPCookieDomain] == nil) Props[NSHTTPCookieDomain] = URLObj.host ?: @"";
        if (Props[NSHTTPCookiePath]   == nil) Props[NSHTTPCookiePath]   = @"/";
        NSHTTPCookie* CookieObj = [NSHTTPCookie cookieWithProperties:Props];
        if (CookieObj != nil)
        {
            [WKWebsiteDataStore.defaultDataStore.httpCookieStore
                setCookie:CookieObj completionHandler:nil];
        }
    });
}

void FInoWebViewImpl_iOS::ClearAllData()
{
    check(IsInGameThread());
    if (bDestroyed) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        NSSet* AllTypes = [WKWebsiteDataStore allWebsiteDataTypes];
        [WKWebsiteDataStore.defaultDataStore
            removeDataOfTypes:AllTypes
                modifiedSince:[NSDate distantPast]
            completionHandler:^{}];
    });
}

bool FInoWebViewImpl_iOS::CapturePreview(EInoImageFormat Format, const FString& OutFilePath)
{
    check(IsInGameThread());
    if (!bReady || bDestroyed) return false;
    auto* Internal = static_cast<FInoWebViewImpl_iOS_Internal*>(InternalPtr);
    if (!Internal) return false;

    const int32 LocalId = InstanceId;
    NSString* OutPathNS = NSStringFromFString(OutFilePath);
    const bool bJpeg = (Format == EInoImageFormat::JPEG);
    const FString Path = OutFilePath;

    dispatch_async(dispatch_get_main_queue(), ^{
        WKWebView* WebView = Internal->WebView;
        if (WebView == nil)
        {
            DispatchOnGameThread(LocalId, [Path](FInoWebViewImpl_iOS* Impl)
            {
                if (Impl->OnCapturePreviewCompleteCallback)
                    Impl->OnCapturePreviewCompleteCallback(false, Path);
            });
            return;
        }

        WKSnapshotConfiguration* SnapConfig = [[WKSnapshotConfiguration alloc] init];
        [WebView takeSnapshotWithConfiguration:SnapConfig completionHandler:^(UIImage* Image, NSError* Err) {
            bool bSuccess = false;
            if (Image != nil && Err == nil)
            {
                NSData* Data = bJpeg
                    ? UIImageJPEGRepresentation(Image, 0.9)
                    : UIImagePNGRepresentation(Image);
                if (Data != nil)
                {
                    NSError* WriteErr = nil;
                    bSuccess = [Data writeToFile:OutPathNS
                                         options:NSDataWritingAtomic
                                           error:&WriteErr];
                    if (!bSuccess && WriteErr != nil)
                    {
                        UE_LOG(LogInoWebUI, Warning,
                            TEXT("CapturePreview write failed: %s"),
                            *FStringFromNSString(WriteErr.localizedDescription));
                    }
                }
            }
            DispatchOnGameThread(LocalId, [bSuccess, Path](FInoWebViewImpl_iOS* Impl)
            {
                if (Impl->OnCapturePreviewCompleteCallback)
                    Impl->OnCapturePreviewCompleteCallback(bSuccess, Path);
            });
        }];
    });
    return true;
}

void FInoWebViewImpl_iOS::LoadURLWithHeaders(const FString& URL,
                                              const TMap<FString, FString>& Headers)
{
    check(IsInGameThread());
    if (bDestroyed) return;
    auto* Internal = static_cast<FInoWebViewImpl_iOS_Internal*>(InternalPtr);
    if (!Internal) return;

    // Cross-platform UX: rewrite https://<vhost>/... → inoweb://<vhost>/...
    NSString* URLStr = NSStringFromFString(RewriteForVHost(URL));
    NSMutableDictionary* HeadersNS = [NSMutableDictionary dictionaryWithCapacity:Headers.Num()];
    for (const TPair<FString, FString>& KV : Headers)
    {
        HeadersNS[NSStringFromFString(KV.Key)] = NSStringFromFString(KV.Value);
    }

    dispatch_async(dispatch_get_main_queue(), ^{
        WKWebView* WebView = Internal->WebView;
        if (WebView == nil) return;
        NSURL* URLObj = [NSURL URLWithString:URLStr];
        if (URLObj == nil) return;
        NSMutableURLRequest* Req = [NSMutableURLRequest requestWithURL:URLObj];
        [HeadersNS enumerateKeysAndObjectsUsingBlock:^(NSString* K, NSString* V, BOOL* Stop) {
            [Req setValue:V forHTTPHeaderField:K];
        }];
        [WebView loadRequest:Req];
    });
    bCachedLoading = true;
}

#endif // PLATFORM_IOS
