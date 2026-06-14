#include <TargetConditionals.h>
#if TARGET_OS_OSX

#include "webview_darwin.h"

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <stdlib.h>

/* Each instance owns its WKWebView and a retained reference to the content
   view it was added to, so it can detach itself on teardown. */
struct WebViewInstance {
    WKWebView *webView;
    NSView    *contentView;
};

WebViewInstance *WebView_Create(void *nsWindow) {
    if (nsWindow == NULL) return NULL;

    WebViewInstance *inst = NULL;
    @autoreleasepool {
        NSWindow *window = (__bridge NSWindow *)(nsWindow);
        NSView *contentView = [window contentView];
        if (contentView == nil) return NULL;

        WKWebViewConfiguration *config = [[WKWebViewConfiguration alloc] init];

        WKWebView *webView = [[WKWebView alloc] initWithFrame:NSZeroRect configuration:config];
        [config release];
        webView.autoresizingMask = 0;
        webView.allowsBackForwardNavigationGestures = YES;

        [contentView addSubview:webView];

        inst = calloc(1, sizeof *inst);
        inst->webView     = webView;             /* keep the +1 from alloc */
        inst->contentView = [contentView retain];
    }
    return inst;
}

void WebView_Destroy(WebViewInstance *inst) {
    if (inst == NULL) return;

    void (^block)(void) = ^{
        @autoreleasepool {
            [inst->webView removeFromSuperview];
            [inst->webView release];
            [inst->contentView release];
        }
    };
    if ([NSThread isMainThread]) block();
    else dispatch_sync(dispatch_get_main_queue(), block);

    free(inst);
}

void WebView_SetFrame(WebViewInstance *inst, double x, double y, double width, double height) {
    if (inst == NULL || inst->webView == nil || inst->contentView == nil) return;

    // Convert from top-left origin (Fyne) to bottom-left origin (macOS)
    CGFloat contentHeight = inst->contentView.bounds.size.height;
    NSRect frame = NSMakeRect(x, contentHeight - y - height, width, height);
    [inst->webView setFrame:frame];
}

void WebView_SetDarkMode(WebViewInstance *inst, int dark) {
    if (inst == NULL || inst->webView == nil) return;

    if (dark) {
        inst->webView.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    } else {
        inst->webView.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    }
}

void WebView_Navigate(WebViewInstance *inst, const char *url) {
    if (inst == NULL || inst->webView == nil || url == NULL) return;
    NSString *urlString = [NSString stringWithUTF8String:url];
    if (![urlString hasPrefix:@"http://"] && ![urlString hasPrefix:@"https://"]) {
        urlString = [@"https://" stringByAppendingString:urlString];
    }

    NSURL *nsurl = [NSURL URLWithString:urlString];
    if (nsurl == nil) {
        NSLog(@"WebView_Navigate: invalid URL: %@", urlString);
        return;
    }

    NSURLRequest *request = [NSURLRequest requestWithURL:nsurl];
    [inst->webView loadRequest:request];
}

void WebView_GoBack(WebViewInstance *inst) {
    if (inst != NULL && inst->webView != nil) [inst->webView goBack];
}

void WebView_GoForward(WebViewInstance *inst) {
    if (inst != NULL && inst->webView != nil) [inst->webView goForward];
}

void WebView_Reload(WebViewInstance *inst) {
    if (inst != NULL && inst->webView != nil) [inst->webView reload];
}

void WebView_Stop(WebViewInstance *inst) {
    if (inst != NULL && inst->webView != nil) [inst->webView stopLoading];
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

#endif // TARGET_OS_OSX
