//go:build linux && !android

/*
 * webview_linux.c — WPE WebKit 2.50+ headless rendering into a pixel buffer.
 *
 * Architecture:
 *   • A single shared GLib main loop (WebView_RunLoop) runs on a dedicated
 *     Go goroutine and performs one-time global init: a WPEDisplayHeadless,
 *     a GBM device for de-tiling DMA-BUF frames, and the process-wide
 *     memory-pressure / cache settings.
 *   • Each widget is a WebViewInstance: its own WebKitWebView, WPEView,
 *     toplevel and frame buffer.  Many instances coexist in one process,
 *     all driven by the shared loop and display.
 *   • WPE delivers each rendered frame via the render_buffer vtable slot.
 *     The slot lives on the (shared) WPEViewHeadless class, so it is patched
 *     once; the handler recovers the owning instance from the WPEView's
 *     object data.  SHM buffers are read directly; DMA-BUF buffers are
 *     de-tiled via GBM with a workaround for Mesa's gbm_bo_map staging-fd leak.
 *   • goFrameReady(goHandle) (exported from Go) is called after each frame
 *     copy so the Go side can refresh the right canvas.Image widget.
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

extern void goFrameReady(uintptr_t goHandle);

/* ── Shared, process-wide state ────────────────────────────────────────── */

static WPEDisplay        *_display = NULL;
static int                _drmFd   = -1;
static struct gbm_device *_gbm     = NULL;

/* Original render_buffer vtable method — we chain to it after reading
   pixels so that WPE's internal buffer lifecycle (recycling, fd
   management) works correctly. */
static gboolean (*_orig_render_buffer)(WPEView *, WPEBuffer *,
                                       const WPERectangle *, guint,
                                       GError **) = NULL;

/* Key under which each WPEView stores a back-pointer to its instance. */
#define WV_INSTANCE_KEY "wv_instance"

/* Minimum interval between frame copies (microseconds).
   Frames arriving faster than this are returned to WPE immediately
   without the expensive pixel copy. */
#define FRAME_MIN_INTERVAL_US (1000000 / 60)   /* ~60 fps */

/* ── Per-instance state ──────────────────────────────────────────────── */

struct WebViewInstance {
    WebKitWebView *webView;
    WPEView       *wpeView;
    WPEToplevel   *toplevel;

    uintptr_t      goHandle;   /* opaque token for goFrameReady */

    int            w;
    int            h;

    /* Frame pixel buffer (RGBA8888). */
    pthread_mutex_t frameMu;
    uint8_t        *frameBuf;
    int             frameW;
    int             frameH;
    int             frameStride;
    int             frameReady;
    int64_t         lastFrameTimeUs;

    /* Coalesced mouse-move: only the latest position is dispatched. */
    pthread_mutex_t moveMu;
    double          moveX;
    double          moveY;
    int             movePending;

    /* Coalesced scroll: accumulate deltas, dispatch once per idle. */
    pthread_mutex_t scrollMu;
    double          scrollX;
    double          scrollY;
    double          scrollDX;
    double          scrollDY;
    int             scrollPending;
};

/* ── render_buffer vtable override ──────────────────────────────────────
   Shared across all WPEViewHeadless instances; the owning WebViewInstance
   is recovered from the view's object data. */

