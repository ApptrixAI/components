#include <TargetConditionals.h>
#if TARGET_OS_OSX

#include "webview_darwin.h"

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

static WKWebView *_webView = nil;
static NSView *_contentView = nil;

void WebView_Create(void *nsWindow) {
    if (nsWindow == NULL) return;

    @autoreleasepool {
        NSWindow *window = (__bridge NSWindow *)(nsWindow);
        _contentView = [window contentView];
        if (_contentView == nil) return;

        WKWebViewConfiguration *config = [[WKWebViewConfiguration alloc] init];

        _webView = [[WKWebView alloc] initWithFrame:NSZeroRect configuration:config];
        _webView.autoresizingMask = 0;
        _webView.allowsBackForwardNavigationGestures = YES;

        [_contentView addSubview:_webView];
    }
}

void WebView_SetFrame(double x, double y, double width, double height) {
    if (_webView == nil || _contentView == nil) return;

    // Convert from top-left origin (Fyne) to bottom-left origin (macOS)
    CGFloat contentHeight = _contentView.bounds.size.height;
    NSRect frame = NSMakeRect(x, contentHeight - y - height, width, height);
    [_webView setFrame:frame];
}

void WebView_SetDarkMode(int dark) {
    if (_webView == nil) return;

    if (dark) {
        _webView.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    } else {
        _webView.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    }
}

void WebView_Navigate(const char *url) {
    if (url == NULL) return;
    NSString *urlString = [NSString stringWithUTF8String:url];
    if (![urlString hasPrefix:@"http://"] && ![urlString hasPrefix:@"https://"]) {
        urlString = [@"https://" stringByAppendingString:urlString];
    }

    if (_webView == nil) {
        NSLog(@"WebView_Navigate: webView is nil");
        return;
    }

    NSURL *nsurl = [NSURL URLWithString:urlString];
    if (nsurl == nil) {
        NSLog(@"WebView_Navigate: invalid URL: %@", urlString);
        return;
    }

    NSURLRequest *request = [NSURLRequest requestWithURL:nsurl];
    [_webView loadRequest:request];
}

void WebView_GoBack(void) {
    if (_webView != nil) [_webView goBack];
}

void WebView_GoForward(void) {
    if (_webView != nil) [_webView goForward];
}

void WebView_Reload(void) {
    if (_webView != nil) [_webView reload];
}

void WebView_Stop(void) {
    if (_webView != nil) [_webView stopLoading];
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

#endif // TARGET_OS_OSX
