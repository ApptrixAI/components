#include "webview_ios.h"
#include "webview_apple_meta.h"

#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#import <dlfcn.h>

#include <stdlib.h>

static void ensureWebKitLoaded(void) {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        dlopen("/System/Library/Frameworks/WebKit.framework/WebKit", RTLD_LAZY);
    });
}

/* Each instance owns its WKWebView, added as a subview of the key window. */
struct WebViewInstance {
    WKWebView *webView;
    AppleMeta  meta;
};

WebViewInstance *WebView_Create(uintptr_t goHandle) {
    __block WebViewInstance *inst = NULL;

    void (^block)(void) = ^{
        @autoreleasepool {
            ensureWebKitLoaded();

            id<UIApplicationDelegate> delegate = [UIApplication sharedApplication].delegate;
            if (delegate == nil || ![delegate respondsToSelector:@selector(window)]) return;
            UIWindow *window = [delegate window];
            if (window == nil) return;

            // Resolve WebKit classes at runtime — cgo doesn't link
            // WebKit.framework for device builds. The (void) class
            // references prevent the compiler from stripping the
            // ObjC class metadata that NSClassFromString needs.
            (void)[WKWebViewConfiguration class];
            (void)[WKWebView class];
            Class configClass = NSClassFromString(@"WKWebViewConfiguration");
            if (configClass == nil) {
                [[NSBundle bundleWithPath:@"/System/Library/Frameworks/WebKit.framework"] load];
                configClass = NSClassFromString(@"WKWebViewConfiguration");
            }

            WKWebViewConfiguration *config = [[configClass alloc] init];
            if (config == nil) return;

            inst = calloc(1, sizeof *inst);
            AppleMeta_Init(&inst->meta, goHandle);
            /* Must be wired into the config before the view is created from it. */
            FyneMeta_Install(config, &inst->meta);

            Class wkClass = NSClassFromString(@"WKWebView");
            WKWebView *webView = (WKWebView *)[[wkClass alloc] initWithFrame:window.bounds configuration:config];
            [config release];
            if (webView == nil) {
                AppleMeta_Destroy(&inst->meta);
                free(inst);
                inst = NULL;
                return;
            }

            webView.autoresizingMask = UIViewAutoresizingNone;
            webView.allowsBackForwardNavigationGestures = YES;

            [window addSubview:webView];

            inst->webView = webView;             /* keep the +1 from alloc */
        }
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), block);
    }

    return inst;
}

void WebView_Destroy(WebViewInstance *inst) {
    if (inst == NULL) return;

    void (^block)(void) = ^{
        @autoreleasepool {
            /* Detach the message handler first so no callback can fire into
               the meta we are about to free. */
            FyneMeta_Uninstall(inst->webView);
            [inst->webView removeFromSuperview];
            [inst->webView release];
        }
    };
    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), block);
    }

    AppleMeta_Destroy(&inst->meta);
    free(inst);
}

void WebView_SetFrame(WebViewInstance *inst, double x, double y, double width, double height) {
    if (inst == NULL || inst->webView == nil) return;

    void (^block)(void) = ^{
        UIEdgeInsets insets = inst->webView.superview.safeAreaInsets;
        inst->webView.frame = CGRectMake(x + insets.left, y + insets.top, width, height);
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

void WebView_SetVisible(WebViewInstance *inst, int visible) {
    if (inst == NULL || inst->webView == nil) return;

    void (^block)(void) = ^{
        if (inst->webView == nil) return;
        inst->webView.hidden = visible ? NO : YES;
        if (visible) {
            // Raise above other web views (e.g. background tabs) sharing the window.
            [inst->webView.superview bringSubviewToFront:inst->webView];
        }
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

void WebView_SetDarkMode(WebViewInstance *inst, int dark) {
    if (inst == NULL || inst->webView == nil) return;

    if (dark) {
        inst->webView.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    } else {
        inst->webView.overrideUserInterfaceStyle = UIUserInterfaceStyleLight;
    }
}

void WebView_Navigate(WebViewInstance *inst, const char *url) {
    if (inst == NULL || url == NULL) return;
    NSString *urlString = [NSString stringWithUTF8String:url];
    if (![urlString hasPrefix:@"http://"] && ![urlString hasPrefix:@"https://"]) {
        urlString = [@"https://" stringByAppendingString:urlString];
    }

    void (^block)(void) = ^{
        if (inst->webView == nil) return;

        NSURL *nsurl = [NSURL URLWithString:urlString];
        if (nsurl == nil) return;

        NSURLRequest *request = [NSURLRequest requestWithURL:nsurl];
        [inst->webView loadRequest:request];
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

void WebView_GoBack(WebViewInstance *inst) {
    if (inst == NULL) return;
    void (^block)(void) = ^{
        if (inst->webView != nil) [inst->webView goBack];
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

void WebView_GoForward(WebViewInstance *inst) {
    if (inst == NULL) return;
    void (^block)(void) = ^{
        if (inst->webView != nil) [inst->webView goForward];
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

void WebView_Reload(WebViewInstance *inst) {
    if (inst == NULL) return;
    void (^block)(void) = ^{
        if (inst->webView != nil) [inst->webView reload];
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

void WebView_Stop(WebViewInstance *inst) {
    if (inst == NULL) return;
    void (^block)(void) = ^{
        if (inst->webView != nil) [inst->webView stopLoading];
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

int WebView_IsLoading(WebViewInstance *inst) {
    if (inst == NULL || inst->webView == nil) return 0;
    return inst->webView.isLoading ? 1 : 0;
}

const char* WebView_GetURL(WebViewInstance *inst) {
    if (inst == NULL || inst->webView == nil) return "";
    NSString *url = inst->webView.URL.absoluteString;
    if (url == nil) return "";
    return [url UTF8String];
}

char *WebView_CopyTitle(WebViewInstance *inst) {
    if (inst == NULL) return NULL;
    return AppleMeta_CopyTitle(&inst->meta);
}

const uint8_t *WebView_LockFavicon(WebViewInstance *inst, int *out_w, int *out_h) {
    if (inst == NULL) return NULL;
    return AppleMeta_LockFavicon(&inst->meta, out_w, out_h);
}

void WebView_UnlockFavicon(WebViewInstance *inst) {
    if (inst == NULL) return;
    AppleMeta_UnlockFavicon(&inst->meta);
}
