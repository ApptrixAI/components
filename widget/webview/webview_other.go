//go:build !android && !darwin && !ios && !linux && !windows

package webview

import "fyne.io/fyne/v2"

func newWebView(win fyne.Window) WebView {
	return nil
}
