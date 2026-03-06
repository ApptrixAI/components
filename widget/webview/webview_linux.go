package webview

/*
#cgo pkg-config: wpe-webkit-2.0 gbm
#cgo LDFLAGS: -lglib-2.0 -lpthread

#include "webview_linux.h"
#include <stdlib.h>
*/
import "C"
import (
	"image"
	"net/url"
	"sync/atomic"
	"time"
	"unsafe"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/canvas"
	"fyne.io/fyne/v2/driver/desktop"
	"fyne.io/fyne/v2/widget"
)

/* ── frame notification channel ────────────────────────────────────── */

var globalFrameCh = make(chan struct{}, 1)

//export goFrameReady
func goFrameReady() {
	select {
	case globalFrameCh <- struct{}{}:
	default:
	}
}

/* ── webView widget ─────────────────────────────────────────────────── */

type webView struct {
	widget.BaseWidget
	created  bool
	raster   *canvas.Raster
	framePtr atomic.Pointer[image.NRGBA]
}

func newWebView(handle uintptr) *webView {
	_ = handle // headless backend; no native window handle needed

	w := &webView{}
	w.ExtendBaseWidget(w)

	if C.WebView_Create(800, 600) != 0 {
		w.created = true

		w.raster = canvas.NewRaster(w.generateFrame)

		// Pump the GLib main context once per frame on the Fyne run loop.
		go func() {
			ticker := time.NewTicker(time.Second / 120)
			defer ticker.Stop()
			for range ticker.C {
				fyne.Do(func() { C.WebView_IterateLoop() })
			}
		}()

		// Frame-ready goroutine: copies pixels then refreshes the raster.
		go func() {
			for range globalFrameCh {
				w.copyFrame()
				if w.raster != nil {
					w.raster.Refresh()
				}
			}
		}()
	}
	return w
}

// generateFrame is called by canvas.Raster on the GUI thread each repaint.
func (w *webView) generateFrame(rw, rh int) image.Image {
	if f := w.framePtr.Load(); f != nil {
		return f
	}
	return image.NewNRGBA(image.Rect(0, 0, rw, rh))
}

// copyFrame reads the latest pixels from the C buffer and stores an NRGBA image.
func (w *webView) copyFrame() {
	var cw, ch, cstride C.int
	ptr := C.WebView_LockFrame(&cw, &ch, &cstride)
	if ptr == nil {
		return
	}
	defer C.WebView_UnlockFrame()

	width := int(cw)
	height := int(ch)
	stride := int(cstride)

	nrgba := image.NewNRGBA(image.Rect(0, 0, width, height))
	src := unsafe.Slice((*uint8)(unsafe.Pointer(ptr)), height*stride)

	// WPE_PIXEL_FORMAT_ARGB8888: on little-endian, each 32-bit word stored
	// as [B, G, R, A] bytes.  NRGBA expects [R, G, B, A].
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			si := y*stride + x*4
			di := (y*width + x) * 4
			nrgba.Pix[di+0] = src[si+2] // R
			nrgba.Pix[di+1] = src[si+1] // G
			nrgba.Pix[di+2] = src[si+0] // B
			nrgba.Pix[di+3] = 255       // A — force opaque; WPE headless leaves alpha at 0
		}
	}

	w.framePtr.Store(nrgba)
}

/* ── Input events ──────────────────────────────────────────────────── */

// fyneButtonToWPE maps Fyne mouse buttons to WPE/GDK button numbers.
// Fyne: Primary=1 Secondary=2 Tertiary=3
// WPE:  Left=1    Middle=2    Right=3
func fyneButtonToWPE(b desktop.MouseButton) C.int {
	switch b {
	case desktop.MouseButtonPrimary:
		return 1
	case desktop.MouseButtonSecondary:
		return 3
	case desktop.MouseButtonTertiary:
		return 2
	default:
		return 1
	}
}

func (w *webView) Tapped(*fyne.PointEvent) {
	c := fyne.CurrentApp().Driver().CanvasForObject(w)
	if c != nil {
		c.Focus(w)
	}
}

func (w *webView) MouseDown(ev *desktop.MouseEvent) {
	if !w.created {
		return
	}
	C.WebView_MouseDown(C.double(ev.Position.X), C.double(ev.Position.Y), fyneButtonToWPE(ev.Button))
}

func (w *webView) MouseUp(ev *desktop.MouseEvent) {
	if !w.created {
		return
	}
	C.WebView_MouseUp(C.double(ev.Position.X), C.double(ev.Position.Y), fyneButtonToWPE(ev.Button))
}

func (w *webView) MouseIn(ev *desktop.MouseEvent) {
	w.MouseMoved(ev)
}

func (w *webView) MouseMoved(ev *desktop.MouseEvent) {
	if !w.created {
		return
	}
	C.WebView_MouseMove(C.double(ev.Position.X), C.double(ev.Position.Y))
}

func (w *webView) MouseOut() {}

func (w *webView) Scrolled(ev *fyne.ScrollEvent) {
	if !w.created {
		return
	}
	// Fyne reports scroll deltas in large pixel-like units (~10-20 per notch);
	// WPE non-precise scroll expects small step values (~1 per notch).
	const scale = 1.0 / 20.0
	C.WebView_Scroll(C.double(ev.Position.X), C.double(ev.Position.Y),
		C.double(ev.Scrolled.DX*scale), C.double(ev.Scrolled.DY*scale))
}

