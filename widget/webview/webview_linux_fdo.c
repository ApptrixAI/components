//go:build linux && !android && wpe_fdo

/*
 * webview_linux_fdo.c — legacy WPEBackend-FDO rendering into a pixel buffer.
 *
 * This is the fallback backend, selected with `-tags wpe_fdo`, for systems
 * whose wpewebkit is built WITHOUT the modern WPE Platform API / headless
 * platform (notably Debian).  It implements the same webview_linux.h C API as
 * the default headless backend (webview_linux.c) but is built on the older
 * libwpe + WPEBackend-FDO stack:
 *
 *   • A single shared GLib main loop (WebView_RunLoop) runs on a dedicated
 *     Go goroutine and performs one-time global init: wpe_fdo_initialize_shm()
 *     and the process-wide memory-pressure / cache settings.
 *   • Each widget is a WebViewInstance owning its own WebKitWebView plus an
 *     "exportable" FDO view backend.  WebKit renders offscreen and exports each
 *     frame as a wl_shm buffer through export_shm_buffer; we copy the pixels out
 *     (BGRA→RGBA), then release the buffer and signal frame-complete so WebKit
 *     renders the next frame.
 *   • goFrameReady(goHandle) (exported from Go) is called after each frame copy
 *     so the Go side can refresh the right canvas.Image widget.
 *
 * The SHM (rather than EGL) exportable is used deliberately: it hands us CPU
 * pixels directly, so no EGL/GL context is required to read frames back.
 *
 * Feature parity note: the legacy stack has no shared WPEDisplay/WPESettings to
 * carry WebKit's real prefers-color-scheme, so WebView_SetDarkMode is emulated
 * by injecting a color-scheme USER stylesheet plus a matchMedia override (see
 * COLORSCHEME_SCRIPT).  That covers UA-rendered pixels and JS feature
 * detection; pages that switch purely via CSS `@media (prefers-color-scheme)`
 * rules — a signal only the engine can set — may not fully follow.
 */

#include "webview_linux.h"

/* WPEBackend-FDO — legacy libwpe path */
#include <wpe/webkit.h>
#include <jsc/jsc.h>

#define WPE_FDO_COMPILATION 0
#include <wpe/fdo.h>
#define __WPE_FDO_SHM_H_INSIDE__
#include <wpe/unstable/fdo-shm.h>
#undef __WPE_FDO_SHM_H_INSIDE__

#include <wpe/wpe.h>              /* libwpe: view backend + input events */
#include <wayland-server-core.h> /* wl_shm_buffer accessors */

#include <glib.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

/* ── Go callback (exported from the Go package) ──────────────────────── */

extern void goFrameReady(uintptr_t goHandle);
extern void goTitleChanged(uintptr_t goHandle);
extern void goFaviconChanged(uintptr_t goHandle);

/* ── Minimum interval between frame copies (microseconds). ───────────────
   Frames arriving faster than this are released back to WPE immediately
   without the expensive pixel copy. */
#define FRAME_MIN_INTERVAL_US (1000000 / 60)   /* ~60 fps */

/* ── Per-instance state ──────────────────────────────────────────────── */

struct WebViewInstance {
    WebKitWebView                          *webView;
    struct wpe_view_backend_exportable_fdo *exportable;
    struct wpe_view_backend                *backend;   /* owned by webView */

    /* Kept for runtime dark-mode toggling: the color-scheme user stylesheet
       is removed and re-added when the mode changes.  dark is the last mode
       pushed, re-applied to each freshly committed document. */
    WebKitUserContentManager *ucm;
    int                       dark;

    uintptr_t      goHandle;   /* opaque token for goFrameReady */

    int            w;
    int            h;
    double         scale;      /* last device scale factor pushed to WebKit */

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

    /* Page metadata: the title (cached from notify::title) and the favicon
       decoded to RGBA by the page itself (WPE exposes no native favicon API).
       Both are guarded by metaMu and read out by the Go side on demand. */
    pthread_mutex_t metaMu;
    char           *title;
    uint8_t        *favBuf;   /* RGBA8888, favW*favH*4 bytes, or NULL */
    int             favW;
    int             favH;
};

