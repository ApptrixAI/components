package webview // import apptrix.org/components/widget/webview

import (
	"errors"
	"fmt"
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
	nw, ok := w.(driver.NativeWindow)
	if !ok {
		return nil, errors.New("window does not implement NativeWindow")
	}

	nw.RunNative(func(ctx any) {
		switch ctx.(type) {
		case driver.MacWindowContext:
			web = newWebView(w)
		case driver.WindowsWindowContext:
			web = newWebView(w)
		case driver.X11WindowContext:
			web = newWebView(w)
		default:
			err = fmt.Errorf("unexpected context type: %T\n", ctx)
		}
	})

	return web, err
}
