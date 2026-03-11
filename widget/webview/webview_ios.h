#ifndef WEBVIEW_IOS_H
#define WEBVIEW_IOS_H

#include <stddef.h>

int WebView_Create(void);
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