/* ── Keyboard input (fyne.Focusable) ───────────────────────────────── */

// XKB keysym constants (matching GDK/XKB definitions).
const (
	xkReturn    = 0xff0d
	xkTab       = 0xff09
	xkBackSpace = 0xff08
	xkEscape    = 0xff1b
	xkDelete    = 0xffff
	xkInsert    = 0xff63
	xkHome      = 0xff50
	xkEnd       = 0xff57
	xkPageUp    = 0xff55
	xkPageDown  = 0xff56
	xkLeft      = 0xff51
	xkUp        = 0xff52
	xkRight     = 0xff53
	xkDown      = 0xff54
	xkF1        = 0xffbe
	xkShiftL    = 0xffe1
	xkControlL  = 0xffe3
	xkAltL      = 0xffe9
	xkSuperL    = 0xffeb
	xkSpace     = 0x0020
)

var fyneKeyToXKB = map[fyne.KeyName]uint{
	fyne.KeyReturn:    xkReturn,
	fyne.KeyTab:       xkTab,
	fyne.KeyBackspace: xkBackSpace,
	fyne.KeyEscape:    xkEscape,
	fyne.KeyDelete:    xkDelete,
	fyne.KeyInsert:    xkInsert,
	fyne.KeyHome:      xkHome,
	fyne.KeyEnd:       xkEnd,
	fyne.KeyPageUp:    xkPageUp,
	fyne.KeyPageDown:  xkPageDown,
	fyne.KeyLeft:      xkLeft,
	fyne.KeyUp:        xkUp,
	fyne.KeyRight:     xkRight,
	fyne.KeyDown:      xkDown,
	fyne.KeyF1:        xkF1,
	fyne.KeyF2:        xkF1 + 1,
	fyne.KeyF3:        xkF1 + 2,
	fyne.KeyF4:        xkF1 + 3,
	fyne.KeyF5:        xkF1 + 4,
	fyne.KeyF6:        xkF1 + 5,
	fyne.KeyF7:        xkF1 + 6,
	fyne.KeyF8:        xkF1 + 7,
	fyne.KeyF9:        xkF1 + 8,
	fyne.KeyF10:       xkF1 + 9,
	fyne.KeyF11:       xkF1 + 10,
	fyne.KeyF12:       xkF1 + 11,
	fyne.KeySpace:     xkSpace,
}

func (w *webView) FocusGained() {}
func (w *webView) FocusLost()   {}

func (w *webView) TypedRune(r rune) {
	if !w.created {
		return
	}
	// For Unicode codepoints <= 0x7e, XKB keysym == codepoint.
	// For >= 0x100, XKB keysym == codepoint | 0x01000000.
	var keyval C.uint
	if r < 0x100 {
		keyval = C.uint(r)
	} else {
		keyval = C.uint(r) | 0x01000000
	}
	C.WebView_KeyDown(keyval)
	C.WebView_KeyUp(keyval)
}

func (w *webView) TypedKey(ev *fyne.KeyEvent) {
	if !w.created {
		return
	}
	xk, ok := fyneKeyToXKB[ev.Name]
	if !ok {
		return
	}
	C.WebView_KeyDown(C.uint(xk))
	C.WebView_KeyUp(C.uint(xk))
}

/* ── fyne.Widget interface ─────────────────────────────────────────── */

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

func (w *webView) scale() float32 {
	c := fyne.CurrentApp().Driver().CanvasForObject(w)
	if c != nil {
		return c.Scale()
	}
	return 1
}

func (w *webView) updateFrame() {
	if !w.created {
		return
	}
	s := w.scale()
	pos := fyne.CurrentApp().Driver().AbsolutePositionForObject(w)
	size := w.Size()
	C.WebView_SetScale(C.double(s))
	C.WebView_SetFrame(C.int(pos.X), C.int(pos.Y), C.int(size.Width), C.int(size.Height))
}

/* ── WebView interface ─────────────────────────────────────────────── */

func (w *webView) Load(u *url.URL) {
	cs := C.CString(u.String())
	defer C.free(unsafe.Pointer(cs))
	C.WebView_Navigate(cs)
}

func (w *webView) Back()    { C.WebView_GoBack() }
func (w *webView) Forward() { C.WebView_GoForward() }
func (w *webView) Reload()  { C.WebView_Reload() }
func (w *webView) Stop()    { C.WebView_Stop() }

func (w *webView) Loading() bool {
	return C.WebView_IsLoading() != 0
}

func (w *webView) CurrentURL() *url.URL {
	u, _ := url.Parse(C.GoString(C.WebView_GetURL()))
	return u
}

/* ── WidgetRenderer ────────────────────────────────────────────────── */

type webViewRenderer struct {
	view *webView
}

func (r *webViewRenderer) Destroy() {}

func (r *webViewRenderer) Layout(size fyne.Size) {
	if r.view.raster != nil {
		r.view.raster.Resize(size)
	}
}

func (r *webViewRenderer) MinSize() fyne.Size { return fyne.NewSize(100, 100) }

func (r *webViewRenderer) Objects() []fyne.CanvasObject {
	if r.view.raster != nil {
		return []fyne.CanvasObject{r.view.raster}
	}
	return nil
}

func (r *webViewRenderer) Refresh() {
	if r.view.raster != nil {
		r.view.raster.Refresh()
	}
}
