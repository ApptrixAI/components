/*
 * webview_linux.c — WPE WebKit 2.50+ headless rendering into a pixel buffer.
 *
 * Architecture:
 *   • WPEDisplayHeadless backs the WebKit web view — no X11/Wayland/GBM needed.
 *   • WPE delivers each rendered frame via the render_buffer vtable slot.
 *     Buffers may be SHM or DMA-BUF depending on the system; DMA-BUF frames
 *     are imported through GBM so tiled GPU formats are handled correctly.
 *   • A Go goroutine (runtime.LockOSThread) runs the GLib main loop.
 *   • goFrameReady() (exported from Go) is called after each frame copy so the
 *     Go side can refresh the canvas.Raster widget.
 */

#include "webview_linux.h"

/* WPE Platform headless backend */
#define __WPE_HEADLESS_H_INSIDE__
#include <wpe/headless/WPEDisplayHeadless.h>
#include <wpe/headless/WPEToplevelHeadless.h>
#include <wpe/headless/WPEViewHeadless.h>
#undef __WPE_HEADLESS_H_INSIDE__

#include <wpe/wpe-platform.h>
#include <wpe/webkit.h>

#include <glib.h>
#include <gbm.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Go callback (exported from the Go package) ──────────────────────── */

extern void goFrameReady(void);

/* ── Global state ────────────────────────────────────────────────────── */

static WPEDisplay    *_display  = NULL;
static WPEToplevel   *_toplevel = NULL;
static WPEView       *_wpeView  = NULL;
static WebKitWebView *_webView  = NULL;

static int _w = 800;
static int _h = 600;

/* GBM device for importing DMA-BUF frames. */
static int                _drmFd = -1;
static struct gbm_device *_gbm   = NULL;

/* ── Frame pixel buffer ──────────────────────────────────────────────── */

static pthread_mutex_t _frameMu     = PTHREAD_MUTEX_INITIALIZER;
static uint8_t        *_frameBuf    = NULL;
static int             _frameW      = 0;
static int             _frameH      = 0;
static int             _frameStride = 0;
static int             _frameReady  = 0;

/* ── render_buffer vtable override ──────────────────────────────────── */

static gboolean our_render_buffer(WPEView            *view,
                                  WPEBuffer          *buffer,
                                  const WPERectangle *rects,
                                  guint               n_rects,
                                  GError            **error)
{
    (void)rects; (void)n_rects; (void)error;

    int           w      = wpe_buffer_get_width(buffer);
    int           h      = wpe_buffer_get_height(buffer);
    const uint8_t *pixels = NULL;
    int           stride  = 0;
    gsize         size    = 0;
    GBytes       *bytes   = NULL;   /* only set for SHM (needs unref) */

    /* GBM mapping state for DMA-BUF path. */
    struct gbm_bo *bo       = NULL;
    void          *map_data = NULL;

    if (WPE_IS_BUFFER_SHM(buffer)) {
        WPEBufferSHM *shm = WPE_BUFFER_SHM(buffer);
        bytes  = wpe_buffer_shm_get_data(shm);
        stride = (int)wpe_buffer_shm_get_stride(shm);
        pixels = g_bytes_get_data(bytes, &size);
    } else if (WPE_IS_BUFFER_DMA_BUF(buffer) && _gbm) {
        WPEBufferDMABuf *dmabuf = WPE_BUFFER_DMA_BUF(buffer);
        guint n_planes = wpe_buffer_dma_buf_get_n_planes(dmabuf);

        struct gbm_import_fd_modifier_data import = {
            .width    = (uint32_t)w,
            .height   = (uint32_t)h,
            .format   = wpe_buffer_dma_buf_get_format(dmabuf),
            .num_fds  = n_planes,
            .modifier = wpe_buffer_dma_buf_get_modifier(dmabuf),
        };
        for (guint i = 0; i < n_planes && i < 4; i++) {
            import.fds[i]     = wpe_buffer_dma_buf_get_fd(dmabuf, i);
            import.strides[i] = (int)wpe_buffer_dma_buf_get_stride(dmabuf, i);
            import.offsets[i] = (int)wpe_buffer_dma_buf_get_offset(dmabuf, i);
        }

        bo = gbm_bo_import(_gbm, GBM_BO_IMPORT_FD_MODIFIER,
                           &import, GBM_BO_USE_LINEAR);
        if (bo) {
            uint32_t map_stride = 0;
            void *mapped = gbm_bo_map(bo, 0, 0, (uint32_t)w, (uint32_t)h,
                                      GBM_BO_TRANSFER_READ,
                                      &map_stride, &map_data);
            if (mapped) {
                pixels = (const uint8_t *)mapped;
                stride = (int)map_stride;
                size   = (gsize)h * (gsize)stride;
            }
        }
    }

    if (pixels && w > 0 && h > 0 && stride > 0 &&
        size >= (gsize)h * (gsize)stride)
    {
        pthread_mutex_lock(&_frameMu);
        if (!_frameBuf || _frameW != w || _frameH != h) {
            free(_frameBuf);
            _frameBuf = malloc((gsize)h * (gsize)stride);
        }
        if (_frameBuf) {
            memcpy(_frameBuf, pixels, (gsize)h * (gsize)stride);
            _frameW      = w;
            _frameH      = h;
            _frameStride = stride;
            _frameReady  = 1;
        }
        pthread_mutex_unlock(&_frameMu);
    }

    if (bo) {
        if (map_data) gbm_bo_unmap(bo, map_data);
        gbm_bo_destroy(bo);
    }
    if (bytes) g_bytes_unref(bytes);

    wpe_view_buffer_rendered(view, buffer);
    goFrameReady();
    return TRUE;
}

