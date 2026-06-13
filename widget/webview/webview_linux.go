//go:build linux && !android

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
	"os"
	"runtime"
	"runtime/cgo"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/canvas"
	"fyne.io/fyne/v2/driver/desktop"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"
)

// The shared GLib main loop is started once for the whole process; every
// webView instance is created on, and driven by, that single loop.
var (
	loopOnce  sync.Once
	loopReady bool
)

func init() {
	// The headless WPE view renders entirely offscreen, so the AT-SPI
	// accessibility bridge has no toplevel to attach to. This avoids libatk-bridge
	// dereferencing a NULL connection in containers or when XDG_RUNTIME_DIR is unset.
	if _, set := os.LookupEnv("NO_AT_BRIDGE"); !set {
		os.Setenv("NO_AT_BRIDGE", "1")
	}

	// Move JSC's GC signal away from SIGUSR1 (signal 10) which
	// conflicts with Go's runtime signal handlers. Must be set before
	// WebKit initialises, so do it once at package load.
	os.Setenv("JSC_SIGNAL_FOR_GC", "42")
}

// ensureLoop starts the shared GLib main loop (and one-time global init)
// exactly once, blocking until initialisation has completed. It reports
// whether the loop is usable.
func ensureLoop() bool {
	loopOnce.Do(func() {
		// GLib expects the thread that creates the default main context to
		// be the one that iterates it, so the loop owns a locked OS thread
		// for its entire life.
		go func() {
			runtime.LockOSThread()
			C.WebView_RunLoop() // performs global init, then blocks forever
		}()
		loopReady = C.WebView_WaitReady() != 0
	})
	return loopReady
}