static gboolean our_render_buffer(WPEView            *view,
                                  WPEBuffer          *buffer,
                                  const WPERectangle *rects,
                                  guint               n_rects,
                                  GError            **error)
{
    (void)rects; (void)n_rects; (void)error;

    WebViewInstance *inst =
        (WebViewInstance *)g_object_get_data(G_OBJECT(view), WV_INSTANCE_KEY);
    if (!inst) {
        /* No owner (being destroyed) — just recycle the buffer. */
        if (_orig_render_buffer)
            return _orig_render_buffer(view, buffer, rects, n_rects, error);
        wpe_view_buffer_rendered(view, buffer);
        return TRUE;
    }

    /* Rate-limit: skip the expensive pixel copy if we produced a frame
       recently.  Always release the buffer immediately so WPE can
       reuse it — holding buffers forces the web process to allocate
       new ones, causing unbounded memory growth. */
    int64_t now = g_get_monotonic_time();
    if (now - inst->lastFrameTimeUs < FRAME_MIN_INTERVAL_US) {
        /* Skip pixel copy but still chain to original for buffer recycling. */
        if (_orig_render_buffer)
            return _orig_render_buffer(view, buffer, rects, n_rects, error);
        wpe_view_buffer_rendered(view, buffer);
        return TRUE;
    }

    int           w      = wpe_buffer_get_width(buffer);
    int           h      = wpe_buffer_get_height(buffer);
    const uint8_t *pixels = NULL;
    int           stride  = 0;
    gsize         size    = 0;

    /* Per-frame GBM map state (unmap after copy). */
    struct gbm_bo *bo       = NULL;
    void          *map_data = NULL;

    if (WPE_IS_BUFFER_SHM(buffer)) {
        WPEBufferSHM *shm = WPE_BUFFER_SHM(buffer);
        GBytes *bytes  = wpe_buffer_shm_get_data(shm);
        stride = (int)wpe_buffer_shm_get_stride(shm);
        pixels = g_bytes_get_data(bytes, &size);
    } else if (WPE_IS_BUFFER_DMA_BUF(buffer) && _gbm) {
        WPEBufferDMABuf *dmabuf = WPE_BUFFER_DMA_BUF(buffer);
        guint n_planes = wpe_buffer_dma_buf_get_n_planes(dmabuf);

        struct gbm_import_fd_modifier_data imp = {
            .width    = (uint32_t)w,
            .height   = (uint32_t)h,
            .format   = wpe_buffer_dma_buf_get_format(dmabuf),
            .num_fds  = n_planes,
            .modifier = wpe_buffer_dma_buf_get_modifier(dmabuf),
        };
        for (guint i = 0; i < n_planes && i < 4; i++) {
            imp.fds[i]     = wpe_buffer_dma_buf_get_fd(dmabuf, i);
            imp.strides[i] = (int)wpe_buffer_dma_buf_get_stride(dmabuf, i);
            imp.offsets[i] = (int)wpe_buffer_dma_buf_get_offset(dmabuf, i);
        }

        bo = gbm_bo_import(_gbm, GBM_BO_IMPORT_FD_MODIFIER,
                           &imp, GBM_BO_USE_LINEAR);
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
        int dst_stride = w * 4;
        pthread_mutex_lock(&inst->frameMu);
        if (!inst->frameBuf || inst->frameW != w || inst->frameH != h) {
            free(inst->frameBuf);
            inst->frameBuf = malloc((gsize)h * (gsize)dst_stride);
        }
        if (inst->frameBuf) {
            /* BGRA → RGBA swizzle during copy so Go can memcpy directly. */
            for (int y = 0; y < h; y++) {
                const uint8_t *src_row = pixels + y * stride;
                uint8_t       *dst_row = inst->frameBuf + y * dst_stride;
                for (int x = 0; x < w; x++) {
                    dst_row[x*4+0] = src_row[x*4+2]; /* R */
                    dst_row[x*4+1] = src_row[x*4+1]; /* G */
                    dst_row[x*4+2] = src_row[x*4+0]; /* B */
                    dst_row[x*4+3] = 255;             /* A — force opaque */
                }
            }
            inst->frameW      = w;
            inst->frameH      = h;
            inst->frameStride = dst_stride;
            inst->frameReady  = 1;
            inst->lastFrameTimeUs = now;
        }
        pthread_mutex_unlock(&inst->frameMu);
    }

    if (bo) {
        if (map_data)
            gbm_bo_unmap(bo, map_data);
        gbm_bo_destroy(bo);
    }

    goFrameReady(inst->goHandle);

    /* Chain to the original WPEViewHeadless render_buffer so that WPE's
       internal buffer pool management (recycling, fd cleanup) works.
       Without this, WPE allocates a new DMA-BUF every frame. */
    if (_orig_render_buffer)
        return _orig_render_buffer(view, buffer, rects, n_rects, error);
    wpe_view_buffer_rendered(view, buffer);
    return TRUE;
}

/* ── g_idle_add helpers ──────────────────────────────────────────────────
   Each runs on the loop thread.  The data pointer carries the target
   instance (and any extra args). */

typedef struct { WebViewInstance *inst; char *url; } NavigateArgs;

static gboolean idle_navigate(gpointer data)
{
    NavigateArgs *a = (NavigateArgs *)data;
    if (a->inst->webView)
        webkit_web_view_load_uri(a->inst->webView, a->url);
    free(a->url);
    free(a);
    return G_SOURCE_REMOVE;
}

