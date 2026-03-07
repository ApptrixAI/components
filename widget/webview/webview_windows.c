/*
 * webview_windows.c — WebView2 (Microsoft Edge) embedded in a parent HWND.
 *
 * WebView2Loader.dll is loaded at runtime so the app can gracefully handle
 * the DLL not being present.  The WebView2 COM interfaces are called through
 * manually-defined vtable slot indices to avoid a build-time dependency on
 * the WebView2 SDK headers.
 */

#include "webview_windows.h"

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── COM vtable slot indices ─────────────────────────────────────────── */

/* ICoreWebView2Environment (IUnknown + 3 methods) */
#define ENV_CREATE_CONTROLLER 3

/* ICoreWebView2Controller (IUnknown + 23 methods) */
#define CTL_PUT_BOUNDS        6
#define CTL_MOVE_FOCUS        12
#define CTL_GET_COREWEBVIEW2  25

/* ICoreWebView2 (IUnknown + 57 methods) */
#define WV_GET_SOURCE         4
#define WV_NAVIGATE           5
#define WV_ADD_NAV_STARTING   7
#define WV_ADD_NAV_COMPLETED  15
#define WV_RELOAD             31
#define WV_GOBACK             40
#define WV_GOFORWARD          41
#define WV_STOP               43


/* ── Vtable accessor ─────────────────────────────────────────────────── */

#define VTBL(obj) (*(void ***)(obj))

/* ── GUIDs ───────────────────────────────────────────────────────────── */

static const GUID IID_IUnknown_local =
    {0x00000000,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

/* ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler */
static const GUID IID_EnvHandler =
    {0x4e8a3389,0xc9d8,0x4bd2,{0xb6,0xb5,0x12,0x4f,0xee,0x6c,0xc1,0x4d}};

/* ICoreWebView2CreateCoreWebView2ControllerCompletedHandler */
static const GUID IID_CtlHandler =
    {0x6c4819f3,0xc9b7,0x4260,{0x81,0x27,0xc9,0xf5,0xbd,0xe7,0xf6,0x8c}};

/* ICoreWebView2NavigationStartingEventHandler */
static const GUID IID_NavStartHandler =
    {0x9adbe429,0xf36d,0x432b,{0x9d,0xdc,0xf8,0x88,0x1f,0xbd,0x76,0xe3}};

/* ICoreWebView2NavigationCompletedEventHandler */
static const GUID IID_NavCompHandler =
    {0xd33a35bf,0x1c49,0x4f98,{0x93,0xab,0x00,0x6e,0x05,0x33,0xfe,0x1c}};

/* ── Global state ────────────────────────────────────────────────────── */

static HWND  _parentHwnd  = NULL;
static void *_environment = NULL;  /* ICoreWebView2Environment* */
static void *_controller  = NULL;  /* ICoreWebView2Controller*  */
static void *_webView     = NULL;  /* ICoreWebView2*            */
static int   _ready       = 0;
static int   _isLoading   = 0;
static char  _urlBuf[4096];

/* ── Parent HWND subclass for focus management ───────────────────────── */

/*
 * WebView2 creates child HWNDs that capture Win32 keyboard focus.
 * GLFW doesn't call SetFocus on mouse clicks (it assumes it owns the
 * whole window), so clicking on the Fyne area never reclaims focus.
 * We subclass the parent HWND to return focus on any mouse click in
 * the non-WebView2 area.
 */
static WNDPROC _origWndProc = NULL;

static LRESULT CALLBACK webviewSubclassProc(
        HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        SetFocus(hwnd);
        break;
    }
    return CallWindowProcW(_origWndProc, hwnd, msg, wParam, lParam);
}

/* ── UTF-8 ↔ UTF-16 helpers ─────────────────────────────────────────── */

static wchar_t *utf8_to_wide(const char *s) {
    if (!s) return NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (len <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc(len * sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, len);
    return w;
}

static char *wide_to_utf8(const wchar_t *w) {
    if (!w) return NULL;
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return NULL;
    char *s = (char *)malloc(len);
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, len, NULL, NULL);
    return s;
}

