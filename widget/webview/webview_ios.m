#include "webview_ios.h"

#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#import <dlfcn.h>

static void ensureWebKitLoaded(void) {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        dlopen("/System/Library/Frameworks/WebKit.framework/WebKit", RTLD_LAZY);
    });
}

static WKWebView *_webView = nil;

int WebView_Create(void) {
    __block int result = 0;

    void (^block)(void) = ^{
        @autoreleasepool {
            if (_webView != nil) {
                result = 1;
                return;
            }

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

            Class wkClass = NSClassFromString(@"WKWebView");
            _webView = (WKWebView *)[[wkClass alloc] initWithFrame:window.bounds configuration:config];
            if (_webView == nil) return;

            _webView.autoresizingMask = UIViewAutoresizingNone;
            _webView.allowsBackForwardNavigationGestures = YES;

            [window addSubview:_webView];
            result = 1;
        }
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), block);
    }

    return result;
}

void WebView_SetFrame(double x, double y, double width, double height) {
    if (_webView == nil) return;

    void (^block)(void) = ^{
        UIEdgeInsets insets = _webView.superview.safeAreaInsets;
        _webView.frame = CGRectMake(x + insets.left, y + insets.top, width, height);
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

void WebView_SetDarkMode(int dark) {
    if (_webView == nil) return;

    if (dark) {
        _webView.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    } else {
        _webView.overrideUserInterfaceStyle = UIUserInterfaceStyleLight;
    }
}

void WebView_Navigate(const char *url) {
    if (url == NULL) return;
    NSString *urlString = [NSString stringWithUTF8String:url];
    if (![urlString hasPrefix:@"http://"] && ![urlString hasPrefix:@"https://"]) {
        urlString = [@"https://" stringByAppendingString:urlString];
    }

    void (^block)(void) = ^{
        if (_webView == nil) return;

        NSURL *nsurl = [NSURL URLWithString:urlString];
        if (nsurl == nil) return;

        NSURLRequest *request = [NSURLRequest requestWithURL:nsurl];
        [_webView loadRequest:request];
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

void WebView_GoBack(void) {
    void (^block)(void) = ^{
        if (_webView == nil) return;

        [_webView goBack];
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

void WebView_GoForward(void) {
    void (^block)(void) = ^{
        if (_webView == nil) return;

        [_webView goForward];
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

void WebView_Reload(void) {
    void (^block)(void) = ^{
        if (_webView == nil) return;

        [_webView reload];
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

void WebView_Stop(void) {
    void (^block)(void) = ^{
        if (_webView == nil) return;

        [_webView stopLoading];
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

int WebView_IsLoading(void) {
    if (_webView == nil) return 0;
    return _webView.isLoading ? 1 : 0;
}

const char* WebView_GetURL(void) {
    if (_webView == nil) return "";
    NSString *url = _webView.URL.absoluteString;
    if (url == nil) return "";
    return [url UTF8String];
}
