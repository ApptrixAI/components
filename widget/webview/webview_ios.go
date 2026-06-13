//go:build ios

package webview

/*
#cgo LDFLAGS: -framework UIKit -framework WebKit -framework CoreGraphics

#include "webview_ios.h"
#include <stdlib.h>
*/
import "C"

import (
	"image"
	"net/url"
	"unsafe"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"
)

type webView struct {
	widget.BaseWidget
	win     fyne.Window
	created bool
}

func newWebView(win fyne.Window) *webView {
	w := &webView{win: win}
	w.untilCreated(func() {
		if C.WebView_Create() == 0 {
			return
		}
		w.updateFrame()
		w.created = true
	})
	w.ExtendBaseWidget(w)
	w.syncTheme()

	ch := make(chan fyne.Settings)
	fyne.CurrentApp().Settings().AddChangeListener(ch)
	go func() {
		for range ch {
			fyne.Do(w.syncTheme)
		}
	}()

	return w
}

func (w *webView) syncTheme() {
	dark := 0
	if fyne.CurrentApp().Settings().ThemeVariant() == theme.VariantDark {
		dark = 1
	}
	C.WebView_SetDarkMode(C.int(dark))
}

func (w *webView) CreateRenderer() fyne.WidgetRenderer {
	return &webViewRenderer{view: w}
}

func (w *webView) Resize(size fyne.Size) {
	w.BaseWidget.Resize(size)
	w.updateFrame()
}

func (w *webView) Move(pos fyne.Position) {
	w.BaseWidget.Move(pos)
	w.updateFrame()
}

func (w *webView) updateFrame() {
	if !w.created {
		return
	}

	pos := fyne.CurrentApp().Driver().AbsolutePositionForObject(w)
	size := w.Size()
	C.WebView_SetFrame(C.double(pos.X), C.double(pos.Y), C.double(size.Width), C.double(size.Height))
}

func (w *webView) Load(url *url.URL) {
	w.afterCreated(func() {
		cs := C.CString(url.String())
		defer C.free(unsafe.Pointer(cs))
		C.WebView_Navigate(cs)
	})
}

func (w *webView) Back() {
	C.WebView_GoBack()
}

func (w *webView) Forward() {
	C.WebView_GoForward()
}

func (w *webView) Reload() {
	C.WebView_Reload()
}

func (w *webView) Stop() {
	C.WebView_Stop()
}

func (w *webView) Loading() bool {
	return C.WebView_IsLoading() != 0
}

func (w *webView) CurrentURL() *url.URL {
	u, _ := url.Parse(C.GoString(C.WebView_GetURL()))
	return u
}

// TODO: title/favicon are not yet wired to WKWebView on this platform.
func (w *webView) Title() string                              { return "" }
func (w *webView) Favicon() image.Image                       { return nil }
func (w *webView) SetOnTitleChanged(func(title string))       {}
func (w *webView) SetOnFaviconChanged(func(icon image.Image)) {}

type webViewRenderer struct {
	view *webView
}

func (r *webViewRenderer) Destroy()                     {}
func (r *webViewRenderer) Layout(fyne.Size)             {}
func (r *webViewRenderer) MinSize() fyne.Size           { return fyne.NewSize(100, 100) }
func (r *webViewRenderer) Objects() []fyne.CanvasObject { return nil }
func (r *webViewRenderer) Refresh()                     {}