/* ── Generic COM callback handler ────────────────────────────────────── */

typedef struct {
    void       **lpVtbl;
    LONG         ref;
    const GUID  *iid;
} Handler;

static HRESULT STDMETHODCALLTYPE Handler_QueryInterface(
        Handler *this, const GUID *riid, void **ppv) {
    if (IsEqualGUID(riid, &IID_IUnknown_local) ||
        IsEqualGUID(riid, this->iid)) {
        *ppv = this;
        InterlockedIncrement(&this->ref);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Handler_AddRef(Handler *this) {
    return InterlockedIncrement(&this->ref);
}

static ULONG STDMETHODCALLTYPE Handler_Release(Handler *this) {
    return InterlockedDecrement(&this->ref);
}

/* ── URL cache helper ────────────────────────────────────────────────── */

/* ICoreWebView2NavigationStartingEventArgs::get_Uri — slot 3 */
#define NAV_ARGS_GET_URI 3

static void cache_url_from_source(void *webview) {
    if (!webview) return;
    wchar_t *wurl = NULL;
    HRESULT hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, wchar_t **))(
        VTBL(webview)[WV_GET_SOURCE]))(webview, &wurl);
    if (FAILED(hr) || !wurl) return;
    char *utf8 = wide_to_utf8(wurl);
    CoTaskMemFree(wurl);
    if (utf8) {
        strncpy(_urlBuf, utf8, sizeof(_urlBuf) - 1);
        _urlBuf[sizeof(_urlBuf) - 1] = '\0';
        free(utf8);
    }
}

/* ── NavigationStarting handler ──────────────────────────────────────── */

static HRESULT STDMETHODCALLTYPE NavStart_Invoke(
        Handler *this, void *sender, void *args) {
    (void)this; (void)sender;
    _isLoading = 1;

    /* Read the target URI from the event args (get_Source on sender
       still returns the old URL at this point). */
    if (args) {
        wchar_t *wurl = NULL;
        HRESULT hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, wchar_t **))(
            VTBL(args)[NAV_ARGS_GET_URI]))(args, &wurl);
        if (SUCCEEDED(hr) && wurl) {
            char *utf8 = wide_to_utf8(wurl);
            CoTaskMemFree(wurl);
            if (utf8) {
                strncpy(_urlBuf, utf8, sizeof(_urlBuf) - 1);
                _urlBuf[sizeof(_urlBuf) - 1] = '\0';
                free(utf8);
            }
        }
    }
    return S_OK;
}

static void *navStartVtbl[] = {
    Handler_QueryInterface, Handler_AddRef, Handler_Release, NavStart_Invoke
};
static Handler navStartHandler = { navStartVtbl, 1, &IID_NavStartHandler };

/* ── NavigationCompleted handler ─────────────────────────────────────── */

static HRESULT STDMETHODCALLTYPE NavComp_Invoke(
        Handler *this, void *sender, void *args) {
    (void)this; (void)args;
    _isLoading = 0;
    /* Update cached URL to the final URL (handles redirects). */
    cache_url_from_source(sender);
    return S_OK;
}

static void *navCompVtbl[] = {
    Handler_QueryInterface, Handler_AddRef, Handler_Release, NavComp_Invoke
};
static Handler navCompHandler = { navCompVtbl, 1, &IID_NavCompHandler };

/* ── ControllerCompleted handler ─────────────────────────────────────── */

