#ifndef WEBVIEW_WINDOWS_H
#define WEBVIEW_WINDOWS_H

#include <stddef.h>
#include <stdint.h>

/* An opaque per-widget web view.  Several instances may coexist in one
   process — each owns its own WebView2 environment, controller and core
   web view, hosted in the same parent HWND. */
typedef struct WebViewInstance WebViewInstance;

/* Create a new web view instance hosted in the given parent HWND (passed as a
   uintptr).  goHandle is an opaque token passed to the goTitleChanged/
   goFaviconChanged callbacks.  Returns NULL on failure. */
WebViewInstance *WebView_Create(uintptr_t hwnd, uintptr_t goHandle);

/* Tear down an instance: close its controller and release its resources. */
void WebView_Destroy(WebViewInstance *inst);

void WebView_SetFrame(WebViewInstance *inst, double x, double y, double width, double height);
/* Show or hide the native view. */
void WebView_SetVisible(WebViewInstance *inst, int visible);
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

/* Current page title, newly-allocated (NULL if none); the caller frees it.
   Updated whenever the goTitleChanged callback fires. */
char *WebView_CopyTitle(WebViewInstance *inst);

/* Favicon pixels.  WebView_LockFavicon() returns RGBA8888 data and fills out
   its width/height, or NULL if none; the caller must call WebView_UnlockFavicon()
   when done.  Updated whenever goFaviconChanged fires. */
const uint8_t *WebView_LockFavicon(WebViewInstance *inst, int *out_w, int *out_h);
void           WebView_UnlockFavicon(WebViewInstance *inst);

#endif
