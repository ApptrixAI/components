package chat

import (
	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"
)

const ColorNameRemoteMessageBackground = "remote-msg-bg"
const ColorNameLocalMessageBackground = "local-msg-bg"

// Chat is a widget that shows a simple chat interface with a message list,
// an input field, and send button.
type Chat struct {
	widget.BaseWidget

	// OnSubmitted is called when a send is triggered from the send button or the
	// input field via enter key.
	OnSubmitted func(text string)

	container *fyne.Container
	list      *messageList
	input     *input
	submit    *widget.Button
	source    MessageSource
	maxId     int
}

type MessageSource interface {
	Length() int
	GetItem(int) Message
}

// Creates a new chat widget, using the given source, and calling the onSend
// function with the text and time of the message to send.
func New(source MessageSource, onSubmitted func(string)) *Chat {
	c := &Chat{
		source:      source,
		OnSubmitted: onSubmitted,
	}
	c.ExtendBaseWidget(c)

	c.list = newMessageList()
	c.list.Length = func() int {
		if c.source == nil {
			return 0
		}
		return c.source.Length()
	}
	c.list.CreateItem = func() fyne.CanvasObject {
		return newMessageItem()
	}
	c.list.UpdateItem = func(id widget.ListItemID, obj fyne.CanvasObject) {
		if c.source == nil {
			return
		}
		msg := c.source.GetItem(id)

		item := obj.(*messageItem)
		item.setMessage(msg)

		c.list.SetItemHeight(id, item.MinSize().Height)

		// when an item id from an update is lower than the highest seen id
		// an item above the current view is being updated (scrolled back)
		if id > c.maxId || c.maxId >= c.list.Length() {
			c.maxId = id
		}
		c.list.paused = id < c.maxId
	}
	c.list.OnSelected = func(id widget.ListItemID) {
		c.list.UnselectAll()
	}

	c.input = newInput(func() {
		if fn := c.submit.OnTapped; fn != nil {
			fn()
		}
	})
	c.submit = widget.NewButtonWithIcon("", theme.MailSendIcon(), func() {
		if c.input.Text == "" {
			return
		}

		if fn := c.OnSubmitted; fn != nil {
			fn(c.input.Text)
		}

		c.input.SetText("")
		c.list.paused = false
		c.list.ScrollToBottom()
		c.list.Refresh()
		c.list.ScrollToBottom()
	})
	c.submit.Disable()
	c.input.OnChanged = func(s string) {
		if c.input.Text == "" {
			c.submit.Disable()
		} else {
			c.submit.Enable()
		}
	}

	if c.OnSubmitted == nil {
		c.input.Disable()
		c.submit.Disable()
	}

	bottom := container.NewBorder(
		nil, nil,
		nil, c.submit,
		c.input,
	)

	c.container = container.NewBorder(
		nil, bottom,
		nil, nil,
		c.list,
	)

	return c
}

func (c *Chat) ScrollToBottom() {
	c.list.ScrollToBottom()
}

func (c *Chat) CreateRenderer() fyne.WidgetRenderer {
	return widget.NewSimpleRenderer(c.container)
}
