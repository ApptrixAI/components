#ifndef WEBVIEW_WINDOWS_H
#define WEBVIEW_WINDOWS_H

#include <stddef.h>

void WebView_Create(void *hwnd);
void WebView_SetFrame(double x, double y, double width, double height);
void WebView_SetDarkMode(int dark);
void WebView_Navigate(const char *url);
void WebView_GoBack(void);
void WebView_GoForward(void);
void WebView_Reload(void);
void WebView_Stop(void);
int WebView_IsLoading(void);
const char* WebView_GetURL(void);
void WebView_Focus(void);
void WebView_Unfocus(void);

#endif