static HRESULT STDMETHODCALLTYPE CtlCompleted_Invoke(
        Handler *this, HRESULT result, void *controller) {
    (void)this;
    if (FAILED(result) || !controller) {
        fprintf(stderr, "WebView2: controller creation failed (0x%08lx)\n",
                result);
        _ready = 1;
        return S_OK;
    }

    _controller = controller;
    /* AddRef */
    ((ULONG (STDMETHODCALLTYPE *)(void *))(VTBL(controller)[1]))(controller);

    /* get_CoreWebView2 */
    HRESULT hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, void **))(
        VTBL(controller)[CTL_GET_COREWEBVIEW2]))(controller, &_webView);
    if (FAILED(hr) || !_webView) {
        fprintf(stderr, "WebView2: get_CoreWebView2 failed (0x%08lx)\n", hr);
        _ready = 1;
        return S_OK;
    }

    /* Register navigation event handlers for IsLoading tracking. */
    long long token;
    ((HRESULT (STDMETHODCALLTYPE *)(void *, void *, long long *))(
        VTBL(_webView)[WV_ADD_NAV_STARTING]))(_webView, &navStartHandler,
                                               &token);
    ((HRESULT (STDMETHODCALLTYPE *)(void *, void *, long long *))(
        VTBL(_webView)[WV_ADD_NAV_COMPLETED]))(_webView, &navCompHandler,
                                                &token);

    _ready = 1;
    return S_OK;
}

static void *ctlCompletedVtbl[] = {
    Handler_QueryInterface, Handler_AddRef, Handler_Release,
    CtlCompleted_Invoke
};
static Handler ctlCompletedHandler = {
    ctlCompletedVtbl, 1, &IID_CtlHandler
};

/* ── EnvironmentCompleted handler ────────────────────────────────────── */

static HRESULT STDMETHODCALLTYPE EnvCompleted_Invoke(
        Handler *this, HRESULT result, void *env) {
    (void)this;
    if (FAILED(result) || !env) {
        fprintf(stderr, "WebView2: environment creation failed (0x%08lx)\n",
                result);
        _ready = 1;
        return S_OK;
    }

    _environment = env;
    /* AddRef */
    ((ULONG (STDMETHODCALLTYPE *)(void *))(VTBL(env)[1]))(env);

    /* CreateCoreWebView2Controller(HWND, handler) */
    HRESULT hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, HWND, void *))(
        VTBL(env)[ENV_CREATE_CONTROLLER]))(env, _parentHwnd,
                                            &ctlCompletedHandler);
    if (FAILED(hr)) {
        fprintf(stderr, "WebView2: CreateController call failed (0x%08lx)\n",
                hr);
        _ready = 1;
    }

    return S_OK;
}

static void *envCompletedVtbl[] = {
    Handler_QueryInterface, Handler_AddRef, Handler_Release,
    EnvCompleted_Invoke
};
static Handler envCompletedHandler = {
    envCompletedVtbl, 1, &IID_EnvHandler
};

/* ── WebView2 creation function types ────────────────────────────────── */

/* Exported by WebView2Loader.dll (public SDK API). */
typedef HRESULT (STDMETHODCALLTYPE *CreateWebView2EnvironmentFunc)(
    LPCWSTR, LPCWSTR, void *, void *);

/* Exported by EmbeddedBrowserWebView.dll (runtime-internal API).
   Parameters (from disassembly of the x64 export):
     1. BOOL  — version-match flag          (use FALSE)
     2. int   — runtime type enum           (use 0 = auto)
     3. PCWSTR — userDataFolder             (NULL for default)
     4. void* — ICoreWebView2EnvironmentOptions*  (NULL)
     5. void* — completion handler           (required, non-NULL) */
typedef HRESULT (STDMETHODCALLTYPE *CreateWebView2EnvironmentInternalFunc)(
    BOOL, int, LPCWSTR, void *, void *);

/*
 * find_webview2_module tries to load a DLL that exports
 * CreateCoreWebView2EnvironmentWithOptions:
 *
 *  1. WebView2Loader.dll (app directory or PATH)
 *  2. EmbeddedBrowserWebView.dll from the WebView2 Runtime (registry)
 *  3. EmbeddedBrowserWebView.dll from the Edge browser (registry)
 */
