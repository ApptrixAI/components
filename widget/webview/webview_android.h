#ifndef WEBVIEW_ANDROID_H
#define WEBVIEW_ANDROID_H

#include <jni.h>
#include <stddef.h>

/* An opaque per-widget web view.  Several instances may coexist in one
   process — each owns its own android.webkit.WebView added to the window
   manager.  They share a single JNI environment and UI-thread command
   queue. */
typedef struct WebViewInstance WebViewInstance;

/* One-time process-wide JNI initialisation. */
void WebView_Init(JavaVM *vm, JNIEnv *env, jobject ctx);

/* Allocate a new (not-yet-created) instance and register it. */
WebViewInstance *WebView_New(void);

/* Request creation of the native WebView for inst (idempotent).  Returns 1
   once the WebView exists, 0 while creation is still pending. */
int WebView_TryCreate(WebViewInstance *inst);

/* Tear down inst: remove its WebView from the window manager, release its
   JNI references, unregister and free it. */
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