static gboolean idle_go_back(gpointer data)
{
    WebViewInstance *inst = (WebViewInstance *)data;
    if (inst->webView) webkit_web_view_go_back(inst->webView);
    return G_SOURCE_REMOVE;
}

static gboolean idle_go_forward(gpointer data)
{
    WebViewInstance *inst = (WebViewInstance *)data;
    if (inst->webView) webkit_web_view_go_forward(inst->webView);
    return G_SOURCE_REMOVE;
}

static gboolean idle_reload(gpointer data)
{
    WebViewInstance *inst = (WebViewInstance *)data;
    if (inst->webView) webkit_web_view_reload(inst->webView);
    return G_SOURCE_REMOVE;
}

static gboolean idle_stop(gpointer data)
{
    WebViewInstance *inst = (WebViewInstance *)data;
    if (inst->webView) webkit_web_view_stop_loading(inst->webView);
    return G_SOURCE_REMOVE;
}

typedef struct { WebViewInstance *inst; int w, h; } SizeArgs;

static gboolean idle_set_size(gpointer data)
{
    SizeArgs *a = (SizeArgs *)data;
    if (a->inst->wpeView)
        wpe_view_resized(a->inst->wpeView, a->w, a->h);
    if (a->inst->toplevel)
        wpe_toplevel_resize(a->inst->toplevel, a->w, a->h);
    free(a);
    return G_SOURCE_REMOVE;
}

/* ── Signal handlers ─────────────────────────────────────────────────── */

static void on_web_process_terminated(WebKitWebView *wv,
                                      WebKitWebProcessTerminationReason reason,
                                      gpointer user_data)
{
    (void)user_data;
    const char *msg = "unknown";
    switch (reason) {
    case WEBKIT_WEB_PROCESS_CRASHED:               msg = "crashed";               break;
    case WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT:  msg = "exceeded memory limit";  break;
    case WEBKIT_WEB_PROCESS_TERMINATED_BY_API:      msg = "terminated by API";      break;
    }
    fprintf(stderr, "webview_linux: web process %s — reloading\n", msg);
    webkit_web_view_reload(wv);
}

/* ── One-time global initialisation (runs on the loop thread) ─────────── */

static int global_init(void)
{
    GError *error = NULL;

    /* Open a DRI render node for GBM so we can de-tile DMA-BUF frames. */
    const char *render_nodes[] = {
        "/dev/dri/renderD128", "/dev/dri/renderD129", NULL
    };
    for (int i = 0; render_nodes[i] && _drmFd < 0; i++)
        _drmFd = open(render_nodes[i], O_RDWR);
    if (_drmFd >= 0)
        _gbm = gbm_create_device(_drmFd);
    if (!_gbm)
        fprintf(stderr, "webview_linux: no GBM device (DMA-BUF frames will be unavailable)\n");

    /* Create and connect the headless display, shared by every instance. */
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

    /* ── Memory pressure — MUST be set before any WebKitNetworkSession
       or WebKitWebContext is touched, otherwise the defaults are already
       baked in and these calls are silently ignored. ────────────────── */
    WebKitMemoryPressureSettings *mem = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_memory_limit(mem, 256);
    webkit_memory_pressure_settings_set_conservative_threshold(mem, 0.3);
    webkit_memory_pressure_settings_set_strict_threshold(mem, 0.5);
    webkit_memory_pressure_settings_set_kill_threshold(mem, 0.8);
    /* Poll every second so the pressure handler fires even without
       cgroup memory-pressure notifications from the OS. */
    webkit_memory_pressure_settings_set_poll_interval(mem, 1.0);
    webkit_network_session_set_memory_pressure_settings(mem);
    webkit_memory_pressure_settings_free(mem);

    /* Now it is safe to touch the web context / cache model. */
    webkit_web_context_set_cache_model(
        webkit_web_context_get_default(),
        WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);

    return 1;
}

/* ── Loop readiness handshake ────────────────────────────────────────── */

static pthread_mutex_t _readyMu   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  _readyCond = PTHREAD_COND_INITIALIZER;
static int             _ready     = 0;   /* 0 = pending, 1 = ok, -1 = failed */

void WebView_RunLoop(void)
{
    int ok = global_init();

    pthread_mutex_lock(&_readyMu);
    _ready = ok ? 1 : -1;
    pthread_cond_broadcast(&_readyCond);
    pthread_mutex_unlock(&_readyMu);

    if (!ok)
        return;

    /* Run the real GLib main loop so that all GLib/WebKit timers,
       idle handlers, and — critically — the memory-pressure poller
       fire on schedule. */
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);          /* blocks forever */
    g_main_loop_unref(loop);
}