static HMODULE find_webview2_module(void) {
    HMODULE h;

    /* 1. Try WebView2Loader.dll directly. */
    h = LoadLibraryW(L"WebView2Loader.dll");
    if (h) return h;

    /* 2–3. Look up WebView2 Runtime / Edge Stable in the registry.
       The entries live under WOW6432Node on 64-bit Windows, so we
       try both the native and 32-bit registry views. */
    static const wchar_t *subkeys[] = {
        L"SOFTWARE\\Microsoft\\EdgeUpdate\\ClientState\\"
            L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",  /* WebView2 Runtime */
        L"SOFTWARE\\Microsoft\\EdgeUpdate\\ClientState\\"
            L"{56EB18F8-B008-4CBD-B6D2-8C97FE7E9062}",  /* Edge Stable */
        NULL
    };
    static const HKEY roots[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    static const REGSAM views[] = { KEY_WOW64_32KEY, KEY_WOW64_64KEY, 0 };

#ifdef _WIN64
    static const wchar_t *arch = L"x64";
#else
    static const wchar_t *arch = L"x86";
#endif

    for (int r = 0; r < 2; r++) {
        for (int s = 0; subkeys[s]; s++) {
            for (int v = 0; v < 3; v++) {
                HKEY key;
                if (RegOpenKeyExW(roots[r], subkeys[s], 0,
                                  KEY_READ | views[v],
                                  &key) != ERROR_SUCCESS)
                    continue;

                wchar_t val[MAX_PATH];
                DWORD size, type;
                wchar_t dll[MAX_PATH * 2];

                /* Try the "EBWebView" value (full install path). */
                size = sizeof(val);
                if (RegQueryValueExW(key, L"EBWebView", NULL, &type,
                                     (BYTE *)val, &size) == ERROR_SUCCESS
                    && type == REG_SZ && val[0]) {
                    _snwprintf(dll, MAX_PATH * 2,
                        L"%s\\EBWebView\\%s\\EmbeddedBrowserWebView.dll",
                        val, arch);
                    h = LoadLibraryW(dll);
                    if (h) { RegCloseKey(key); return h; }
                }

                /* Try the "pv" value (version) and probe known paths. */
                size = sizeof(val);
                if (RegQueryValueExW(key, L"pv", NULL, &type,
                                     (BYTE *)val, &size) == ERROR_SUCCESS
                    && type == REG_SZ && val[0]) {
                    static const wchar_t *products[] = {
                        L"EdgeWebView", L"Edge", NULL
                    };
                    static const wchar_t *envvars[] = {
                        L"ProgramFiles(x86)", L"ProgramFiles",
                        L"LOCALAPPDATA", NULL
                    };
                    for (int p = 0; products[p]; p++) {
                        for (int e = 0; envvars[e]; e++) {
                            wchar_t base[MAX_PATH];
                            if (!GetEnvironmentVariableW(envvars[e],
                                                         base, MAX_PATH))
                                continue;
                            _snwprintf(dll, MAX_PATH * 2,
                                L"%s\\Microsoft\\%s\\Application\\%s\\"
                                L"EBWebView\\%s\\"
                                L"EmbeddedBrowserWebView.dll",
                                base, products[p], val, arch);
                            h = LoadLibraryW(dll);
                            if (h) { RegCloseKey(key); return h; }
                        }
                    }
                }

                RegCloseKey(key);
            }
        }
    }

    return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void WebView_Create(void *hwnd) {
    if (!hwnd) return;
    _parentHwnd = (HWND)hwnd;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    HMODULE loader = find_webview2_module();
    if (!loader) {
        fprintf(stderr,
                "WebView2: could not find WebView2Loader.dll or "
                "EmbeddedBrowserWebView.dll\n");
        return;
    }

    HRESULT hr;

    /* Try the public API first (WebView2Loader.dll). */
    CreateWebView2EnvironmentFunc createEnv =
        (CreateWebView2EnvironmentFunc)GetProcAddress(
            loader, "CreateCoreWebView2EnvironmentWithOptions");
    if (createEnv) {
        hr = createEnv(NULL, NULL, NULL, &envCompletedHandler);
    } else {
        /* Fall back to the runtime-internal export
           (EmbeddedBrowserWebView.dll). */
        CreateWebView2EnvironmentInternalFunc createInternal =
            (CreateWebView2EnvironmentInternalFunc)GetProcAddress(
                loader, "CreateWebViewEnvironmentWithOptionsInternal");
        if (!createInternal) {
            fprintf(stderr,
                    "WebView2: no usable creation function found\n");
            FreeLibrary(loader);
            return;
        }
        hr = createInternal(FALSE, 0, NULL, NULL, &envCompletedHandler);
    }
    if (FAILED(hr)) {
        fprintf(stderr,
                "WebView2: CreateCoreWebView2EnvironmentWithOptions "
                "failed (0x%08lx)\n", hr);
        return;
    }

    /* Pump the message loop until the async creation sequence completes.
       A timeout prevents hanging if something goes wrong. */
    DWORD start = GetTickCount();
    MSG msg;
    while (!_ready) {
        if (GetTickCount() - start > 10000) {
            fprintf(stderr, "WebView2: creation timed out\n");
            break;
        }
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            Sleep(1);
        }
    }

    /* Subclass the parent HWND so clicks on the Fyne area reclaim
       keyboard focus from the WebView2 child window. */
    if (_ready && !_origWndProc) {
        _origWndProc = (WNDPROC)SetWindowLongPtrW(
            _parentHwnd, GWLP_WNDPROC, (LONG_PTR)webviewSubclassProc);
    }

    /* Undo the initial focus steal — return focus to GLFW. */
    SetFocus(_parentHwnd);
}

void WebView_SetFrame(double x, double y, double width, double height) {
    if (!_controller) return;

    RECT bounds;
    bounds.left   = (LONG)x;
    bounds.top    = (LONG)y;
    bounds.right  = (LONG)(x + width);
    bounds.bottom = (LONG)(y + height);

    ((HRESULT (STDMETHODCALLTYPE *)(void *, RECT))(
        VTBL(_controller)[CTL_PUT_BOUNDS]))(_controller, bounds);
}

void WebView_SetDarkMode(int dark) {
    /* WebView2 follows the OS preferred color scheme by default. */
    (void)dark;
}

void WebView_Navigate(const char *url) {
    if (!url || !_webView) return;

    const char *u = url;
    char buf[4096];
    if (strncmp(url, "http://", 7) != 0 &&
        strncmp(url, "https://", 8) != 0) {
        snprintf(buf, sizeof(buf), "https://%s", url);
        u = buf;
    }

    wchar_t *wurl = utf8_to_wide(u);
    if (!wurl) return;

    ((HRESULT (STDMETHODCALLTYPE *)(void *, LPCWSTR))(
        VTBL(_webView)[WV_NAVIGATE]))(_webView, wurl);
    free(wurl);
}

void WebView_GoBack(void) {
    if (_webView)
        ((HRESULT (STDMETHODCALLTYPE *)(void *))(
            VTBL(_webView)[WV_GOBACK]))(_webView);
}

void WebView_GoForward(void) {
    if (_webView)
        ((HRESULT (STDMETHODCALLTYPE *)(void *))(
            VTBL(_webView)[WV_GOFORWARD]))(_webView);
}

void WebView_Reload(void) {
    if (_webView)
        ((HRESULT (STDMETHODCALLTYPE *)(void *))(
            VTBL(_webView)[WV_RELOAD]))(_webView);
}

void WebView_Stop(void) {
    if (_webView)
        ((HRESULT (STDMETHODCALLTYPE *)(void *))(
            VTBL(_webView)[WV_STOP]))(_webView);
}

int WebView_IsLoading(void) {
    return _isLoading;
}

const char *WebView_GetURL(void) {
    return _urlBuf;
}

void WebView_Focus(void) {
    if (!_controller) return;
    /* COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC = 0 */
    ((HRESULT (STDMETHODCALLTYPE *)(void *, int))(
        VTBL(_controller)[CTL_MOVE_FOCUS]))(_controller, 0);
}

void WebView_Unfocus(void) {
    if (!_parentHwnd) return;
    /* Return keyboard focus to the parent (Fyne/GLFW) window. */
    SetFocus(_parentHwnd);
}