/* ── g_idle_add helpers ──────────────────────────────────────────────── */

static gboolean idle_navigate(gpointer data)
{
    char *url = (char *)data;
    if (_webView)
        webkit_web_view_load_uri(_webView, url);
    free(url);
    return G_SOURCE_REMOVE;
}

static gboolean idle_go_back(gpointer data)
{
    (void)data;
    if (_webView) webkit_web_view_go_back(_webView);
    return G_SOURCE_REMOVE;
}

static gboolean idle_go_forward(gpointer data)
{
    (void)data;
    if (_webView) webkit_web_view_go_forward(_webView);
    return G_SOURCE_REMOVE;
}

static gboolean idle_reload(gpointer data)
{
    (void)data;
    if (_webView) webkit_web_view_reload(_webView);
    return G_SOURCE_REMOVE;
}

static gboolean idle_stop(gpointer data)
{
    (void)data;
    if (_webView) webkit_web_view_stop_loading(_webView);
    return G_SOURCE_REMOVE;
}

typedef struct { int w, h; } SizeArgs;

static gboolean idle_set_size(gpointer data)
{
    SizeArgs *a = (SizeArgs *)data;
    if (_wpeView)
        wpe_view_resized(_wpeView, a->w, a->h);
    if (_toplevel)
        wpe_toplevel_resize(_toplevel, a->w, a->h);
    free(a);
    return G_SOURCE_REMOVE;
}

/* ── Public C API ────────────────────────────────────────────────────── */

