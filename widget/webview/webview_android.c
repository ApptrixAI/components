#include "webview_android.h"
#include <android/log.h>
#include <pthread.h>
#include <string.h>

#define TAG "WebViewNative"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static JavaVM   *_jvm;
static jobject   _activity;   /* global ref */
static jobject   _webView;    /* global ref */
static jobject   _windowMgr;  /* global ref */
static jclass    _helperCls;  /* global ref to embedded Runnable class */
static int       _statusBarH; /* status bar height in pixels */
static char      _currentURL[4096];
static int       _loading;

/* ── Async command queue ──────────────────────────────────────────── */

enum {
    CMD_CREATE = 0,
    CMD_SET_FRAME,
    CMD_SET_DARK,
    CMD_NAVIGATE,
    CMD_GO_BACK,
    CMD_GO_FORWARD,
    CMD_RELOAD,
    CMD_STOP,
};

typedef struct {
    int type;
    double x, y, w, h;
    int intVal;
    char url[4096];
} QEntry;

#define Q_CAP 64
static QEntry _q[Q_CAP];
static int _qHead, _qTail;
static pthread_mutex_t _qMu = PTHREAD_MUTEX_INITIALIZER;
static int _uiPosted;

/*
 * Embedded DEX bytecode for a minimal Runnable class:
 *
 *   public class R implements java.lang.Runnable {
 *       public static native void n();
 *       public R() { super(); }
 *       public void run() { R.n(); }
 *   }
 */
static const unsigned char _dex[] = {
    0x64,0x65,0x78,0x0A,0x30,0x33,0x35,0x00,0xB7,0x34,0x01,0x8C,0xE8,0xE9,0x96,0xD2,
    0x03,0xD8,0xFB,0xBC,0x68,0x93,0x0B,0x60,0x65,0x6E,0x1B,0x55,0x23,0xE1,0x6E,0x3F,
    0x08,0x02,0x00,0x00,0x70,0x00,0x00,0x00,0x78,0x56,0x34,0x12,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x80,0x01,0x00,0x00,0x08,0x00,0x00,0x00,0x70,0x00,0x00,0x00,
    0x04,0x00,0x00,0x00,0x90,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0xA0,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0xAC,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0xCC,0x00,0x00,0x00,0x1C,0x01,0x00,0x00,0xEC,0x00,0x00,0x00,
    0x36,0x01,0x00,0x00,0x3E,0x01,0x00,0x00,0x43,0x01,0x00,0x00,0x57,0x01,0x00,0x00,
    0x6D,0x01,0x00,0x00,0x75,0x01,0x00,0x00,0x78,0x01,0x00,0x00,0x7B,0x01,0x00,0x00,
    0x01,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x05,0x00,0x00,0x00,
    0x05,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x07,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0xEC,0x00,0x00,0x00,0x04,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x24,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x02,0x00,0x00,0x00,0x01,0x00,0x01,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x04,0x00,0x00,0x00,0x70,0x10,0x03,0x00,0x00,0x00,0x0E,0x00,0x01,0x00,0x01,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x71,0x00,0x01,0x00,
    0x00,0x00,0x0E,0x00,0x00,0x00,0x02,0x01,0x00,0x81,0x80,0x04,0xF4,0x01,0x01,0x89,
    0x02,0x00,0x02,0x01,0x8C,0x02,0x06,0x3C,0x69,0x6E,0x69,0x74,0x3E,0x00,0x03,0x4C,
    0x52,0x3B,0x00,0x12,0x4C,0x6A,0x61,0x76,0x61,0x2F,0x6C,0x61,0x6E,0x67,0x2F,0x4F,
    0x62,0x6A,0x65,0x63,0x74,0x3B,0x00,0x14,0x4C,0x6A,0x61,0x76,0x61,0x2F,0x6C,0x61,
    0x6E,0x67,0x2F,0x52,0x75,0x6E,0x6E,0x61,0x62,0x6C,0x65,0x3B,0x00,0x06,0x52,0x2E,
    0x6A,0x61,0x76,0x61,0x00,0x01,0x56,0x00,0x01,0x6E,0x00,0x03,0x72,0x75,0x6E,0x00,
    0x0B,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x70,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0x04,0x00,0x00,0x00,0x90,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0xA0,0x00,0x00,0x00,0x05,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0xAC,0x00,0x00,0x00,
    0x06,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0xCC,0x00,0x00,0x00,0x01,0x10,0x00,0x00,
    0x01,0x00,0x00,0x00,0xEC,0x00,0x00,0x00,0x01,0x20,0x00,0x00,0x02,0x00,0x00,0x00,
    0xF4,0x00,0x00,0x00,0x00,0x20,0x00,0x00,0x01,0x00,0x00,0x00,0x24,0x01,0x00,0x00,
    0x02,0x20,0x00,0x00,0x08,0x00,0x00,0x00,0x36,0x01,0x00,0x00,0x00,0x10,0x00,0x00,
    0x01,0x00,0x00,0x00,0x80,0x01,0x00,0x00,
};
#define DEX_SIZE 520

