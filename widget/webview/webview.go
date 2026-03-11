package webview // import apptrix.org/components/widget/webview

import (
	"errors"
	"net/url"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/driver"
)

type WebView interface {
	fyne.Widget

	Back()
	Forward()
	Stop()
	Reload()
	Load(*url.URL)
	Loading() bool
	CurrentURL() *url.URL
}

func NewWebView(w fyne.Window) (web WebView, err error) {
	_, ok := w.(driver.NativeWindow)
	if !ok {
		return nil, errors.New("webview requires a NativeWindow")
	}

	web = newWebView(w)
	if web == nil {
		return web, errors.New("WebView is not supported on current platform")
	}

	return web, err
}