/* Injected into every top frame at document-end.  WPE has no favicon API, so
   the page locates its own icon, lets the engine decode whatever format it is
   (.ico/.png/.svg/.webp), rasterises it to RGBA via a canvas, and posts the
   pixels back through the "favicon" script message handler.  A MutationObserver
   re-runs it when the document's icon links change. */
static const char *FAVICON_SCRIPT =
"(function(){"
"  var MAX=64, last=null;"
"  function pick(){"
"    var ls=document.querySelectorAll('link[rel~=\"icon\"]'),best=null,bs=-1;"
"    for(var i=0;i<ls.length;i++){var l=ls[i];if(!l.href)continue;"
"      var s=0,m=l.sizes&&l.sizes.value&&/(\\d+)x(\\d+)/.exec(l.sizes.value);"
"      if(m)s=parseInt(m[1],10);if(s>=bs){bs=s;best=l.href;}}"
"    return best||(location.origin+'/favicon.ico');"
"  }"
"  function send(u){"
"    var img=new Image();img.crossOrigin='anonymous';"
"    img.onload=function(){try{"
"      var w=img.naturalWidth||img.width,h=img.naturalHeight||img.height;if(!w||!h)return;"
"      if(w>MAX||h>MAX){var sc=MAX/Math.max(w,h);w=Math.round(w*sc);h=Math.round(h*sc);}"
"      var c=document.createElement('canvas');c.width=w;c.height=h;"
"      var x=c.getContext('2d');x.drawImage(img,0,0,w,h);"
"      var d=x.getImageData(0,0,w,h).data;"
"      var b=btoa(String.fromCharCode.apply(null,d));"
"      window.webkit.messageHandlers.favicon.postMessage(w+' '+h+' '+b);"
"    }catch(e){}};"
"    img.src=u;"
"  }"
"  function update(){var u=pick();if(u!==last){last=u;send(u);}}"
"  update();"
"  try{new MutationObserver(update).observe(document.head||document.documentElement,"
"    {childList:true,subtree:true,attributes:true,attributeFilter:['href','rel','sizes']});}catch(e){}"
"})();";

/* Dark-mode support for the legacy backend, which cannot set WebKit's real
   prefers-color-scheme.  The color-scheme itself is forced with an injected
   USER stylesheet (see apply_dark_stylesheet), which covers UA-rendered pixels
   — default backgrounds, form controls, scrollbars.  This companion script,
   injected at document-start into every frame, additionally makes JS feature
   detection agree: it overrides window.matchMedia so prefers-color-scheme
   queries report the forced value and deliver change events to listeners.
   The current value is pushed in via window.__wvApplyDark (from C) on each
   document commit and whenever the mode toggles. */