int WebView_WaitReady(void)
{
    pthread_mutex_lock(&_readyMu);
    while (_ready == 0)
        pthread_cond_wait(&_readyCond, &_readyMu);
    int r = _ready;
    pthread_mutex_unlock(&_readyMu);
    return r == 1 ? 1 : 0;
}

/* ── Instance creation (runs on the loop thread) ─────────────────────── */

static WebViewInstance *instance_create(int width, int height, uintptr_t goHandle)
{
    WebViewInstance *inst = calloc(1, sizeof *inst);
    if (!inst)
        return NULL;

    pthread_mutex_init(&inst->frameMu, NULL);
    pthread_mutex_init(&inst->moveMu, NULL);
    pthread_mutex_init(&inst->scrollMu, NULL);
    inst->w        = width;
    inst->h        = height;
    inst->goHandle = goHandle;

    WebKitSettings *settings = webkit_settings_new();
    webkit_settings_set_enable_page_cache(settings, FALSE);
    webkit_settings_set_enable_html5_local_storage(settings, FALSE);
    webkit_settings_set_enable_html5_database(settings, FALSE);
    webkit_settings_set_enable_webaudio(settings, FALSE);
    webkit_settings_set_enable_webgl(settings, FALSE);
    webkit_settings_set_enable_2d_canvas_acceleration(settings, FALSE);
    webkit_settings_set_enable_mediasource(settings, FALSE);
    webkit_settings_set_enable_encrypted_media(settings, FALSE);
    webkit_settings_set_enable_media_capabilities(settings, FALSE);
    webkit_settings_set_enable_media_stream(settings, FALSE);

    /* Create the WebKitWebView using the primary (headless) display. */
    inst->webView = g_object_ref_sink(
        WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
                                     "settings", settings,
                                     NULL)));
    g_object_unref(settings);
    if (!inst->webView) {
        fprintf(stderr, "webview_linux: webkit_web_view_new() failed\n");
        free(inst);
        return NULL;
    }

    /* When the web process crashes or is killed for exceeding memory,
       reload the current page to restart the process. */
    g_signal_connect(inst->webView, "web-process-terminated",
        G_CALLBACK(on_web_process_terminated), NULL);

    /* Retrieve the underlying WPEView. */
    inst->wpeView = webkit_web_view_get_wpe_view(inst->webView);
    if (!inst->wpeView) {
        fprintf(stderr, "webview_linux: webkit_web_view_get_wpe_view() returned NULL\n");
        g_object_unref(inst->webView);
        free(inst);
        return NULL;
    }

    /* Let our_render_buffer find this instance from the view. */
    g_object_set_data(G_OBJECT(inst->wpeView), WV_INSTANCE_KEY, inst);

    /* The WebView may have created its own display internally (different
       pointer from _display).  Always use the view's actual display for the
       toplevel so the pointer-equality assertion in wpe_view_set_toplevel
       is satisfied. */
    WPEDisplay *viewDisplay = webkit_web_view_get_display(inst->webView);
    if (!viewDisplay) {
        fprintf(stderr, "webview_linux: webkit_web_view_get_display() returned NULL\n");
        g_object_unref(inst->webView);
        free(inst);
        return NULL;
    }

    inst->toplevel = wpe_display_create_toplevel(viewDisplay, 1);
    if (!inst->toplevel) {
        fprintf(stderr, "webview_linux: wpe_display_create_toplevel() returned NULL\n");
        g_object_unref(inst->webView);
        free(inst);
        return NULL;
    }
    wpe_toplevel_resize(inst->toplevel, width, height);
    wpe_view_set_toplevel(inst->wpeView, inst->toplevel);
    wpe_view_resized(inst->wpeView, width, height);

    /* Patch the WPEViewHeadless class vtable to capture rendered frames.
       The vtable is shared by every instance, so do it only once and save
       the original so we can chain to it for proper buffer lifecycle. */
    if (!_orig_render_buffer) {
        _orig_render_buffer = WPE_VIEW_GET_CLASS(inst->wpeView)->render_buffer;
        WPE_VIEW_GET_CLASS(inst->wpeView)->render_buffer = our_render_buffer;
    }

    /* Map the view so WPE starts rendering. */
    wpe_view_map(inst->wpeView);

    return inst;
}

