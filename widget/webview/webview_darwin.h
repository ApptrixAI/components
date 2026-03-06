#ifndef WEBVIEW_DARWIN_H
#define WEBVIEW_DARWIN_H

#include <stddef.h>

void WebView_Create(void *nsWindow);
void WebView_SetFrame(double x, double y, double width, double height);
void WebView_Navigate(const char *url);
void WebView_GoBack(void);
void WebView_GoForward(void);
void WebView_Reload(void);
void WebView_Stop(void);
int WebView_IsLoading(void);
const char* WebView_GetURL(void);

#endif