static const char *COLORSCHEME_SCRIPT =
"(function(){"
"  if(window.__wvApplyDark)return;"
"  var dark=false, mqls=[];"
"  function fire(){"
"    for(var i=0;i<mqls.length;i++){var m=mqls[i],ev;"
"      try{ev=new MediaQueryListEvent('change',{matches:m.matches,media:m.media});}"
"      catch(e){ev={type:'change',media:m.media,matches:m.matches};}"
"      var ls=(m.__wvL||[]).slice();"
"      for(var j=0;j<ls.length;j++){try{ls[j].call(m,ev);}catch(e){}}"
"      if(typeof m.onchange==='function'){try{m.onchange.call(m,ev);}catch(e){}}}"
"  }"
"  window.__wvApplyDark=function(d){dark=!!d;fire();};"
"  var orig=window.matchMedia;"
"  if(!orig)return;"
"  window.matchMedia=function(q){"
"    var mql=orig.call(this,q);"
"    if(mql&&/prefers-color-scheme/i.test(String(q))){"
"      var wantsDark=!/light/i.test(String(q));"
"      try{Object.defineProperty(mql,'matches',{configurable:true,"
"        get:function(){return wantsDark?dark:!dark;}});}catch(e){}"
"      mql.__wvL=[];"
"      var _a=mql.addEventListener&&mql.addEventListener.bind(mql);"
"      var _r=mql.removeEventListener&&mql.removeEventListener.bind(mql);"
"      mql.addEventListener=function(t,cb){if(t==='change'&&cb){mql.__wvL.push(cb);}else if(_a){_a.apply(null,arguments);}};"
"      mql.removeEventListener=function(t,cb){if(t==='change'){var k=mql.__wvL.indexOf(cb);if(k>=0)mql.__wvL.splice(k,1);}else if(_r){_r.apply(null,arguments);}};"
"      mql.addListener=function(cb){if(cb)mql.__wvL.push(cb);};"
"      mql.removeListener=function(cb){var k=mql.__wvL.indexOf(cb);if(k>=0)mql.__wvL.splice(k,1);};"
"      mqls.push(mql);"
"    }"
"    return mql;"
"  };"
"})();";

/* ── SHM frame export ────────────────────────────────────────────────────
   Runs on the loop thread when WebKit has a new frame.  We copy the pixels
   out (BGRA→RGBA), then always release the buffer and signal frame-complete
   so WebKit can render the next frame — even when we skip the copy for
   rate-limiting, the buffer must be returned or rendering stalls. */

static void on_export_shm_buffer(void *data,
                                 struct wpe_fdo_shm_exported_buffer *buffer)
{
    WebViewInstance *inst = (WebViewInstance *)data;

    int64_t now = g_get_monotonic_time();
    int rate_limited = (now - inst->lastFrameTimeUs < FRAME_MIN_INTERVAL_US);

    struct wl_shm_buffer *shm =
        buffer ? wpe_fdo_shm_exported_buffer_get_shm_buffer(buffer) : NULL;

