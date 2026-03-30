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

// New returns a new WebView that uses a system library to provide an embedded web widget.
// The window parameter is required for some platforms to draw the native component.
// Do not move this widget to a different window once it has been created.
func New(w fyne.Window) (web WebView, err error) {
	_, ok := w.(driver.NativeWindow)
	if !ok {
		return nil, errors.New("webview requires a NativeWindow")
	}

	web = newWebView(w)
	//lint:ignore SA4023 on different platforms the view may be nil
	if web == nil {
		return web, errors.New("WebView is not supported on current platform")
	}

	return web, err
}
