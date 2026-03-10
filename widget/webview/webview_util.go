//go:build darwin || windows

package webview

import (
	"time"

	"fyne.io/fyne/v2"
)

func (w *webView) afterCreated(fn func()) {
	go func() {
		for !w.created {
			time.Sleep(100 * time.Millisecond)
		}

		fyne.Do(fn)
	}()
}

func (w *webView) untilCreated(fn func()) {
	go func() {
		for !w.created {
			fyne.Do(fn)

			time.Sleep(100 * time.Millisecond)
		}
	}()
}
