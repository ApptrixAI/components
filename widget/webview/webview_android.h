#ifndef WEBVIEW_ANDROID_H
#define WEBVIEW_ANDROID_H

#include <jni.h>
#include <stddef.h>

void WebView_Init(JavaVM *vm, JNIEnv *env, jobject ctx);
int WebView_TryCreate(void);
void WebView_SetFrame(double x, double y, double width, double height);
void WebView_SetDarkMode(int dark);
void WebView_Navigate(const char *url);
void WebView_GoBack(void);
void WebView_GoForward(void);
void WebView_Reload(void);
void WebView_Stop(void);
int WebView_IsLoading(void);
const char* WebView_GetURL(void);

#endif
