#ifndef WEBVIEW_WINDOWS_H
#define WEBVIEW_WINDOWS_H

#include <stddef.h>

/* An opaque per-widget web view.  Several instances may coexist in one
   process — each owns its own WebView2 environment, controller and core
   web view, hosted in the same parent HWND. */
typedef struct WebViewInstance WebViewInstance;

/* Create a new web view instance hosted in the given parent HWND.
   Returns NULL on failure. */
WebViewInstance *WebView_Create(void *hwnd);

/* Tear down an instance: close its controller and release its resources. */
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
void WebView_Focus(WebViewInstance *inst);
void WebView_Unfocus(WebViewInstance *inst);

#endif
