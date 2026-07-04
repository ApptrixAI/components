#ifndef WEBVIEW_APPLE_META_H
#define WEBVIEW_APPLE_META_H

/*
 * Shared title/favicon plumbing for the macOS and iOS WKWebView backends.
 *
 * WKWebView exposes the page title (via KVO) but no favicon API, so — as on
 * the WPE/Linux backend — the page itself locates its icon, lets WebKit decode
 * whatever format it is, rasterises it to RGBA through a <canvas>, and posts the
 * pixels (plus the title) back through a "fyneMeta" script message handler.
 */

#import <Foundation/Foundation.h>
#import <WebKit/WebKit.h>

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Exported from Go (see the //export directives in the platform .go files). */
extern void goTitleChanged(uintptr_t goHandle);
extern void goFaviconChanged(uintptr_t goHandle);

/* Cached page metadata, guarded by mu and read out by the Go side on demand.
   Embedded in each platform's WebViewInstance struct. */
typedef struct {
    pthread_mutex_t mu;
    char           *title;
    uint8_t        *favBuf;   /* RGBA8888, favW*favH*4 bytes, or NULL */
    int             favW;
    int             favH;
    uintptr_t       goHandle;
} AppleMeta;

/* Injected at document end into the main frame. Uses single-quoted JS so the C
   string literal needs no double-quote escaping beyond the icon selector. */
static const char *FYNE_META_SCRIPT =
"(function(){"
"  var MAX=64, lastIcon=null, lastTitle=null;"
"  function post(s){try{window.webkit.messageHandlers.fyneMeta.postMessage(s);}catch(e){}}"
"  function sendTitle(){var t=document.title||'';if(t!==lastTitle){lastTitle=t;post('T'+t);}}"
"  function pick(){"
"    var ls=document.querySelectorAll('link[rel~=\"icon\"]'),best=null,bs=-1;"
"    for(var i=0;i<ls.length;i++){var l=ls[i];if(!l.href)continue;"
"      var s=0,m=l.sizes&&l.sizes.value&&/(\\d+)x(\\d+)/.exec(l.sizes.value);"
"      if(m)s=parseInt(m[1],10);if(s>=bs){bs=s;best=l.href;}}"
"    return best||(location.origin+'/favicon.ico');"
"  }"
"  function sendIcon(u){"
"    var img=new Image();img.crossOrigin='anonymous';"
"    img.onload=function(){try{"
"      var w=img.naturalWidth||img.width,h=img.naturalHeight||img.height;if(!w||!h)return;"
"      if(w>MAX||h>MAX){var sc=MAX/Math.max(w,h);w=Math.round(w*sc);h=Math.round(h*sc);}"
"      var c=document.createElement('canvas');c.width=w;c.height=h;"
"      var x=c.getContext('2d');x.drawImage(img,0,0,w,h);"
"      var d=x.getImageData(0,0,w,h).data;"
"      var b=btoa(String.fromCharCode.apply(null,d));"
"      post('F '+w+' '+h+' '+b);"
"    }catch(e){}};"
"    img.src=u;"
"  }"
"  function update(){sendTitle();var u=pick();if(u!==lastIcon){lastIcon=u;sendIcon(u);}}"
"  update();"
"  try{new MutationObserver(update).observe(document.documentElement,"
"    {childList:true,subtree:true,characterData:true,attributes:true,"
"     attributeFilter:['href','rel','sizes']});}catch(e){}"
"})();";

/* ── AppleMeta lifecycle & accessors ─────────────────────────────────── */

static inline void AppleMeta_Init(AppleMeta *m, uintptr_t goHandle) {
    pthread_mutex_init(&m->mu, NULL);
    m->title    = NULL;
    m->favBuf   = NULL;
    m->favW     = 0;
    m->favH     = 0;
    m->goHandle = goHandle;
}

static inline void AppleMeta_Destroy(AppleMeta *m) {
    pthread_mutex_lock(&m->mu);
    free(m->title);
    m->title = NULL;
    free(m->favBuf);
    m->favBuf = NULL;
    pthread_mutex_unlock(&m->mu);
    pthread_mutex_destroy(&m->mu);
}

/* Returns a newly malloc'd copy of the title (NULL if none); caller frees. */
static inline char *AppleMeta_CopyTitle(AppleMeta *m) {
    pthread_mutex_lock(&m->mu);
    char *t = m->title ? strdup(m->title) : NULL;
    pthread_mutex_unlock(&m->mu);
    return t;
}

/* Locks the favicon buffer for reading. Returns NULL (and does not lock) when
   no favicon is present; otherwise the caller must call AppleMeta_UnlockFavicon. */
static inline const uint8_t *AppleMeta_LockFavicon(AppleMeta *m, int *w, int *h) {
    pthread_mutex_lock(&m->mu);
    if (m->favBuf == NULL) {
        pthread_mutex_unlock(&m->mu);
        return NULL;
    }
    *w = m->favW;
    *h = m->favH;
    return m->favBuf;
}

static inline void AppleMeta_UnlockFavicon(AppleMeta *m) {
    pthread_mutex_unlock(&m->mu);
}

/* ── Script message handler ──────────────────────────────────────────── */

/* Deliberately does not declare <WKScriptMessageHandler> conformance: that
   would pull in the protocol's link symbol, which the iOS device build avoids
   (see the NSClassFromString dance in webview_ios.m). */
@interface FyneMetaHandler : NSObject
@property (nonatomic, assign) AppleMeta *meta;   /* not retained; owned by the instance */
@end

@implementation FyneMetaHandler

- (void)userContentController:(WKUserContentController *)ucc
      didReceiveScriptMessage:(WKScriptMessage *)message {
    (void)ucc;
    AppleMeta *m = self.meta;
    if (m == NULL || ![message.body isKindOfClass:[NSString class]]) return;

    NSString *body = (NSString *)message.body;
    if (body.length < 1) return;

    unichar tag = [body characterAtIndex:0];
    if (tag == 'T') {
        NSString *title = [body substringFromIndex:1];
        const char *utf8 = [title UTF8String];
        pthread_mutex_lock(&m->mu);
        free(m->title);
        m->title = (utf8 && *utf8) ? strdup(utf8) : NULL;
        pthread_mutex_unlock(&m->mu);
        goTitleChanged(m->goHandle);
        return;
    }
    if (tag == 'F') {
        /* "F <w> <h> <base64 RGBA>" */
        NSScanner *sc = [NSScanner scannerWithString:body];
        [sc setScanLocation:1];
        int w = 0, h = 0;
        if (![sc scanInt:&w] || ![sc scanInt:&h] || w <= 0 || h <= 0) return;
        NSString *b64 = [[body substringFromIndex:[sc scanLocation]]
            stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        NSData *data = [[NSData alloc] initWithBase64EncodedString:b64
                            options:NSDataBase64DecodingIgnoreUnknownCharacters];
        if (data != nil && data.length == (NSUInteger)w * (NSUInteger)h * 4) {
            pthread_mutex_lock(&m->mu);
            free(m->favBuf);
            m->favBuf = (uint8_t *)malloc(data.length);
            if (m->favBuf != NULL) {
                memcpy(m->favBuf, data.bytes, data.length);
                m->favW = w;
                m->favH = h;
            }
            pthread_mutex_unlock(&m->mu);
            [data release];
            goFaviconChanged(m->goHandle);
        } else {
            [data release];
        }
    }
}

@end

/* Installs the message handler and user script into config.userContentController,
   which must be done before the WKWebView is created from that config. */
static inline void FyneMeta_Install(WKWebViewConfiguration *config, AppleMeta *meta) {
    FyneMetaHandler *handler = [[FyneMetaHandler alloc] init];
    handler.meta = meta;
    [config.userContentController addScriptMessageHandler:(id)handler name:@"fyneMeta"];
    [handler release];   /* the content controller retains it */

    Class scriptClass = NSClassFromString(@"WKUserScript");
    id script = [[scriptClass alloc]
        initWithSource:[NSString stringWithUTF8String:FYNE_META_SCRIPT]
         injectionTime:WKUserScriptInjectionTimeAtDocumentEnd
      forMainFrameOnly:YES];
    [config.userContentController addUserScript:script];
    [script release];
}

/* Detaches the handler so no message can arrive after the instance is freed. */
static inline void FyneMeta_Uninstall(WKWebView *webView) {
    WKUserContentController *ucc = webView.configuration.userContentController;
    [ucc removeScriptMessageHandlerForName:@"fyneMeta"];
    [ucc removeAllUserScripts];
}

#endif // WEBVIEW_APPLE_META_H