    if (shm && !rate_limited) {
        wl_shm_buffer_begin_access(shm);
        const uint8_t *pixels = (const uint8_t *)wl_shm_buffer_get_data(shm);
        int w      = wl_shm_buffer_get_width(shm);
        int h      = wl_shm_buffer_get_height(shm);
        int stride = wl_shm_buffer_get_stride(shm);

        if (pixels && w > 0 && h > 0 && stride > 0) {
            int dst_stride = w * 4;
            pthread_mutex_lock(&inst->frameMu);
            if (!inst->frameBuf || inst->frameW != w || inst->frameH != h) {
                free(inst->frameBuf);
                inst->frameBuf = malloc((gsize)h * (gsize)dst_stride);
            }
            if (inst->frameBuf) {
                /* BGRA → RGBA swizzle during copy so Go can memcpy directly. */
                for (int y = 0; y < h; y++) {
                    const uint8_t *src_row = pixels + (gsize)y * stride;
                    uint8_t       *dst_row = inst->frameBuf + (gsize)y * dst_stride;
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
        wl_shm_buffer_end_access(shm);

        goFrameReady(inst->goHandle);
    }

    /* Always return the buffer and signal frame-complete so rendering
       continues, whether or not we copied this frame. */
    if (buffer)
        wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(
            inst->exportable, buffer);
    wpe_view_backend_exportable_fdo_dispatch_frame_complete(inst->exportable);
}

static const struct wpe_view_backend_exportable_fdo_client s_exportable_client = {
    .export_buffer_resource = NULL,   /* unused: SHM init only exports SHM */
    .export_dmabuf_resource = NULL,
    .export_shm_buffer      = on_export_shm_buffer,
    ._wpe_reserved0         = NULL,
    ._wpe_reserved1         = NULL,
};

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
    if (a->inst->backend)
        wpe_view_backend_dispatch_set_size(a->inst->backend,
                                           (uint32_t)a->w, (uint32_t)a->h);
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
    fprintf(stderr, "webview_linux(fdo): web process %s — reloading\n", msg);
    webkit_web_view_reload(wv);
}

/* notify::title — cache the new title and tell Go.  Runs on the loop thread. */
static void on_notify_title(GObject *obj, GParamSpec *pspec, gpointer data)
{
    (void)obj; (void)pspec;
    WebViewInstance *inst = (WebViewInstance *)data;
    const char *t = webkit_web_view_get_title(inst->webView);

    pthread_mutex_lock(&inst->metaMu);
    free(inst->title);
    inst->title = (t && *t) ? strdup(t) : NULL;
    pthread_mutex_unlock(&inst->metaMu);

    goTitleChanged(inst->goHandle);
}

/* "favicon" script message — the page posts "W H base64RGBA"; decode and store
   the pixels, then tell Go.  Runs on the loop thread. */
static void on_favicon_message(WebKitUserContentManager *m, JSCValue *value,
                               gpointer data)
{
    (void)m;
    WebViewInstance *inst = (WebViewInstance *)data;
    if (!jsc_value_is_string(value))
        return;

    char *str = jsc_value_to_string(value);
    if (!str)
        return;

    char *p = str, *end = NULL;
    long w = strtol(p, &end, 10);
    if (end == p) { g_free(str); return; }
    p = end;
    long h = strtol(p, &end, 10);
    if (end == p) { g_free(str); return; }
    p = end;
    while (*p == ' ') p++;

    gsize outlen = 0;
    guchar *rgba = g_base64_decode(p, &outlen);
    if (rgba && w > 0 && h > 0 && outlen == (gsize)w * (gsize)h * 4) {
        pthread_mutex_lock(&inst->metaMu);
        free(inst->favBuf);
        inst->favBuf = malloc(outlen);
        if (inst->favBuf) {
            memcpy(inst->favBuf, rgba, outlen);
            inst->favW = (int)w;
            inst->favH = (int)h;
        }
        pthread_mutex_unlock(&inst->metaMu);
        g_free(rgba);
        goFaviconChanged(inst->goHandle);
    } else {
        g_free(rgba);
    }
    g_free(str);
}

/* ── Dark mode (runs on the loop thread) ─────────────────────────────────
   The legacy stack has no shared WPEDisplay/WPESettings to carry the real
   prefers-color-scheme, so we force the color-scheme with a USER stylesheet
   (visible pixels: default backgrounds, form controls, scrollbars) and push
   the same value into the matchMedia override (JS feature detection). */

static void apply_dark_stylesheet(WebViewInstance *inst)
{
    if (!inst->ucm)
        return;
    /* Only the color-scheme sheet is ever added here, so clearing all user
       style sheets before re-adding is safe (scripts are a separate list). */
    webkit_user_content_manager_remove_all_style_sheets(inst->ucm);
    const char *css = inst->dark
        ? ":root{color-scheme:dark!important}"
        : ":root{color-scheme:light!important}";
    WebKitUserStyleSheet *ss = webkit_user_style_sheet_new(
        css, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_STYLE_LEVEL_USER, NULL, NULL);
    webkit_user_content_manager_add_style_sheet(inst->ucm, ss);
    webkit_user_style_sheet_unref(ss);
}

/* Push the current mode into the page's matchMedia override.  A newly loaded
   document re-runs COLORSCHEME_SCRIPT (which defaults to light), so this must
   be re-applied on every commit as well as on toggle. */
static void apply_dark_js(WebViewInstance *inst)
{
    if (!inst->webView)
        return;
    char *js = g_strdup_printf(
        "window.__wvApplyDark&&window.__wvApplyDark(%s)",
        inst->dark ? "true" : "false");
    webkit_web_view_evaluate_javascript(inst->webView, js, -1,
                                        NULL, NULL, NULL, NULL, NULL);
    g_free(js);
}

/* Re-push the JS dark state once each new document has committed; the
   stylesheet persists across loads on its own. */
static void on_load_changed(WebKitWebView *wv, WebKitLoadEvent event, gpointer data)
{
    (void)wv;
    if (event == WEBKIT_LOAD_COMMITTED)
        apply_dark_js((WebViewInstance *)data);
}

/* ── One-time global initialisation (runs on the loop thread) ─────────── */

static int global_init(void)
{
    /* libwpe dlopens its backend implementation at runtime.  Its default is
       libWPEBackend-default.so, which most distributions don't ship — the FDO
       backend installs as libWPEBackend-fdo-1.0.so.1 (its SONAME, present even
       without -dev packages).  Point the loader at it explicitly, unless the
       caller has already chosen a backend via WPE_BACKEND_LIBRARY. */
    if (!g_getenv("WPE_BACKEND_LIBRARY")) {
        if (!wpe_loader_init("libWPEBackend-fdo-1.0.so.1"))
            fprintf(stderr, "webview_linux(fdo): wpe_loader_init() failed — "
                            "is libWPEBackend-fdo installed?\n");
    }

    /* Bring up the FDO SHM backend.  Unlike the EGL exportable this needs no
       EGL display, so frames come back as CPU-mappable wl_shm buffers. */
    if (!wpe_fdo_initialize_shm()) {
        fprintf(stderr, "webview_linux(fdo): wpe_fdo_initialize_shm() failed\n");
        return 0;
    }

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

    /* Run the real GLib main loop so that all GLib/WebKit timers, idle
       handlers, the FDO wl_display source, and — critically — the
       memory-pressure poller fire on schedule. */
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
    pthread_mutex_init(&inst->metaMu, NULL);
    inst->w        = width;
    inst->h        = height;
    inst->scale    = 1.0;
    inst->goHandle = goHandle;

    /* User content manager: carries the favicon-reading script and receives
       the decoded pixels the page posts back. */
    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler(ucm, "favicon", NULL);
    g_signal_connect(ucm, "script-message-received::favicon",
        G_CALLBACK(on_favicon_message), inst);
    WebKitUserScript *script = webkit_user_script_new(
        FAVICON_SCRIPT,
        WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
        NULL, NULL);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);

    /* Dark-mode matchMedia override — installed early, in every frame. */
    WebKitUserScript *csScript = webkit_user_script_new(
        COLORSCHEME_SCRIPT,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        NULL, NULL);
    webkit_user_content_manager_add_script(ucm, csScript);
    webkit_user_script_unref(csScript);

    /* Keep our own ref so dark mode can toggle the color-scheme stylesheet at
       runtime; released in idle_destroy. */
    inst->ucm = g_object_ref(ucm);

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

    /* Create the exportable FDO view backend that renders offscreen and hands
       us wl_shm frames.  The WebKitWebViewBackend wrapper takes ownership: when
       the web view is destroyed it invokes the destroy-notify below, which
       tears the exportable down — so we never destroy it ourselves. */
    inst->exportable = wpe_view_backend_exportable_fdo_create(
        &s_exportable_client, inst, (uint32_t)width, (uint32_t)height);
    if (!inst->exportable) {
        fprintf(stderr, "webview_linux(fdo): exportable_fdo_create() failed\n");
        g_object_unref(settings);
        g_object_unref(ucm);
        free(inst);
        return NULL;
    }
    inst->backend = wpe_view_backend_exportable_fdo_get_view_backend(inst->exportable);

    WebKitWebViewBackend *vb = webkit_web_view_backend_new(
        inst->backend,
        (GDestroyNotify)wpe_view_backend_exportable_fdo_destroy,
        inst->exportable);

    inst->webView = g_object_ref_sink(
        WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
                                     "backend", vb,
                                     "settings", settings,
                                     "user-content-manager", ucm,
                                     NULL)));
    g_object_unref(settings);
    g_object_unref(ucm);   /* the web view now holds the only ref */
    if (!inst->webView) {
        fprintf(stderr, "webview_linux(fdo): webkit_web_view_new() failed\n");
        /* vb (and thus the exportable) is freed by WebKit's failed construction
           taking the floating backend; nothing else to release here. */
        free(inst);
        return NULL;
    }

    /* When the web process crashes or is killed for exceeding memory,
       reload the current page to restart the process. */
    g_signal_connect(inst->webView, "web-process-terminated",
        G_CALLBACK(on_web_process_terminated), NULL);

    /* Title changes. */
    g_signal_connect(inst->webView, "notify::title",
        G_CALLBACK(on_notify_title), inst);

    /* Re-push dark state to each newly committed document. */
    g_signal_connect(inst->webView, "load-changed",
        G_CALLBACK(on_load_changed), inst);

    /* Mark the offscreen view visible, focused and in-window.  The headless
       backend gets this implicitly from wpe_view_map(); the legacy backend
       does not, and without it WebKit ignores pointer/keyboard input (no
       focus) and throttles rendering as if the page were hidden. */
    wpe_view_backend_add_activity_state(inst->backend,
        wpe_view_activity_state_visible |
        wpe_view_activity_state_focused |
        wpe_view_activity_state_in_window);

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

    if (inst->webView) {
        g_signal_handlers_disconnect_by_data(inst->webView, NULL);
        /* Disposing the view frees its WebKitWebViewBackend, whose destroy
           notify tears down the exportable (and its wpe_view_backend).  After
           this no further export_shm_buffer callback can fire for this inst. */
        g_object_unref(inst->webView);
    }
    if (inst->ucm) {
        g_object_unref(inst->ucm);
        inst->ucm = NULL;
    }
    inst->exportable = NULL;
    inst->backend    = NULL;

    pthread_mutex_lock(&inst->frameMu);
    free(inst->frameBuf);
    inst->frameBuf = NULL;
    pthread_mutex_unlock(&inst->frameMu);

    pthread_mutex_lock(&inst->metaMu);
    free(inst->title);
    inst->title = NULL;
    free(inst->favBuf);
    inst->favBuf = NULL;
    pthread_mutex_unlock(&inst->metaMu);

    pthread_mutex_destroy(&inst->frameMu);
    pthread_mutex_destroy(&inst->moveMu);
    pthread_mutex_destroy(&inst->scrollMu);
    pthread_mutex_destroy(&inst->metaMu);
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
    if (a->scale > 0)
        a->inst->scale = a->scale;   /* used to convert pointer coords below */
    if (a->inst->backend)
        wpe_view_backend_dispatch_set_device_scale_factor(a->inst->backend,
                                                          (float)a->scale);
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

