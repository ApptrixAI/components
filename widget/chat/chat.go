package chat // import apptrix.org/components/widget/chat

import (
	"strings"
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"
)

const ColorNameRemoteMessageBackground = "remote-msg-bg"
const ColorNameLocalMessageBackground = "local-msg-bg"

type Chat struct {
	widget.BaseWidget
	container     *fyne.Container
	list          *messageList
	input         *input
	submit        *widget.Button
	paused        bool
	maxId         int
	inputDisabled bool
}

type MessageSender interface {
	Send(Message)
}

type MessageSource interface {
	Length() int
	GetItem(int) Message
}

func New(src MessageSource) *Chat {
	c := &Chat{}
	c.ExtendBaseWidget(c)

	sender, _ := src.(MessageSender)

	c.list = newMessageList()
	c.list.Length = func() int {
		return src.Length()
	}
	c.list.CreateItem = func() fyne.CanvasObject {
		return newMessageItem()
	}
	c.list.UpdateItem = func(id widget.ListItemID, obj fyne.CanvasObject) {
		msg := src.GetItem(id)

		item := obj.(*messageItem)
		item.SetSender(msg.SenderName())
		item.SetText(msg.Text())
		item.SetOwner(msg.SenderName() == "me")

		minSize := item.MinSize()

		if item.height != minSize.Height {
			item.height = minSize.Height
			c.list.SetItemHeight(id, minSize.Height)
		}

		// when an item id from an update is lower than the highest seen id
		// an item above the current view is being updated (scrolled back)
		if id > c.maxId || c.maxId >= c.list.Length() {
			c.maxId = id
		}
		c.paused = id < c.maxId
	}

	c.input = newInput(func() {
		if fn := c.submit.OnTapped; fn != nil {
			fn()
		}
	})
	c.submit = widget.NewButtonWithIcon("", theme.MailSendIcon(), func() {
		if sender == nil {
			return
		}

		t := strings.TrimSpace(c.input.Text)
		if len(t) == 0 {
			return
		}

		sender.Send(&genericMessage{"", "sndrxxx", "me", t, time.Now()})

		c.input.SetText("")
		c.list.Refresh()
		c.list.ScrollToBottom()
		c.list.ScrollToBottom()
	})

	bottom := container.NewBorder(
		nil, nil,
		nil, c.submit,
		c.input,
	)
	if sender == nil {
		if c.inputDisabled {
			c.input.Disable()
			c.submit.Disable()
		} else {
			bottom.Hide()
		}
	}

	c.container = container.NewBorder(
		nil, bottom,
		nil, nil,
		c.list,
	)

	c.list.Refresh()

	return c
}

func (c *Chat) ScrollToBottom() {
	if c.paused {
		return
	}
	c.list.ScrollToBottom()
}

func (c *Chat) CreateRenderer() fyne.WidgetRenderer {
	return widget.NewSimpleRenderer(c.container)
}