/* ── JNI helpers ──────────────────────────────────────────────────── */

static JNIEnv *getEnv(void) {
    if (!_jvm) return NULL;
    JNIEnv *env = NULL;
    if ((*_jvm)->GetEnv(_jvm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED)
        (*_jvm)->AttachCurrentThread(_jvm, &env, NULL);
    return env;
}

static int checkException(JNIEnv *env) {
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        return 1;
    }
    return 0;
}

static void postToUI(JNIEnv *env) {
    jmethodID init = (*env)->GetMethodID(env, _helperCls, "<init>", "()V");
    jobject r = (*env)->NewObject(env, _helperCls, init);
    if (checkException(env) || !r) return;

    jclass cls = (*env)->GetObjectClass(env, _activity);
    jmethodID run = (*env)->GetMethodID(env, cls, "runOnUiThread",
                                         "(Ljava/lang/Runnable;)V");
    (*env)->CallVoidMethod(env, _activity, run, r);
    checkException(env);
    (*env)->DeleteLocalRef(env, r);
    (*env)->DeleteLocalRef(env, cls);
}

static void enqueue(QEntry *e) {
    JNIEnv *env = getEnv();
    if (!env) return;

    pthread_mutex_lock(&_qMu);
    int next = (_qTail + 1) % Q_CAP;
    if (next == _qHead)
        _qHead = (_qHead + 1) % Q_CAP; /* drop oldest */
    _q[_qTail] = *e;
    _qTail = next;
    if (!_uiPosted) {
        postToUI(env);
        _uiPosted = 1;
    }
    pthread_mutex_unlock(&_qMu);
}

/* ── UI thread command handlers ───────────────────────────────────── */