typedef struct { WebViewInstance *inst; int dark; } DarkArgs;

static gboolean idle_set_dark(gpointer data)
{
    DarkArgs *a = (DarkArgs *)data;
    a->inst->dark = a->dark ? 1 : 0;
    apply_dark_stylesheet(a->inst);
    apply_dark_js(a->inst);
    free(a);
    return G_SOURCE_REMOVE;
}

void WebView_SetDarkMode(WebViewInstance *inst, int dark)
{
    if (!inst)
        return;
    DarkArgs *a = malloc(sizeof *a);
    if (a) {
        a->inst = inst;
        a->dark = dark;
        g_idle_add(idle_set_dark, a);
    }
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

static uint32_t _ms_now(void) { return (uint32_t)(g_get_monotonic_time() / 1000); }

/* Legacy libwpe pointer coordinates are in physical device pixels — WebKit's
   event factory divides them by the device scale factor — whereas Fyne reports
   logical coordinates.  Convert using the last scale pushed to the backend, or
   hover/click land at the wrong place on HiDPI displays. */
static inline int phys_coord(WebViewInstance *inst, double v)
{
    double s = inst->scale > 0 ? inst->scale : 1.0;
    return (int)(v * s + 0.5);
}

/* The legacy libwpe button convention (1=left, 2=right, 3=middle) swaps middle
   and right relative to the modern WPE Platform API the Go side maps to
   (1=left, 2=middle, 3=right). */
static inline uint32_t legacy_button(int b)
{
    switch (b) {
    case 2:  return 3;   /* Go middle → legacy middle */
    case 3:  return 2;   /* Go right  → legacy right  */
    default: return 1;   /* left */
    }
}

typedef struct { WebViewInstance *inst; double x, y; int button; int down; } MouseBtnArgs;

static gboolean idle_mouse_btn(gpointer data)
{
    MouseBtnArgs *a = data;
    if (a->inst->backend) {
        struct wpe_input_pointer_event ev = {
            .type      = wpe_input_pointer_event_type_button,
            .time      = _ms_now(),
            .x         = phys_coord(a->inst, a->x),
            .y         = phys_coord(a->inst, a->y),
            .button    = legacy_button(a->button),
            .state     = a->down ? 1u : 0u,
            .modifiers = 0,
        };
        wpe_view_backend_dispatch_pointer_event(a->inst->backend, &ev);
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

    if (inst->backend) {
        struct wpe_input_pointer_event ev = {
            .type      = wpe_input_pointer_event_type_motion,
            .time      = _ms_now(),
            .x         = phys_coord(inst, x),
            .y         = phys_coord(inst, y),
            .button    = 0,
            .state     = 0,
            .modifiers = 0,
        };
        wpe_view_backend_dispatch_pointer_event(inst->backend, &ev);
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

    if (inst->backend) {
        /* The legacy smooth 2D axis carries pixel deltas that WebKit scrolls
           by directly (dividing by ~40px per line for tick counts), and it
           negates them, whereas the Go side pre-scales deltas by 1/20 for the
           modern API's step-based scroll.  Undo that and convert to a usable
           pixel delta; negate to match WebKit's legacy axis sign convention.
           SCROLL_GAIN is a feel constant — raise it for faster scrolling,
           flip its sign if the direction comes out inverted. */
        const double SCROLL_GAIN = -60.0;
        struct wpe_input_axis_2d_event ev = {
            .base = {
                .type      = (enum wpe_input_axis_event_type)
                             (wpe_input_axis_event_type_motion_smooth |
                              wpe_input_axis_event_type_mask_2d),
                .time      = _ms_now(),
                .x         = phys_coord(inst, x),
                .y         = phys_coord(inst, y),
                .axis      = 0,
                .value     = 0,
                .modifiers = 0,
            },
            .x_axis = dx * SCROLL_GAIN,
            .y_axis = dy * SCROLL_GAIN,
        };
        wpe_view_backend_dispatch_axis_event(inst->backend, &ev.base);
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
    if (a->inst->backend) {
        struct wpe_input_keyboard_event ev = {
            .time              = _ms_now(),
            .key_code          = a->keyval,   /* XKB/GDK keysym */
            .hardware_key_code = 0,
            .pressed           = a->down ? true : false,
            .modifiers         = 0,
        };
        wpe_view_backend_dispatch_keyboard_event(a->inst->backend, &ev);
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

/* Returns a newly-allocated copy of the current title (NULL if none); the
   caller frees it.  Reading the cached value avoids touching the GObject off
   the loop thread. */
char *WebView_CopyTitle(WebViewInstance *inst)
{
    if (!inst)
        return NULL;
    pthread_mutex_lock(&inst->metaMu);
    char *t = inst->title ? strdup(inst->title) : NULL;
    pthread_mutex_unlock(&inst->metaMu);
    return t;
}

const uint8_t *WebView_LockFavicon(WebViewInstance *inst, int *out_w, int *out_h)
{
    if (!inst)
        return NULL;
    pthread_mutex_lock(&inst->metaMu);
    if (!inst->favBuf) {
        pthread_mutex_unlock(&inst->metaMu);
        return NULL;
    }
    *out_w = inst->favW;
    *out_h = inst->favH;
    return inst->favBuf;   /* caller holds metaMu until WebView_UnlockFavicon */
}

void WebView_UnlockFavicon(WebViewInstance *inst)
{
    if (inst)
        pthread_mutex_unlock(&inst->metaMu);
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
