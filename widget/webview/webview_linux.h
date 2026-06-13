//go:build linux && !android

#ifndef WEBVIEW_LINUX_H
#define WEBVIEW_LINUX_H

#include <stdint.h>

/* An opaque per-widget web view.  Several instances may coexist in one
   process — they share a single GLib main loop and headless display but
   each owns its own WebKitWebView, WPEView, toplevel and frame buffer. */
typedef struct WebViewInstance WebViewInstance;

/* Run the shared GLib main loop.  Performs one-time global initialisation
   (headless display, GBM device, memory-pressure settings) and then blocks
   forever iterating the loop.  Call exactly once from a dedicated goroutine
   with runtime.LockOSThread(). */
void WebView_RunLoop(void);

/* Block until WebView_RunLoop has finished global initialisation.
   Returns 1 if it succeeded, 0 on failure.  Must be called from a thread
   other than the one running WebView_RunLoop. */
int WebView_WaitReady(void);

/* Create a new web view instance on the loop thread.
   width/height are the initial render dimensions.  goHandle is an opaque
   token (a runtime/cgo.Handle) echoed back to goFrameReady so the Go side
   can route frames to the right widget.  Returns NULL on failure. */
WebViewInstance *WebView_Create(int width, int height, uintptr_t goHandle);

/* Tear down an instance and free its resources.  Runs on the loop thread
   and returns once teardown is complete, so no further goFrameReady will
   fire for this instance afterwards. */
void WebView_Destroy(WebViewInstance *inst);

/* Resize the headless view so WPE renders at the new dimensions.
   x and y are ignored (headless has no screen position). */
void WebView_SetFrame(WebViewInstance *inst, int x, int y, int width, int height);
void WebView_SetScale(WebViewInstance *inst, double scale);

/* Navigation */
void WebView_Navigate (WebViewInstance *inst, const char *url);
void WebView_GoBack   (WebViewInstance *inst);
void WebView_GoForward(WebViewInstance *inst);
void WebView_Reload   (WebViewInstance *inst);
void WebView_Stop     (WebViewInstance *inst);

/* Appearance.  Dark mode is a property of the shared headless display, so it
   affects every instance; the parameter is kept for API symmetry. */
void WebView_SetDarkMode(WebViewInstance *inst, int dark);

/* State queries (thread-safe reads of GObject properties) */
int         WebView_IsLoading(WebViewInstance *inst);
const char *WebView_GetURL   (WebViewInstance *inst);

/* Input events — dispatched to WPE via g_idle_add.
   button: 1=left, 2=middle, 3=right.
   keyval: XKB/GDK keysym. */
void WebView_MouseDown(WebViewInstance *inst, double x, double y, int button);
void WebView_MouseUp  (WebViewInstance *inst, double x, double y, int button);
void WebView_MouseMove(WebViewInstance *inst, double x, double y);
void WebView_Scroll   (WebViewInstance *inst, double x, double y, double dx, double dy);
void WebView_KeyDown  (WebViewInstance *inst, unsigned int keyval);
void WebView_KeyUp    (WebViewInstance *inst, unsigned int keyval);

/* Frame access from Go.
   WebView_LockFrame() returns a pointer to RGBA8888 pixel data and fills
   out the dimensions.  Returns NULL when no frame is available.
   The caller MUST call WebView_UnlockFrame() when done. */
const uint8_t *WebView_LockFrame  (WebViewInstance *inst, int *out_w, int *out_h, int *out_stride);
void           WebView_UnlockFrame(WebViewInstance *inst);

#endif /* WEBVIEW_LINUX_H */