int WebView_Create(int width, int height)
{
    _w = width;
    _h = height;

    GError *error = NULL;

    /* Open a DRI render node for GBM so we can import DMA-BUF frames. */
    const char *render_nodes[] = {
        "/dev/dri/renderD128", "/dev/dri/renderD129", NULL
    };
    for (int i = 0; render_nodes[i] && _drmFd < 0; i++)
        _drmFd = open(render_nodes[i], O_RDWR);
    if (_drmFd >= 0)
        _gbm = gbm_create_device(_drmFd);
    if (!_gbm)
        fprintf(stderr, "webview_linux: no GBM device (DMA-BUF frames will be unavailable)\n");

    /* Create and connect the headless display. */
    _display = wpe_display_headless_new();
    if (!_display) {
        fprintf(stderr, "webview_linux: wpe_display_headless_new() failed\n");
        return 0;
    }

    if (!wpe_display_connect(_display, &error)) {
        fprintf(stderr, "webview_linux: wpe_display_connect() failed: %s\n",
                error ? error->message : "(unknown)");
        if (error) g_error_free(error);
        return 0;
    }

    /* Make this the primary display so webkit_web_view_new(NULL) picks it up. */
    wpe_display_set_primary(_display);

    /* Create the WebKitWebView using the primary (headless) display. */
    _webView = g_object_ref_sink(webkit_web_view_new(NULL));
    if (!_webView) {
        fprintf(stderr, "webview_linux: webkit_web_view_new() failed\n");
        return 0;
    }

    /* Retrieve the underlying WPEView. */
    _wpeView = webkit_web_view_get_wpe_view(_webView);
    if (!_wpeView) {
        fprintf(stderr, "webview_linux: webkit_web_view_get_wpe_view() returned NULL\n");
        return 0;
    }

    /* The WebView may have created its own display internally (different
       pointer from _display).  Always use the view's actual display for the
       toplevel so the pointer-equality assertion in wpe_view_set_toplevel
       is satisfied. */
    WPEDisplay *viewDisplay = webkit_web_view_get_display(_webView);
    if (!viewDisplay) {
        fprintf(stderr, "webview_linux: webkit_web_view_get_display() returned NULL\n");
        return 0;
    }

    _toplevel = wpe_toplevel_headless_new(WPE_DISPLAY_HEADLESS(viewDisplay));
    wpe_toplevel_resize(_toplevel, width, height);
    wpe_view_set_toplevel(_wpeView, _toplevel);
    wpe_view_resized(_wpeView, width, height);

    /* Patch the WPEViewHeadless class vtable to capture rendered frames.
       GObject class structures live on the heap (writable memory). */
    WPE_VIEW_GET_CLASS(_wpeView)->render_buffer = our_render_buffer;

    /* Map the view so WPE starts rendering. */
    wpe_view_map(_wpeView);

    return 1;
}

void WebView_IterateLoop(void)
{
    g_main_context_iteration(NULL, FALSE);
}

void WebView_SetFrame(int x, int y, int width, int height)
{
    (void)x; (void)y;   /* headless has no screen position */

    pthread_mutex_lock(&_frameMu);
    _w = width;
    _h = height;
    pthread_mutex_unlock(&_frameMu);

    SizeArgs *a = malloc(sizeof *a);
    if (a) {
        a->w = width;
        a->h = height;
        g_idle_add(idle_set_size, a);
    }
}

typedef struct { double scale; } ScaleArgs;

static gboolean idle_set_scale(gpointer data)
{
    ScaleArgs *a = (ScaleArgs *)data;
    if (_toplevel)
        wpe_toplevel_scale_changed(_toplevel, a->scale);
    free(a);
    return G_SOURCE_REMOVE;
}

void WebView_SetScale(double scale)
{
    ScaleArgs *a = malloc(sizeof *a);
    if (a) {
        a->scale = scale;
        g_idle_add(idle_set_scale, a);
    }
}

void WebView_Navigate(const char *url)
{
    if (url)
        g_idle_add(idle_navigate, strdup(url));
}

void WebView_GoBack(void)    { g_idle_add(idle_go_back,    NULL); }
void WebView_GoForward(void) { g_idle_add(idle_go_forward, NULL); }
void WebView_Reload(void)    { g_idle_add(idle_reload,     NULL); }
void WebView_Stop(void)      { g_idle_add(idle_stop,       NULL); }

/* ── Input events ────────────────────────────────────────────────────── */

typedef struct { double x, y; int button; int down; } MouseBtnArgs;
typedef struct { double x, y; }                        MouseMoveArgs;
typedef struct { double x, y, dx, dy; }                ScrollArgs;

static guint32 _ms_now(void) { return (guint32)(g_get_monotonic_time() / 1000); }

static gboolean idle_mouse_btn(gpointer data)
{
    MouseBtnArgs *a = data;
    if (_wpeView) {
        WPEEvent *ev = wpe_event_pointer_button_new(
            a->down ? WPE_EVENT_POINTER_DOWN : WPE_EVENT_POINTER_UP,
            _wpeView, WPE_INPUT_SOURCE_MOUSE, _ms_now(),
            0, a->button, a->x, a->y,
            a->down ? wpe_view_compute_press_count(_wpeView,
                          a->x, a->y, a->button, _ms_now()) : 0);
        wpe_view_event(_wpeView, ev);
        wpe_event_unref(ev);
    }
    free(a);
    return G_SOURCE_REMOVE;
}