//export goFrameReady
func goFrameReady(h C.uintptr_t) {
	v, ok := cgo.Handle(h).Value().(*webView)
	if !ok {
		return
	}
	select {
	case v.frameCh <- struct{}{}:
	default:
	}
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

type webView struct {
	widget.BaseWidget
	created bool
	inst    *C.WebViewInstance
	handle  cgo.Handle
	img     *canvas.Image
	// Double-buffered frames: back is written by copyFrame, then swapped to front.
	front   atomic.Pointer[image.RGBA]
	back    *image.RGBA
	frameCh chan struct{}
	// closed signals the frame goroutine to stop; finished is closed once it
	// has exited, so Close can wait before freeing the C instance.
	closed   chan struct{}
	finished chan struct{}

	// cbMu guards the change-notification callbacks.
	cbMu             sync.Mutex
	onTitleChanged   func(string)
	onFaviconChanged func(image.Image)
}

func newWebView(_ fyne.Window) *webView {
	w := &webView{
		frameCh:  make(chan struct{}, 1),
		closed:   make(chan struct{}),
		finished: make(chan struct{}),
	}
	w.ExtendBaseWidget(w)

	if !ensureLoop() {
		return w // created == false; methods become no-ops
	}

	// The handle lets the C frame callback route frames back to this widget.
	w.handle = cgo.NewHandle(w)
	w.inst = C.WebView_Create(800, 600, C.uintptr_t(w.handle))
	if w.inst == nil {
		w.handle.Delete()
		return w
	}

	w.created = true
	w.syncTheme()

	fyne.CurrentApp().Settings().AddListener(func(fyne.Settings) {
		fyne.Do(w.syncTheme)
	})

	w.img = canvas.NewImageFromImage(image.NewRGBA(image.Rect(0, 0, 1, 1)))
	w.img.ScaleMode = canvas.ImageScaleSmooth

	// Frame-ready goroutine: copies pixels to back buffer, swaps to
	// front, then refreshes the image on the Fyne thread.
	go func() {
		defer close(w.finished)
		minInterval := time.Second / 60
		lastRefresh := time.Time{}
		for {
			select {
			case <-w.closed:
				return
			case <-w.frameCh:
			}
			now := time.Now()
			if now.Sub(lastRefresh) < minInterval {
				continue
			}
			w.copyFrame()
			lastRefresh = time.Now()
			fyne.Do(func() {
				if f := w.front.Load(); f != nil {
					w.img.Image = f
					w.img.Refresh()
				}
			})
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

// copyFrame reads the latest RGBA pixels from the C buffer into the back
// buffer, then promotes it to front so the Fyne image can display it.
// The C side already performs the BGRA→RGBA swizzle, so this is a straight copy.
func (w *webView) copyFrame() {
	var cw, ch, cstride C.int
	ptr := C.WebView_LockFrame(w.inst, &cw, &ch, &cstride)
	if ptr == nil {
		return
	}
	defer C.WebView_UnlockFrame(w.inst)

	width := int(cw)
	height := int(ch)
	nbytes := width * height * 4

	// Reuse the back buffer if the dimensions haven't changed.
	if w.back == nil || w.back.Rect.Dx() != width || w.back.Rect.Dy() != height {
		w.back = image.NewRGBA(image.Rect(0, 0, width, height))
	}

	src := unsafe.Slice((*uint8)(unsafe.Pointer(ptr)), nbytes)
	copy(w.back.Pix, src)

	// Swap: current back becomes front, old front becomes back for next frame.
	old := w.front.Swap(w.back)
	if old != nil && old.Rect.Dx() == width && old.Rect.Dy() == height {
		w.back = old
	} else {
		w.back = nil // dimensions changed, discard
	}
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
	C.WebView_MouseDown(w.inst, C.double(ev.Position.X), C.double(ev.Position.Y), fyneButtonToWPE(ev.Button))
}

func (w *webView) MouseUp(ev *desktop.MouseEvent) {
	if !w.created {
		return
	}
	C.WebView_MouseUp(w.inst, C.double(ev.Position.X), C.double(ev.Position.Y), fyneButtonToWPE(ev.Button))
}

func (w *webView) MouseIn(ev *desktop.MouseEvent) {
	w.MouseMoved(ev)
}

func (w *webView) MouseMoved(ev *desktop.MouseEvent) {
	if !w.created {
		return
	}
	C.WebView_MouseMove(w.inst, C.double(ev.Position.X), C.double(ev.Position.Y))
}

func (w *webView) MouseOut() {}

func (w *webView) Scrolled(ev *fyne.ScrollEvent) {
	if !w.created {
		return
	}
	// Fyne reports scroll deltas in large pixel-like units (~10-20 per notch);
	// WPE non-precise scroll expects small step values (~1 per notch).
	const scale = 1.0 / 20.0
	C.WebView_Scroll(w.inst, C.double(ev.Position.X), C.double(ev.Position.Y),
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
	C.WebView_KeyDown(w.inst, keyval)
	C.WebView_KeyUp(w.inst, keyval)
}

func (w *webView) TypedKey(ev *fyne.KeyEvent) {
	if !w.created {
		return
	}
	xk, ok := fyneKeyToXKB[ev.Name]
	if !ok {
		return
	}
	C.WebView_KeyDown(w.inst, C.uint(xk))
	C.WebView_KeyUp(w.inst, C.uint(xk))
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
	size := w.Size()
	// A zero-size resize puts the WPE view surface into a non-rendering state
	// it does not recover from, so ignore degenerate sizes (e.g. transient
	// 0x0 layouts inside a DocTabs before the tab is first shown).
	if size.Width <= 0 || size.Height <= 0 {
		return
	}
	s := w.scale()
	pos := fyne.CurrentApp().Driver().AbsolutePositionForObject(w)
	C.WebView_SetScale(w.inst, C.double(s))
	C.WebView_SetFrame(w.inst, C.int(pos.X), C.int(pos.Y), C.int(size.Width), C.int(size.Height))
}

/* ── WebView interface ─────────────────────────────────────────────── */

func (w *webView) Load(u *url.URL) {
	cs := C.CString(u.String())
	defer C.free(unsafe.Pointer(cs))
	C.WebView_Navigate(w.inst, cs)
}

func (w *webView) Back()    { C.WebView_GoBack(w.inst) }
func (w *webView) Forward() { C.WebView_GoForward(w.inst) }
func (w *webView) Reload()  { C.WebView_Reload(w.inst) }
func (w *webView) Stop()    { C.WebView_Stop(w.inst) }

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

	// Stop the frame goroutine and wait for it to exit so it can no longer
	// touch w.inst, then destroy the C instance (which blocks until the loop
	// thread has finished tearing the view down) and drop the handle.
	close(w.closed)
	<-w.finished

	C.WebView_Destroy(w.inst)
	w.inst = nil
	w.handle.Delete()
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

/* ── WidgetRenderer ────────────────────────────────────────────────── */

type webViewRenderer struct {
	view *webView
}

func (r *webViewRenderer) Destroy() {}

func (r *webViewRenderer) Layout(size fyne.Size) {
	if r.view.img != nil {
		r.view.img.Resize(size)
	}
}

func (r *webViewRenderer) MinSize() fyne.Size { return fyne.NewSize(100, 100) }

func (r *webViewRenderer) Objects() []fyne.CanvasObject {
	if r.view.img != nil {
		return []fyne.CanvasObject{r.view.img}
	}
	return nil
}

func (r *webViewRenderer) Refresh() {
	if r.view.img != nil {
		r.view.img.Refresh()
	}
}
