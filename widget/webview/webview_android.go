//go:build android

package webview

/*
#cgo LDFLAGS: -landroid -llog

#include "webview_android.h"
#include <stdlib.h>
*/
import "C"
import (
	"net/url"
	"runtime"
	"time"
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
	win.(driver.NativeWindow).RunNative(func(ctx any) {
		ac := ctx.(*driver.AndroidWindowContext)
		if ac.VM == 0 || ac.Env == 0 || ac.Ctx == 0 {
			return
		}
		C.WebView_Init(
			(*C.JavaVM)(unsafe.Pointer(ac.VM)),
			(*C.JNIEnv)(unsafe.Pointer(ac.Env)),
			(C.jobject)(unsafe.Pointer(ac.Ctx)),
		)
	})

	w := &webView{}
	w.ExtendBaseWidget(w)

	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()

		for !w.created {
			if C.WebView_TryCreate() != 0 {
				w.created = true
				fyne.Do(func() {
					w.updateFrame()
					w.syncTheme()
				})
			} else {
				time.Sleep(100 * time.Millisecond)
			}
		}
	}()

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

	s := float64(1)
	if c := fyne.CurrentApp().Driver().CanvasForObject(w); c != nil {
		s = float64(c.Scale())
	}
	pos := fyne.CurrentApp().Driver().AbsolutePositionForObject(w)
	size := w.Size()
	C.WebView_SetFrame(C.double(float64(pos.X)*s), C.double(float64(pos.Y)*s),
		C.double(float64(size.Width)*s), C.double(float64(size.Height)*s))
}

func (w *webView) Load(u *url.URL) {
	go func() {
		for !w.created {
			time.Sleep(100 * time.Millisecond)
		}
		fyne.Do(func() {
			cs := C.CString(u.String())
			defer C.free(unsafe.Pointer(cs))
			C.WebView_Navigate(cs)
		})
	}()
}

func (w *webView) Back() {
	if !w.created {
		return
	}
	C.WebView_GoBack()
}

func (w *webView) Forward() {
	if !w.created {
		return
	}
	C.WebView_GoForward()
}

func (w *webView) Reload() {
	if !w.created {
		return
	}
	C.WebView_Reload()
}

func (w *webView) Stop() {
	if !w.created {
		return
	}
	C.WebView_Stop()
}

func (w *webView) Loading() bool {
	if !w.created {
		return false
	}
	return C.WebView_IsLoading() != 0
}

func (w *webView) CurrentURL() *url.URL {
	if !w.created {
		return &url.URL{}
	}
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
