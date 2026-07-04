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
	"runtime/cgo"
	"sync"
	"unsafe"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"
)

type webView struct {
	widget.BaseWidget
	win     fyne.Window
	created bool
	inst    *C.WebViewInstance
	handle  cgo.Handle

	// cbMu guards the change-notification callbacks.
	cbMu             sync.Mutex
	onTitleChanged   func(string)
	onFaviconChanged func(image.Image)
}

//export goTitleChanged
func goTitleChanged(h C.uintptr_t) {
	v, ok := cgo.Handle(h).Value().(*webView)
	if !ok {
		return
	}
	v.cbMu.Lock()
	cb := v.onTitleChanged
	v.cbMu.Unlock()
	if cb != nil {
		title := v.Title()
		fyne.Do(func() { cb(title) })
	}
}

//export goFaviconChanged
func goFaviconChanged(h C.uintptr_t) {
	v, ok := cgo.Handle(h).Value().(*webView)
	if !ok {
		return
	}
	v.cbMu.Lock()
	cb := v.onFaviconChanged
	v.cbMu.Unlock()
	if cb != nil {
		icon := v.Favicon()
		fyne.Do(func() { cb(icon) })
	}
}

func newWebView(win fyne.Window) *webView {
	w := &webView{win: win}
	w.untilCreated(func() {
		if w.inst != nil {
			w.created = true
			return
		}
		if w.handle == 0 {
			w.handle = cgo.NewHandle(w)
		}
		inst := C.WebView_Create(C.uintptr_t(w.handle))
		if inst == nil {
			return
		}
		w.inst = inst
		w.created = true
		w.updateFrame()
		// A view created for a background tab must not steal the front.
		w.setVisible(w.Visible())
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

// Show reveals the native view and raises it above any sibling web views.
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

	pos := fyne.CurrentApp().Driver().AbsolutePositionForObject(w)
	size := w.Size()
	C.WebView_SetFrame(w.inst, C.double(pos.X), C.double(pos.Y), C.double(size.Width), C.double(size.Height))
}

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
	if w.handle != 0 {
		w.handle.Delete()
		w.handle = 0
	}
}

func (w *webView) CurrentURL() *url.URL {
	u, _ := url.Parse(C.GoString(C.WebView_GetURL(w.inst)))
	return u
}

// Title returns the current page title, or "" if none/unavailable.
func (w *webView) Title() string {
	if !w.created {
		return ""
	}
	cs := C.WebView_CopyTitle(w.inst)
	if cs == nil {
		return ""
	}
	defer C.free(unsafe.Pointer(cs))
	return C.GoString(cs)
}

// Favicon returns the current page favicon as an image, or nil if none is
// available yet.
func (w *webView) Favicon() image.Image {
	if !w.created {
		return nil
	}
	var cw, ch C.int
	ptr := C.WebView_LockFavicon(w.inst, &cw, &ch)
	if ptr == nil {
		return nil
	}
	defer C.WebView_UnlockFavicon(w.inst)

	width, height := int(cw), int(ch)
	img := image.NewRGBA(image.Rect(0, 0, width, height))
	src := unsafe.Slice((*uint8)(unsafe.Pointer(ptr)), width*height*4)
	copy(img.Pix, src)
	return img
}

// SetOnTitleChanged registers a callback invoked (on the Fyne goroutine)
// whenever the page title changes. Pass nil to clear it.
func (w *webView) SetOnTitleChanged(fn func(title string)) {
	w.cbMu.Lock()
	w.onTitleChanged = fn
	w.cbMu.Unlock()
}

// SetOnFaviconChanged registers a callback invoked (on the Fyne goroutine)
// whenever the page favicon changes. The image may be nil. Pass nil to clear it.
func (w *webView) SetOnFaviconChanged(fn func(icon image.Image)) {
	w.cbMu.Lock()
	w.onFaviconChanged = fn
	w.cbMu.Unlock()
}

type webViewRenderer struct {
	view *webView
}

func (r *webViewRenderer) Destroy()                     {}
func (r *webViewRenderer) Layout(fyne.Size)             {}
func (r *webViewRenderer) MinSize() fyne.Size           { return fyne.NewSize(100, 100) }
func (r *webViewRenderer) Objects() []fyne.CanvasObject { return nil }
func (r *webViewRenderer) Refresh()                     {}