typedef struct {
    int             width;
    int             height;
    uintptr_t       goHandle;
    WebViewInstance *result;
    pthread_mutex_t mu;
    pthread_cond_t  cond;
    int             done;
} CreateArgs;

static gboolean idle_create(gpointer data)
{
    CreateArgs *a = (CreateArgs *)data;
    WebViewInstance *inst = instance_create(a->width, a->height, a->goHandle);

    pthread_mutex_lock(&a->mu);
    a->result = inst;
    a->done   = 1;
    pthread_cond_signal(&a->cond);
    pthread_mutex_unlock(&a->mu);
    return G_SOURCE_REMOVE;
}

WebViewInstance *WebView_Create(int width, int height, uintptr_t goHandle)
{
    CreateArgs a;
    a.width    = width;
    a.height   = height;
    a.goHandle = goHandle;
    a.result   = NULL;
    a.done     = 0;
    pthread_mutex_init(&a.mu, NULL);
    pthread_cond_init(&a.cond, NULL);

    /* Create on the loop thread so all GLib/WebKit objects are created and
       used on the thread that iterates their main context. */
    g_idle_add(idle_create, &a);

    pthread_mutex_lock(&a.mu);
    while (!a.done)
        pthread_cond_wait(&a.cond, &a.mu);
    WebViewInstance *inst = a.result;
    pthread_mutex_unlock(&a.mu);

    pthread_mutex_destroy(&a.mu);
    pthread_cond_destroy(&a.cond);
    return inst;
}

/* ── Instance teardown (runs on the loop thread) ─────────────────────── */

typedef struct {
    WebViewInstance *inst;
    pthread_mutex_t  mu;
    pthread_cond_t   cond;
    int              done;
} DestroyArgs;

static gboolean idle_destroy(gpointer data)
{
    DestroyArgs *a = (DestroyArgs *)data;
    WebViewInstance *inst = a->inst;

    if (inst->wpeView) {
        /* Stop frames routing to this instance before we free it. */
        g_object_set_data(G_OBJECT(inst->wpeView), WV_INSTANCE_KEY, NULL);
        wpe_view_unmap(inst->wpeView);
    }
    if (inst->webView) {
        g_signal_handlers_disconnect_by_data(inst->webView, NULL);
        g_object_unref(inst->webView);   /* also disposes its WPEView */
    }
    if (inst->toplevel)
        g_object_unref(inst->toplevel);

    pthread_mutex_lock(&inst->frameMu);
    free(inst->frameBuf);
    inst->frameBuf = NULL;
    pthread_mutex_unlock(&inst->frameMu);

    pthread_mutex_destroy(&inst->frameMu);
    pthread_mutex_destroy(&inst->moveMu);
    pthread_mutex_destroy(&inst->scrollMu);
    free(inst);

    pthread_mutex_lock(&a->mu);
    a->done = 1;
    pthread_cond_signal(&a->cond);
    pthread_mutex_unlock(&a->mu);
    return G_SOURCE_REMOVE;
}

void WebView_Destroy(WebViewInstance *inst)
{
    if (!inst)
        return;

    DestroyArgs a;
    a.inst = inst;
    a.done = 0;
    pthread_mutex_init(&a.mu, NULL);
    pthread_cond_init(&a.cond, NULL);

    g_idle_add(idle_destroy, &a);

    pthread_mutex_lock(&a.mu);
    while (!a.done)
        pthread_cond_wait(&a.cond, &a.mu);
    pthread_mutex_unlock(&a.mu);

    pthread_mutex_destroy(&a.mu);
    pthread_cond_destroy(&a.cond);
}

/* ── Geometry ────────────────────────────────────────────────────────── */

void WebView_SetFrame(WebViewInstance *inst, int x, int y, int width, int height)
{
    (void)x; (void)y;   /* headless has no screen position */
    if (!inst)
        return;

    pthread_mutex_lock(&inst->frameMu);
    inst->w = width;
    inst->h = height;
    pthread_mutex_unlock(&inst->frameMu);

    SizeArgs *a = malloc(sizeof *a);
    if (a) {
        a->inst = inst;
        a->w    = width;
        a->h    = height;
        g_idle_add(idle_set_size, a);
    }
}

typedef struct { WebViewInstance *inst; double scale; } ScaleArgs;

