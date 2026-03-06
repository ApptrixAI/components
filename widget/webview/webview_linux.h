#ifndef WEBVIEW_LINUX_H
#define WEBVIEW_LINUX_H

#include <stdint.h>

/* Create the WebKit web view backed by a WPEDisplayHeadless.
   width/height are the initial render dimensions.
   Returns 1 on success, 0 on failure. */
int         WebView_Create   (int width, int height);

/* Pump the GLib main context once (non-blocking).  Call this every
   frame from the Fyne run loop via fyne.Do(). */
void        WebView_IterateLoop(void);

/* Resize the headless view so WPE renders at the new dimensions.
   x and y are ignored (headless has no screen position). */
void        WebView_SetFrame (int x, int y, int width, int height);
void        WebView_SetScale (double scale);

/* Navigation */
void        WebView_Navigate (const char *url);
void        WebView_GoBack   (void);
void        WebView_GoForward(void);
void        WebView_Reload   (void);
void        WebView_Stop     (void);

/* State queries (thread-safe reads of GObject properties) */
int         WebView_IsLoading(void);
const char *WebView_GetURL   (void);

/* Input events — dispatched to WPE via g_idle_add.
   button: 1=left, 2=middle, 3=right.
   keyval: XKB/GDK keysym. */
void WebView_MouseDown(double x, double y, int button);
void WebView_MouseUp  (double x, double y, int button);
void WebView_MouseMove(double x, double y);
void WebView_Scroll   (double x, double y, double dx, double dy);
void WebView_KeyDown  (unsigned int keyval);
void WebView_KeyUp    (unsigned int keyval);

/* Frame access from Go.
   WebView_LockFrame() returns a pointer to ARGB8888 pixel data and fills
   out the dimensions.  Returns NULL when no frame is available.
   The caller MUST call WebView_UnlockFrame() when done. */
const uint8_t *WebView_LockFrame  (int *out_w, int *out_h, int *out_stride);
void           WebView_UnlockFrame(void);

#endif /* WEBVIEW_LINUX_H */
