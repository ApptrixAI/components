#ifndef WEBVIEW_IOS_H
#define WEBVIEW_IOS_H

#include <stddef.h>

/* An opaque per-widget web view.  Several instances may coexist in one
   process — each owns its own WKWebView added as a subview of the key
   window. */
typedef struct WebViewInstance WebViewInstance;

/* Create a new web view instance.  Returns NULL on failure. */
WebViewInstance *WebView_Create(void);

/* Tear down an instance: remove it from its superview and release it. */
void WebView_Destroy(WebViewInstance *inst);

void WebView_SetFrame(WebViewInstance *inst, double x, double y, double width, double height);
void WebView_SetDarkMode(WebViewInstance *inst, int dark);
void WebView_Navigate(WebViewInstance *inst, const char *url);
void WebView_GoBack(WebViewInstance *inst);
void WebView_GoForward(WebViewInstance *inst);
void WebView_Reload(WebViewInstance *inst);
void WebView_Stop(WebViewInstance *inst);
int WebView_IsLoading(WebViewInstance *inst);
const char* WebView_GetURL(WebViewInstance *inst);

#endif