static void uiCreateWebView(JNIEnv *env) {
    if (_webView) return;

    jclass actCls = (*env)->GetObjectClass(env, _activity);

    /* Get window token for TYPE_APPLICATION_PANEL */
    jmethodID getWindow = (*env)->GetMethodID(env, actCls, "getWindow",
                                              "()Landroid/view/Window;");
    jobject window = (*env)->CallObjectMethod(env, _activity, getWindow);
    if (checkException(env) || !window) { (*env)->DeleteLocalRef(env, actCls); return; }

    jclass winCls = (*env)->GetObjectClass(env, window);
    jmethodID getDecor = (*env)->GetMethodID(env, winCls, "getDecorView",
                                             "()Landroid/view/View;");
    jobject decor = (*env)->CallObjectMethod(env, window, getDecor);
    (*env)->DeleteLocalRef(env, winCls);
    (*env)->DeleteLocalRef(env, window);
    if (checkException(env) || !decor) { (*env)->DeleteLocalRef(env, actCls); return; }

    jclass viewCls = (*env)->GetObjectClass(env, decor);
    jmethodID getToken = (*env)->GetMethodID(env, viewCls, "getWindowToken",
                                              "()Landroid/os/IBinder;");
    jobject token = (*env)->CallObjectMethod(env, decor, getToken);
    (*env)->DeleteLocalRef(env, viewCls);
    (*env)->DeleteLocalRef(env, decor);
    if (checkException(env) || !token) { (*env)->DeleteLocalRef(env, actCls); return; }

    /* WindowManager */
    jclass ctxCls = (*env)->FindClass(env, "android/content/Context");
    jfieldID wsSvcField = (*env)->GetStaticFieldID(env, ctxCls, "WINDOW_SERVICE",
                                                    "Ljava/lang/String;");
    jstring wsSvc = (jstring)(*env)->GetStaticObjectField(env, ctxCls, wsSvcField);
    jmethodID getSvc = (*env)->GetMethodID(env, actCls, "getSystemService",
                                            "(Ljava/lang/String;)Ljava/lang/Object;");
    jobject wm = (*env)->CallObjectMethod(env, _activity, getSvc, wsSvc);
    (*env)->DeleteLocalRef(env, wsSvc);
    (*env)->DeleteLocalRef(env, ctxCls);
    if (checkException(env) || !wm) {
        (*env)->DeleteLocalRef(env, token);
        (*env)->DeleteLocalRef(env, actCls);
        return;
    }
    _windowMgr = (*env)->NewGlobalRef(env, wm);
    (*env)->DeleteLocalRef(env, wm);

    /* Status bar height for coordinate adjustment */
    jmethodID getRes = (*env)->GetMethodID(env, actCls, "getResources",
                                            "()Landroid/content/res/Resources;");
    jobject res = (*env)->CallObjectMethod(env, _activity, getRes);
    if (!checkException(env) && res) {
        jclass resCls = (*env)->GetObjectClass(env, res);
        jmethodID getIdent = (*env)->GetMethodID(env, resCls, "getIdentifier",
                                                  "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I");
        jstring sbName = (*env)->NewStringUTF(env, "status_bar_height");
        jstring sbType = (*env)->NewStringUTF(env, "dimen");
        jstring sbPkg  = (*env)->NewStringUTF(env, "android");
        jint resId = (*env)->CallIntMethod(env, res, getIdent, sbName, sbType, sbPkg);
        if (resId > 0) {
            jmethodID getDim = (*env)->GetMethodID(env, resCls, "getDimensionPixelSize", "(I)I");
            _statusBarH = (*env)->CallIntMethod(env, res, getDim, resId);
        }
        (*env)->DeleteLocalRef(env, sbPkg);
        (*env)->DeleteLocalRef(env, sbType);
        (*env)->DeleteLocalRef(env, sbName);
        (*env)->DeleteLocalRef(env, resCls);
        (*env)->DeleteLocalRef(env, res);
    }

    /* Create WebView */
    jclass wvCls = (*env)->FindClass(env, "android/webkit/WebView");
    if (checkException(env) || !wvCls) {
        (*env)->DeleteLocalRef(env, token);
        (*env)->DeleteLocalRef(env, actCls);
        return;
    }

    jmethodID wvInit = (*env)->GetMethodID(env, wvCls, "<init>",
                                           "(Landroid/content/Context;)V");
    jobject wv = (*env)->NewObject(env, wvCls, wvInit, _activity);
    if (checkException(env) || !wv) {
        (*env)->DeleteLocalRef(env, wvCls);
        (*env)->DeleteLocalRef(env, token);
        (*env)->DeleteLocalRef(env, actCls);
        return;
    }
    _webView = (*env)->NewGlobalRef(env, wv);
    (*env)->DeleteLocalRef(env, wv);

    /* WebViewClient keeps navigation inside the WebView */
    jclass wvcCls = (*env)->FindClass(env, "android/webkit/WebViewClient");
    if (!checkException(env) && wvcCls) {
        jmethodID wvcInit = (*env)->GetMethodID(env, wvcCls, "<init>", "()V");
        jobject wvc = (*env)->NewObject(env, wvcCls, wvcInit);
        if (!checkException(env) && wvc) {
            jmethodID setClient = (*env)->GetMethodID(env, wvCls, "setWebViewClient",
                                                       "(Landroid/webkit/WebViewClient;)V");
            (*env)->CallVoidMethod(env, _webView, setClient, wvc);
            checkException(env);
            (*env)->DeleteLocalRef(env, wvc);
        }
        (*env)->DeleteLocalRef(env, wvcCls);
    }

    /* Enable JavaScript and DOM storage */
    jmethodID getSettings = (*env)->GetMethodID(env, wvCls, "getSettings",
                                                "()Landroid/webkit/WebSettings;");
    if (!checkException(env) && getSettings) {
        jobject settings = (*env)->CallObjectMethod(env, _webView, getSettings);
        if (!checkException(env) && settings) {
            jclass sCls = (*env)->GetObjectClass(env, settings);
            jmethodID setJS = (*env)->GetMethodID(env, sCls, "setJavaScriptEnabled", "(Z)V");
            if (setJS) (*env)->CallVoidMethod(env, settings, setJS, JNI_TRUE);
            jmethodID setDom = (*env)->GetMethodID(env, sCls, "setDomStorageEnabled", "(Z)V");
            if (setDom) (*env)->CallVoidMethod(env, settings, setDom, JNI_TRUE);
            checkException(env);
            (*env)->DeleteLocalRef(env, sCls);
            (*env)->DeleteLocalRef(env, settings);
        }
    }

    /* Add via WindowManager as a separate window layer above the GL surface.
     * TYPE_APPLICATION_PANEL = 1000, sub-window attached to the activity.
     * FLAGS: NOT_FOCUSABLE(0x8) | NOT_TOUCH_MODAL(0x20) | HW_ACCELERATED(0x01000000)
     * PixelFormat.TRANSLUCENT = -3 */
    jclass lpCls = (*env)->FindClass(env, "android/view/WindowManager$LayoutParams");
    jmethodID lpInit = (*env)->GetMethodID(env, lpCls, "<init>", "(IIIII)V");
    jobject lp = (*env)->NewObject(env, lpCls, lpInit,
                                   0, 0,
                                   (jint)1000,
                                   (jint)(0x8 | 0x20 | 0x01000000),
                                   (jint)-3);
    if (checkException(env) || !lp) {
        (*env)->DeleteGlobalRef(env, _webView); _webView = NULL;
        (*env)->DeleteLocalRef(env, lpCls);
        (*env)->DeleteLocalRef(env, wvCls);
        (*env)->DeleteLocalRef(env, token);
        (*env)->DeleteLocalRef(env, actCls);
        return;
    }

    jfieldID tokenField = (*env)->GetFieldID(env, lpCls, "token", "Landroid/os/IBinder;");
    (*env)->SetObjectField(env, lp, tokenField, token);

    jfieldID gravField = (*env)->GetFieldID(env, lpCls, "gravity", "I");
    (*env)->SetIntField(env, lp, gravField, 0x33); /* TOP | LEFT */

    jclass wmCls = (*env)->FindClass(env, "android/view/ViewManager");
    jmethodID addView = (*env)->GetMethodID(env, wmCls, "addView",
                                            "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V");
    (*env)->CallVoidMethod(env, _windowMgr, addView, _webView, lp);
    if (checkException(env)) {
        (*env)->DeleteGlobalRef(env, _webView); _webView = NULL;
        (*env)->DeleteGlobalRef(env, _windowMgr); _windowMgr = NULL;
    }

    (*env)->DeleteLocalRef(env, wmCls);
    (*env)->DeleteLocalRef(env, lp);
    (*env)->DeleteLocalRef(env, lpCls);
    (*env)->DeleteLocalRef(env, wvCls);
    (*env)->DeleteLocalRef(env, token);
    (*env)->DeleteLocalRef(env, actCls);
}

