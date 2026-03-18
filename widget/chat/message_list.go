package chat

import (
	"fyne.io/fyne/v2/widget"
)

type messageList struct {
	widget.List
}

func newMessageList() *messageList {
	l := &messageList{}
	l.ExtendBaseWidget(l)

	return l
}

func (l *messageList) Resize(s fyne.Size) {
	l.List.Resize(s)
	l.List.ScrollToBottom()
}