static gboolean idle_set_scale(gpointer data)
{
    ScaleArgs *a = (ScaleArgs *)data;
    if (a->inst->toplevel)
        wpe_toplevel_scale_changed(a->inst->toplevel, a->scale);
    free(a);
    return G_SOURCE_REMOVE;
}

void WebView_SetScale(WebViewInstance *inst, double scale)
{
    if (!inst)
        return;
    ScaleArgs *a = malloc(sizeof *a);
    if (a) {
        a->inst  = inst;
        a->scale = scale;
        g_idle_add(idle_set_scale, a);
    }
}

/* ── Appearance ──────────────────────────────────────────────────────── */

static gboolean idle_set_dark_mode(gpointer data)
{
    gboolean dark = GPOINTER_TO_INT(data);
    WPEDisplay *d = _display;
    if (!d) d = wpe_display_get_primary();
    if (d) {
        WPESettings *settings = wpe_display_get_settings(d);
        if (settings)
            wpe_settings_set_boolean(settings, WPE_SETTING_DARK_MODE, dark,
                                     WPE_SETTINGS_SOURCE_APPLICATION, NULL);
    }
    return G_SOURCE_REMOVE;
}

void WebView_SetDarkMode(WebViewInstance *inst, int dark)
{
    (void)inst;   /* dark mode is a property of the shared display */
    g_idle_add(idle_set_dark_mode, GINT_TO_POINTER(dark));
}

/* ── Navigation ──────────────────────────────────────────────────────── */

void WebView_Navigate(WebViewInstance *inst, const char *url)
{
    if (!inst || !url)
        return;
    NavigateArgs *a = malloc(sizeof *a);
    if (a) {
        a->inst = inst;
        a->url  = strdup(url);
        g_idle_add(idle_navigate, a);
    }
}

void WebView_GoBack(WebViewInstance *inst)    { if (inst) g_idle_add(idle_go_back,    inst); }
void WebView_GoForward(WebViewInstance *inst) { if (inst) g_idle_add(idle_go_forward, inst); }
void WebView_Reload(WebViewInstance *inst)    { if (inst) g_idle_add(idle_reload,     inst); }
void WebView_Stop(WebViewInstance *inst)      { if (inst) g_idle_add(idle_stop,       inst); }

/* ── Input events ────────────────────────────────────────────────────── */

typedef struct { WebViewInstance *inst; double x, y; int button; int down; } MouseBtnArgs;

static guint32 _ms_now(void) { return (guint32)(g_get_monotonic_time() / 1000); }

static gboolean idle_mouse_btn(gpointer data)
{
    MouseBtnArgs *a = data;
    WPEView *view = a->inst->wpeView;
    if (view) {
        WPEEvent *ev = wpe_event_pointer_button_new(
            a->down ? WPE_EVENT_POINTER_DOWN : WPE_EVENT_POINTER_UP,
            view, WPE_INPUT_SOURCE_MOUSE, _ms_now(),
            0, a->button, a->x, a->y,
            a->down ? wpe_view_compute_press_count(view,
                          a->x, a->y, a->button, _ms_now()) : 0);
        wpe_view_event(view, ev);
        wpe_event_unref(ev);
    }
    free(a);
    return G_SOURCE_REMOVE;
}

static gboolean idle_mouse_move(gpointer data)
{
    WebViewInstance *inst = (WebViewInstance *)data;
    pthread_mutex_lock(&inst->moveMu);
    double x = inst->moveX;
    double y = inst->moveY;
    inst->movePending = 0;
    pthread_mutex_unlock(&inst->moveMu);

    if (inst->wpeView) {
        WPEEvent *ev = wpe_event_pointer_move_new(
            WPE_EVENT_POINTER_MOVE, inst->wpeView,
            WPE_INPUT_SOURCE_MOUSE, _ms_now(),
            0, x, y, 0, 0);
        wpe_view_event(inst->wpeView, ev);
        wpe_event_unref(ev);
    }
    return G_SOURCE_REMOVE;
}

static gboolean idle_scroll(gpointer data)
{
    WebViewInstance *inst = (WebViewInstance *)data;
    pthread_mutex_lock(&inst->scrollMu);
    double x  = inst->scrollX;
    double y  = inst->scrollY;
    double dx = inst->scrollDX;
    double dy = inst->scrollDY;
    inst->scrollDX = 0;
    inst->scrollDY = 0;
    inst->scrollPending = 0;
    pthread_mutex_unlock(&inst->scrollMu);

    if (inst->wpeView) {
        WPEEvent *ev = wpe_event_scroll_new(
            inst->wpeView, WPE_INPUT_SOURCE_MOUSE, _ms_now(),
            0, dx, dy, FALSE, FALSE, x, y);
        wpe_view_event(inst->wpeView, ev);
        wpe_event_unref(ev);
    }
    return G_SOURCE_REMOVE;
}