static gboolean idle_mouse_move(gpointer data)
{
    MouseMoveArgs *a = data;
    if (_wpeView) {
        WPEEvent *ev = wpe_event_pointer_move_new(
            WPE_EVENT_POINTER_MOVE, _wpeView,
            WPE_INPUT_SOURCE_MOUSE, _ms_now(),
            0, a->x, a->y, 0, 0);
        wpe_view_event(_wpeView, ev);
        wpe_event_unref(ev);
    }
    free(a);
    return G_SOURCE_REMOVE;
}

static gboolean idle_scroll(gpointer data)
{
    ScrollArgs *a = data;
    if (_wpeView) {
        WPEEvent *ev = wpe_event_scroll_new(
            _wpeView, WPE_INPUT_SOURCE_MOUSE, _ms_now(),
            0, a->dx, a->dy, FALSE, FALSE, a->x, a->y);
        wpe_view_event(_wpeView, ev);
        wpe_event_unref(ev);
    }
    free(a);
    return G_SOURCE_REMOVE;
}

void WebView_MouseDown(double x, double y, int button)
{
    MouseBtnArgs *a = malloc(sizeof *a);
    if (a) { *a = (MouseBtnArgs){x, y, button, 1}; g_idle_add(idle_mouse_btn, a); }
}

void WebView_MouseUp(double x, double y, int button)
{
    MouseBtnArgs *a = malloc(sizeof *a);
    if (a) { *a = (MouseBtnArgs){x, y, button, 0}; g_idle_add(idle_mouse_btn, a); }
}

void WebView_MouseMove(double x, double y)
{
    MouseMoveArgs *a = malloc(sizeof *a);
    if (a) { *a = (MouseMoveArgs){x, y}; g_idle_add(idle_mouse_move, a); }
}

void WebView_Scroll(double x, double y, double dx, double dy)
{
    ScrollArgs *a = malloc(sizeof *a);
    if (a) { *a = (ScrollArgs){x, y, dx, dy}; g_idle_add(idle_scroll, a); }
}

typedef struct { guint keyval; int down; } KeyArgs;

static gboolean idle_key(gpointer data)
{
    KeyArgs *a = data;
    if (_wpeView) {
        /* Look up the hardware keycode via the display keymap. */
        guint keycode = 0;
        WPEKeymap *keymap = wpe_display_get_keymap(_display);
        if (keymap) {
            WPEKeymapEntry *entries = NULL;
            guint n = 0;
            if (wpe_keymap_get_entries_for_keyval(keymap, a->keyval, &entries, &n) && n > 0)
                keycode = entries[0].keycode;
            g_free(entries);
        }
        WPEEvent *ev = wpe_event_keyboard_new(
            a->down ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP,
            _wpeView, WPE_INPUT_SOURCE_KEYBOARD, _ms_now(),
            0, keycode, a->keyval);
        wpe_view_event(_wpeView, ev);
        wpe_event_unref(ev);
    }
    free(a);
    return G_SOURCE_REMOVE;
}

void WebView_KeyDown(unsigned int keyval)
{
    KeyArgs *a = malloc(sizeof *a);
    if (a) { *a = (KeyArgs){keyval, 1}; g_idle_add(idle_key, a); }
}

void WebView_KeyUp(unsigned int keyval)
{
    KeyArgs *a = malloc(sizeof *a);
    if (a) { *a = (KeyArgs){keyval, 0}; g_idle_add(idle_key, a); }
}

/* ── State queries ───────────────────────────────────────────────────── */

int WebView_IsLoading(void)
{
    return (_webView && webkit_web_view_is_loading(_webView)) ? 1 : 0;
}

const char *WebView_GetURL(void)
{
    if (!_webView)
        return "";
    const char *uri = webkit_web_view_get_uri(_webView);
    return uri ? uri : "";
}

const uint8_t *WebView_LockFrame(int *out_w, int *out_h, int *out_stride)
{
    pthread_mutex_lock(&_frameMu);
    if (!_frameReady || !_frameBuf) {
        pthread_mutex_unlock(&_frameMu);
        return NULL;
    }
    *out_w      = _frameW;
    *out_h      = _frameH;
    *out_stride = _frameStride;
    return _frameBuf;   /* caller holds _frameMu until WebView_UnlockFrame */
}

void WebView_UnlockFrame(void)
{
    pthread_mutex_unlock(&_frameMu);
}