static void uiSetFrame(JNIEnv *env, double x, double y, double w, double h) {
    if (!_webView || !_windowMgr) return;

    jclass viewCls = (*env)->GetObjectClass(env, _webView);
    jmethodID getLP = (*env)->GetMethodID(env, viewCls, "getLayoutParams",
                                          "()Landroid/view/ViewGroup$LayoutParams;");
    jobject lp = (*env)->CallObjectMethod(env, _webView, getLP);
    if (checkException(env) || !lp) { (*env)->DeleteLocalRef(env, viewCls); return; }

    jclass lpCls = (*env)->FindClass(env, "android/view/WindowManager$LayoutParams");
    (*env)->SetIntField(env, lp, (*env)->GetFieldID(env, lpCls, "width", "I"), (jint)w);
    (*env)->SetIntField(env, lp, (*env)->GetFieldID(env, lpCls, "height", "I"), (jint)h);
    (*env)->SetIntField(env, lp, (*env)->GetFieldID(env, lpCls, "x", "I"), (jint)x);
    (*env)->SetIntField(env, lp, (*env)->GetFieldID(env, lpCls, "y", "I"), (jint)(y + _statusBarH));

    jclass wmCls = (*env)->FindClass(env, "android/view/ViewManager");
    jmethodID updateLP = (*env)->GetMethodID(env, wmCls, "updateViewLayout",
                                              "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V");
    (*env)->CallVoidMethod(env, _windowMgr, updateLP, _webView, lp);
    checkException(env);

    (*env)->DeleteLocalRef(env, wmCls);
    (*env)->DeleteLocalRef(env, lp);
    (*env)->DeleteLocalRef(env, lpCls);
    (*env)->DeleteLocalRef(env, viewCls);
}