void WebView_MouseDown(WebViewInstance *inst, double x, double y, int button)
{
    if (!inst) return;
    MouseBtnArgs *a = malloc(sizeof *a);
    if (a) { *a = (MouseBtnArgs){inst, x, y, button, 1}; g_idle_add(idle_mouse_btn, a); }
}

void WebView_MouseUp(WebViewInstance *inst, double x, double y, int button)
{
    if (!inst) return;
    MouseBtnArgs *a = malloc(sizeof *a);
    if (a) { *a = (MouseBtnArgs){inst, x, y, button, 0}; g_idle_add(idle_mouse_btn, a); }
}

void WebView_MouseMove(WebViewInstance *inst, double x, double y)
{
    if (!inst) return;
    pthread_mutex_lock(&inst->moveMu);
    inst->moveX = x;
    inst->moveY = y;
    int was_pending = inst->movePending;
    inst->movePending = 1;
    pthread_mutex_unlock(&inst->moveMu);

    if (!was_pending)
        g_idle_add(idle_mouse_move, inst);
}

void WebView_Scroll(WebViewInstance *inst, double x, double y, double dx, double dy)
{
    if (!inst) return;
    pthread_mutex_lock(&inst->scrollMu);
    inst->scrollX   = x;
    inst->scrollY   = y;
    inst->scrollDX += dx;
    inst->scrollDY += dy;
    int was_pending = inst->scrollPending;
    inst->scrollPending = 1;
    pthread_mutex_unlock(&inst->scrollMu);

    if (!was_pending)
        g_idle_add(idle_scroll, inst);
}

typedef struct { WebViewInstance *inst; guint keyval; int down; } KeyArgs;

static gboolean idle_key(gpointer data)
{
    KeyArgs *a = data;
    if (a->inst->wpeView) {
        /* Look up the hardware keycode via the shared display keymap. */
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
            a->inst->wpeView, WPE_INPUT_SOURCE_KEYBOARD, _ms_now(),
            0, keycode, a->keyval);
        wpe_view_event(a->inst->wpeView, ev);
        wpe_event_unref(ev);
    }
    free(a);
    return G_SOURCE_REMOVE;
}

void WebView_KeyDown(WebViewInstance *inst, unsigned int keyval)
{
    if (!inst) return;
    KeyArgs *a = malloc(sizeof *a);
    if (a) { *a = (KeyArgs){inst, keyval, 1}; g_idle_add(idle_key, a); }
}

void WebView_KeyUp(WebViewInstance *inst, unsigned int keyval)
{
    if (!inst) return;
    KeyArgs *a = malloc(sizeof *a);
    if (a) { *a = (KeyArgs){inst, keyval, 0}; g_idle_add(idle_key, a); }
}

/* ── State queries ───────────────────────────────────────────────────── */

int WebView_IsLoading(WebViewInstance *inst)
{
    return (inst && inst->webView && webkit_web_view_is_loading(inst->webView)) ? 1 : 0;
}

const char *WebView_GetURL(WebViewInstance *inst)
{
    if (!inst || !inst->webView)
        return "";
    const char *uri = webkit_web_view_get_uri(inst->webView);
    return uri ? uri : "";
}

const uint8_t *WebView_LockFrame(WebViewInstance *inst, int *out_w, int *out_h, int *out_stride)
{
    if (!inst)
        return NULL;
    pthread_mutex_lock(&inst->frameMu);
    if (!inst->frameReady || !inst->frameBuf) {
        pthread_mutex_unlock(&inst->frameMu);
        return NULL;
    }
    inst->frameReady = 0;    /* mark consumed so we don't re-copy unchanged frames */
    *out_w      = inst->frameW;
    *out_h      = inst->frameH;
    *out_stride = inst->frameStride;
    return inst->frameBuf;   /* caller holds frameMu until WebView_UnlockFrame */
}

void WebView_UnlockFrame(WebViewInstance *inst)
{
    if (inst)
        pthread_mutex_unlock(&inst->frameMu);
}
