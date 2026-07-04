package webview

/*
#cgo LDFLAGS: -lole32 -ladvapi32

#include "webview_windows.h"
#include <stdlib.h>
#include <stdint.h>

static inline WebViewInstance *WebView_CreateFromUintptr(uintptr_t p) {
	return WebView_Create((void *)p);
}
*/
import "C"

import (
	"image"
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
	inst    *C.WebViewInstance
}

func newWebView(win fyne.Window) *webView {
	w := &webView{}

	w.untilCreated(func() {
		win.(driver.NativeWindow).RunNative(func(ctx any) {
			if w.inst != nil {
				w.created = true
				return
			}
			handle := ctx.(driver.WindowsWindowContext).HWND
			if handle == 0 {
				return
			}

			inst := C.WebView_CreateFromUintptr(C.uintptr_t(handle))
			if inst == nil {
				return
			}
			w.inst = inst
			w.created = true
			w.updateFrame()
			// A view created for a background tab must not steal the front.
			w.setVisible(w.Visible())
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
	C.WebView_SetDarkMode(w.inst, C.int(dark))
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

// Show reveals the native view.
func (w *webView) Show() {
	w.BaseWidget.Show()
	w.setVisible(true)
	w.updateFrame()
}

// Hide detaches the native view from display.
func (w *webView) Hide() {
	w.BaseWidget.Hide()
	w.setVisible(false)
}

func (w *webView) setVisible(visible bool) {
	if !w.created {
		return
	}
	v := C.int(0)
	if visible {
		v = 1
	}
	C.WebView_SetVisible(w.inst, v)
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
	C.WebView_SetFrame(w.inst, C.double(float64(pos.X)*s), C.double(float64(pos.Y)*s),
		C.double(float64(size.Width)*s), C.double(float64(size.Height)*s))
}

func (w *webView) Tapped(*fyne.PointEvent) {
	c := fyne.CurrentApp().Driver().CanvasForObject(w)
	if c != nil {
		c.Focus(w)
	}
}

func (w *webView) FocusGained() {
	C.WebView_Focus(w.inst)
}

func (w *webView) FocusLost() {
	C.WebView_Unfocus(w.inst)
}

func (w *webView) TypedRune(rune)          {}
func (w *webView) TypedKey(*fyne.KeyEvent) {}

func (w *webView) Load(url *url.URL) {
	w.afterCreated(func() {
		cs := C.CString(url.String())
		defer C.free(unsafe.Pointer(cs))
		C.WebView_Navigate(w.inst, cs)
	})
}

func (w *webView) Back() {
	C.WebView_GoBack(w.inst)
}

func (w *webView) Forward() {
	C.WebView_GoForward(w.inst)
}

func (w *webView) Reload() {
	C.WebView_Reload(w.inst)
}

func (w *webView) Stop() {
	C.WebView_Stop(w.inst)
}

func (w *webView) Loading() bool {
	return C.WebView_IsLoading(w.inst) != 0
}

// Close tears down the underlying web engine and releases its resources.
// After Close the widget becomes inert; it is safe to call more than once.
func (w *webView) Close() {
	if !w.created {
		return
	}
	w.created = false
	C.WebView_Destroy(w.inst)
	w.inst = nil
}

func (w *webView) CurrentURL() *url.URL {
	u, _ := url.Parse(C.GoString(C.WebView_GetURL(w.inst)))
	return u
}

// TODO: title/favicon are not yet wired to the WebView2 control on this platform.
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