static void uiSetDarkMode(JNIEnv *env, int dark) {
    if (!_webView) return;
    jclass cls = (*env)->GetObjectClass(env, _webView);
    jmethodID setBg = (*env)->GetMethodID(env, cls, "setBackgroundColor", "(I)V");
    (*env)->CallVoidMethod(env, _webView, setBg, dark ? (jint)0xFF121212 : (jint)0xFFFFFFFF);
    checkException(env);
    (*env)->DeleteLocalRef(env, cls);
}

static void uiNavigate(JNIEnv *env, const char *url) {
    if (!_webView) return;
    jstring jurl = (*env)->NewStringUTF(env, url);
    jclass cls = (*env)->GetObjectClass(env, _webView);
    jmethodID loadUrl = (*env)->GetMethodID(env, cls, "loadUrl", "(Ljava/lang/String;)V");
    (*env)->CallVoidMethod(env, _webView, loadUrl, jurl);
    checkException(env);
    (*env)->DeleteLocalRef(env, jurl);
    (*env)->DeleteLocalRef(env, cls);
}

static void uiSimpleCall(JNIEnv *env, const char *method) {
    if (!_webView) return;
    jclass cls = (*env)->GetObjectClass(env, _webView);
    jmethodID mid = (*env)->GetMethodID(env, cls, method, "()V");
    (*env)->CallVoidMethod(env, _webView, mid);
    checkException(env);
    (*env)->DeleteLocalRef(env, cls);
}

static void uiRefreshState(JNIEnv *env) {
    if (!_webView) return;

    jclass cls = (*env)->GetObjectClass(env, _webView);

    jmethodID getProgress = (*env)->GetMethodID(env, cls, "getProgress", "()I");
    jint progress = (*env)->CallIntMethod(env, _webView, getProgress);
    if (!checkException(env)) _loading = progress < 100;

    jmethodID getUrl = (*env)->GetMethodID(env, cls, "getUrl", "()Ljava/lang/String;");
    jstring jurl = (jstring)(*env)->CallObjectMethod(env, _webView, getUrl);
    if (!checkException(env) && jurl) {
        const char *utf = (*env)->GetStringUTFChars(env, jurl, NULL);
        strncpy(_currentURL, utf, sizeof(_currentURL) - 1);
        _currentURL[sizeof(_currentURL) - 1] = '\0';
        (*env)->ReleaseStringUTFChars(env, jurl, utf);
        (*env)->DeleteLocalRef(env, jurl);
    }

    (*env)->DeleteLocalRef(env, cls);
}

