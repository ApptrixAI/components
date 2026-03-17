package chat

import (
	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/widget"
)

type input struct {
	widget.Entry
	onSubmit func()
}

func newInput(submit func()) *input {
	i := &input{}
	i.ExtendBaseWidget(i)
	i.onSubmit = submit
	//i.MultiLine = true
	//i.Wrapping = fyne.TextWrapBreak

	return i
}

func (i *input) KeyDown(key *fyne.KeyEvent) {
	if key.Name == "Return" && i.onSubmit != nil {
		i.onSubmit()
		return
	}
	i.Entry.KeyDown(key)
}
