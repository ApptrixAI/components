package webview

/*
#cgo LDFLAGS: -lole32 -ladvapi32

#include "webview_windows.h"
#include <stdlib.h>
#include <stdint.h>

static inline void WebView_CreateFromUintptr(uintptr_t p) {
	WebView_Create((void *)p);
}
*/
import "C"
import (
	"net/url"
	"unsafe"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/driver"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"
)

type webView struct {
	widget.BaseWidget
	created bool
}

func newWebView(win fyne.Window) *webView {
	w := &webView{}

	w.untilCreated(func() {
		win.(driver.NativeWindow).RunNative(func(ctx any) {
			handle := ctx.(driver.WindowsWindowContext).HWND
			if handle == 0 {
				return
			}

			C.WebView_CreateFromUintptr(C.uintptr_t(handle))
			w.updateFrame()
			w.created = true
		})
	})

	w.ExtendBaseWidget(w)
	w.syncTheme()

	fyne.CurrentApp().Settings().AddListener(func(fyne.Settings) {
		fyne.Do(w.syncTheme)
	})

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

	s := float64(1)
	if c := fyne.CurrentApp().Driver().CanvasForObject(w); c != nil {
		s = float64(c.Scale())
	}
	pos := fyne.CurrentApp().Driver().AbsolutePositionForObject(w)
	size := w.Size()
	C.WebView_SetFrame(C.double(float64(pos.X)*s), C.double(float64(pos.Y)*s),
		C.double(float64(size.Width)*s), C.double(float64(size.Height)*s))
}

func (w *webView) Tapped(*fyne.PointEvent) {
	c := fyne.CurrentApp().Driver().CanvasForObject(w)
	if c != nil {
		c.Focus(w)
	}
}

func (w *webView) FocusGained() {
	C.WebView_Focus()
}

func (w *webView) FocusLost() {
	C.WebView_Unfocus()
}

func (w *webView) TypedRune(rune)          {}
func (w *webView) TypedKey(*fyne.KeyEvent) {}

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

type webViewRenderer struct {
	view *webView
}

func (r *webViewRenderer) Destroy()                     {}
func (r *webViewRenderer) Layout(fyne.Size)             {}
func (r *webViewRenderer) MinSize() fyne.Size           { return fyne.NewSize(100, 100) }
func (r *webViewRenderer) Objects() []fyne.CanvasObject { return nil }
func (r *webViewRenderer) Refresh()                     {}
