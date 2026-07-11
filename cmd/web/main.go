package main

import (
	"net/url"

	"apptrix.org/components/widget/webview"
	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"
)

func main() {
	a := app.New()
	w := a.NewWindow("Web")

	web, _ := webview.New(w)
	web.SetOnTitleChanged(func(title string) {
		w.SetTitle(title)
	})

	u, _ := url.Parse("https://fyne.io")
	web.Load(u)

	w.SetContent(web)
	w.Resize(fyne.NewSize(580, 450))
	w.ShowAndRun()
}
