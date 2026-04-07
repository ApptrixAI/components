package chat

import (
	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/widget"
)

type messageList struct {
	widget.List
	paused bool
}

func newMessageList() *messageList {
	l := &messageList{}
	l.ExtendBaseWidget(l)

	l.HideSeparators = true
	return l
}

func (l *messageList) Resize(s fyne.Size) {
	l.List.Resize(s)
	l.ScrollToBottom()
}

func (l *messageList) ScrollToBottom() {
	if l.paused {
		return
	}
	l.List.ScrollToBottom()
}