/* ── nativeRunImpl — called on UI thread by R.run() ──────────────── */

static void JNICALL nativeRunImpl(JNIEnv *env, jclass cls) {
    for (;;) {
        pthread_mutex_lock(&_qMu);
        if (_qHead == _qTail) {
            _uiPosted = 0;
            pthread_mutex_unlock(&_qMu);
            break;
        }
        QEntry e = _q[_qHead];
        _qHead = (_qHead + 1) % Q_CAP;
        pthread_mutex_unlock(&_qMu);

        switch (e.type) {
        case CMD_CREATE:     uiCreateWebView(env); break;
        case CMD_SET_FRAME:  uiSetFrame(env, e.x, e.y, e.w, e.h); break;
        case CMD_SET_DARK:   uiSetDarkMode(env, e.intVal); break;
        case CMD_NAVIGATE:   uiNavigate(env, e.url); break;
        case CMD_GO_BACK:    uiSimpleCall(env, "goBack"); break;
        case CMD_GO_FORWARD: uiSimpleCall(env, "goForward"); break;
        case CMD_RELOAD:     uiSimpleCall(env, "reload"); break;
        case CMD_STOP:       uiSimpleCall(env, "stopLoading"); break;
        }
    }

    uiRefreshState(env);
}

/* ── Load embedded DEX and register native method ─────────────────── */

