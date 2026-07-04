#include <TargetConditionals.h>
#if TARGET_OS_OSX

#include "webview_darwin.h"
#include "webview_apple_meta.h"

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <stdlib.h>

/* Each instance owns its WKWebView and a retained reference to the content
   view it was added to, so it can detach itself on teardown. */
struct WebViewInstance {
    WKWebView *webView;
    NSView    *contentView;
    AppleMeta  meta;
};

WebViewInstance *WebView_Create(uintptr_t nsWindow, uintptr_t goHandle) {
    if (nsWindow == 0) return NULL;

    WebViewInstance *inst = NULL;
    @autoreleasepool {
        NSWindow *window = (__bridge NSWindow *)(void *)nsWindow;
        NSView *contentView = [window contentView];
        if (contentView == nil) return NULL;

        inst = calloc(1, sizeof *inst);
        AppleMeta_Init(&inst->meta, goHandle);

        WKWebViewConfiguration *config = [[WKWebViewConfiguration alloc] init];
        /* Must be wired into the config before the view is created from it. */
        FyneMeta_Install(config, &inst->meta);

        WKWebView *webView = [[WKWebView alloc] initWithFrame:NSZeroRect configuration:config];
        [config release];
        webView.autoresizingMask = 0;
        webView.allowsBackForwardNavigationGestures = YES;

        [contentView addSubview:webView];

        inst->webView     = webView;             /* keep the +1 from alloc */
        inst->contentView = [contentView retain];
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
            [inst->contentView release];
        }
    };
    if ([NSThread isMainThread]) block();
    else dispatch_sync(dispatch_get_main_queue(), block);

    AppleMeta_Destroy(&inst->meta);
    free(inst);
}

void WebView_SetFrame(WebViewInstance *inst, double x, double y, double width, double height) {
    if (inst == NULL || inst->webView == nil || inst->contentView == nil) return;

    // Convert from top-left origin (Fyne) to bottom-left origin (macOS)
    CGFloat contentHeight = inst->contentView.bounds.size.height;
    NSRect frame = NSMakeRect(x, contentHeight - y - height, width, height);
    [inst->webView setFrame:frame];
}

void WebView_SetVisible(WebViewInstance *inst, int visible) {
    if (inst == NULL || inst->webView == nil) return;

    inst->webView.hidden = visible ? NO : YES;
    if (visible && inst->contentView != nil) {
        // Re-order (without detaching) so the visible view sits above others.
        [inst->contentView addSubview:inst->webView
                           positioned:NSWindowAbove
                           relativeTo:nil];
    }
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

#endif // TARGET_OS_OSX