static int loadHelperClass(JNIEnv *env) {
    jclass bbCls = (*env)->FindClass(env, "java/nio/ByteBuffer");
    if (checkException(env) || !bbCls) return 0;

    jmethodID wrap = (*env)->GetStaticMethodID(env, bbCls, "wrap", "([B)Ljava/nio/ByteBuffer;");
    if (checkException(env) || !wrap) { (*env)->DeleteLocalRef(env, bbCls); return 0; }

    jbyteArray dexBytes = (*env)->NewByteArray(env, DEX_SIZE);
    (*env)->SetByteArrayRegion(env, dexBytes, 0, DEX_SIZE, (const jbyte *)_dex);

    jobject bb = (*env)->CallStaticObjectMethod(env, bbCls, wrap, dexBytes);
    if (checkException(env) || !bb) {
        (*env)->DeleteLocalRef(env, dexBytes);
        (*env)->DeleteLocalRef(env, bbCls);
        return 0;
    }

    jclass actCls = (*env)->GetObjectClass(env, _activity);
    jmethodID getCL = (*env)->GetMethodID(env, actCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject parentLoader = (*env)->CallObjectMethod(env, _activity, getCL);
    (*env)->DeleteLocalRef(env, actCls);
    if (checkException(env) || !parentLoader) {
        (*env)->DeleteLocalRef(env, bb);
        (*env)->DeleteLocalRef(env, dexBytes);
        (*env)->DeleteLocalRef(env, bbCls);
        return 0;
    }

    jclass dexCls = (*env)->FindClass(env, "dalvik/system/InMemoryDexClassLoader");
    if (checkException(env) || !dexCls) {
        (*env)->DeleteLocalRef(env, parentLoader);
        (*env)->DeleteLocalRef(env, bb);
        (*env)->DeleteLocalRef(env, dexBytes);
        (*env)->DeleteLocalRef(env, bbCls);
        return 0;
    }

    jmethodID dexInit = (*env)->GetMethodID(env, dexCls, "<init>",
                                             "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    jobject dexLoader = (*env)->NewObject(env, dexCls, dexInit, bb, parentLoader);
    if (checkException(env) || !dexLoader) {
        (*env)->DeleteLocalRef(env, dexCls);
        (*env)->DeleteLocalRef(env, parentLoader);
        (*env)->DeleteLocalRef(env, bb);
        (*env)->DeleteLocalRef(env, dexBytes);
        (*env)->DeleteLocalRef(env, bbCls);
        return 0;
    }

    jmethodID loadClass = (*env)->GetMethodID(env, dexCls, "loadClass",
                                               "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring className = (*env)->NewStringUTF(env, "R");
    jclass helperLocal = (jclass)(*env)->CallObjectMethod(env, dexLoader, loadClass, className);
    if (checkException(env) || !helperLocal) {
        LOGE("loadHelperClass: failed to load class R");
        (*env)->DeleteLocalRef(env, className);
        (*env)->DeleteLocalRef(env, dexLoader);
        (*env)->DeleteLocalRef(env, dexCls);
        (*env)->DeleteLocalRef(env, parentLoader);
        (*env)->DeleteLocalRef(env, bb);
        (*env)->DeleteLocalRef(env, dexBytes);
        (*env)->DeleteLocalRef(env, bbCls);
        return 0;
    }

    _helperCls = (jclass)(*env)->NewGlobalRef(env, helperLocal);

    JNINativeMethod methods[] = { { "n", "()V", (void *)nativeRunImpl } };
    if ((*env)->RegisterNatives(env, _helperCls, methods, 1) != 0) {
        LOGE("loadHelperClass: RegisterNatives failed");
        checkException(env);
        (*env)->DeleteGlobalRef(env, _helperCls); _helperCls = NULL;
    }

    (*env)->DeleteLocalRef(env, helperLocal);
    (*env)->DeleteLocalRef(env, className);
    (*env)->DeleteLocalRef(env, dexLoader);
    (*env)->DeleteLocalRef(env, dexCls);
    (*env)->DeleteLocalRef(env, parentLoader);
    (*env)->DeleteLocalRef(env, bb);
    (*env)->DeleteLocalRef(env, dexBytes);
    (*env)->DeleteLocalRef(env, bbCls);
    return _helperCls ? 1 : 0;
}

/* ── Public API ────────────────────────────────────────────────────── */

void WebView_Init(JavaVM *vm, JNIEnv *env, jobject ctx) {
    if (_jvm) return;
    if (!vm || !env || !ctx) return;

    _jvm = vm;
    _activity = (*env)->NewGlobalRef(env, ctx);
    _currentURL[0] = '\0';

    if (!loadHelperClass(env))
        LOGE("WebView_Init: failed to load helper class");
}

int WebView_TryCreate(void) {
    if (_webView) return 1;
    if (!_jvm || !_activity || !_helperCls) return 0;

    QEntry e = { .type = CMD_CREATE };
    enqueue(&e);
    return 0;
}

void WebView_SetFrame(double x, double y, double width, double height) {
    if (!_webView) return;
    QEntry e = { .type = CMD_SET_FRAME, .x = x, .y = y, .w = width, .h = height };
    enqueue(&e);
}

void WebView_SetDarkMode(int dark) {
    if (!_webView) return;
    QEntry e = { .type = CMD_SET_DARK, .intVal = dark };
    enqueue(&e);
}

void WebView_Navigate(const char *url) {
    if (!_webView) return;
    QEntry e = { .type = CMD_NAVIGATE };
    strncpy(e.url, url, sizeof(e.url) - 1);
    e.url[sizeof(e.url) - 1] = '\0';
    enqueue(&e);
}

void WebView_GoBack(void) {
    if (!_webView) return;
    QEntry e = { .type = CMD_GO_BACK };
    enqueue(&e);
}

void WebView_GoForward(void) {
    if (!_webView) return;
    QEntry e = { .type = CMD_GO_FORWARD };
    enqueue(&e);
}

void WebView_Reload(void) {
    if (!_webView) return;
    QEntry e = { .type = CMD_RELOAD };
    enqueue(&e);
}

void WebView_Stop(void) {
    if (!_webView) return;
    QEntry e = { .type = CMD_STOP };
    enqueue(&e);
}

int WebView_IsLoading(void) {
    return _loading;
}

const char* WebView_GetURL(void) {
    return _currentURL[0] ? _currentURL : "";
}
