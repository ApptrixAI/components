package chat

import (
	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"
)

const (
	ColorNameRemoteMessageBackground = "remote-msg-bg"
	ColorNameLocalMessageBackground  = "local-msg-bg"
)

// Chat is a widget that shows a simple chat interface with a message list,
// an input field, and send button.
type Chat struct {
	widget.BaseWidget

	// OnSubmitted is called when a send is triggered from the send button or the
	// input field via enter key.
	OnSubmitted func(text string) `json:"-"`

	// Source is a simple list-oriented message source that only needs to
	// provide a Length and GetMessage method.
	Source MessageSource `json:"-"`

	// HideNames can be set to disable the display of sender names above chat messages.
	HideNames bool

	// Selectable should be set to true if you want the messages to support text selection.
	Selectable bool

	// TypeIncoming can be set to reveal messages that arrive from others one
	// character at a time, as chat bots often do. Messages that we send, and
	// those already present when the chat is first shown, always appear complete.
	TypeIncoming bool

	container *fyne.Container
	follow    *fyne.Container
	list      *messageList
	input     *input
	submit    *widget.Button
	maxId     int

	// typer is the message currently being setRevealed, if any.
	typer *typewriter
	// shown is how far down the list we have setRevealed - messages before it are
	// drawn complete, and are never typed out again as we scroll back to them.
	shown            int
	hasShown         bool
	lastRevealedText string
}

type MessageSource interface {
	Length() int
	GetMessage(int) Message
}

// Creates a new chat widget, using the given source, and calling the onSend
// function with the text and time of the message to send.
func New(source MessageSource, onSubmitted func(string)) *Chat {
	c := &Chat{
		Source:      source,
		OnSubmitted: onSubmitted,
	}
	c.ExtendBaseWidget(c)
	c.setup()
	return c
}

func (c *Chat) hideNames() bool {
	return c.HideNames
}

func (c *Chat) setup() {
	c.list = newMessageList()
	c.list.Length = func() int {
		if c.Source == nil {
			return 0
		}
		return c.Source.Length()
	}
	c.list.CreateItem = func() fyne.CanvasObject {
		return newMessageItem(c)
	}
	c.list.UpdateItem = func(id widget.ListItemID, obj fyne.CanvasObject) {
		if c.Source == nil {
			return
		}
		msg := c.Source.GetMessage(id)

		item := obj.(*messageItem)
		item.hideNames = c.hideNames
		item.setMessage(id, msg)

		c.list.SetItemHeight(id, item.MinSize().Height)

		// when an item id from an update is lower than the highest seen id
		// an item above the current view is being updated (scrolled back)
		if id > c.maxId || c.maxId >= c.list.Length() {
			c.maxId = id
		}
		c.list.paused = id < c.maxId
		if c.list.paused {
			c.follow.Show()
		} else {
			c.follow.Hide()
		}
	}
	c.list.OnSelected = func(id widget.ListItemID) {
		c.list.UnselectAll()
	}

	c.input = newInput(
		func() {
			if fn := c.submit.OnTapped; fn != nil {
				fn()
			}
		},
		func() {
			// The input has flipped between single-line and the
			// multi-line height — re-run the chat container's layout.
			if c.container != nil {
				c.container.Refresh()
			}
		},
	)
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

	followButton := widget.NewButtonWithIcon("", theme.MoveDownIcon(), func() {
		c.list.paused = false
		c.list.ScrollToBottom()
		c.list.RefreshItem(c.Source.Length() - 1)
	})
	c.follow = container.NewBorder(
		nil, container.NewBorder(
			nil, nil,
			nil, container.NewPadded(followButton),
		),
		nil, nil,
	)
	c.follow.Hide()

	c.container = container.NewBorder(
		nil, bottom,
		nil, nil,
		container.NewStack(
			c.list,
			c.follow,
		),
	)
}

func (c *Chat) Refresh() {
	c.list.Refresh()
	c.list.ScrollToBottom()

	c.input.Refresh()
	c.submit.Refresh()
}

func (c *Chat) CreateRenderer() fyne.WidgetRenderer {
	if c.container == nil {
		c.setup()
	}

	// messages that were in the source before we were first shown are history -
	// they did not arrive in front of the user, so they are not typed out
	if !c.hasShown {
		c.hasShown = true

		if c.Source != nil {
			c.shown = c.Source.Length()
			if c.shown > 0 {
				if last := c.Source.GetMessage(c.shown - 1); last != nil {
					c.lastRevealedText = last.Text()
				}
			}
		}
	}

	return widget.NewSimpleRenderer(c.container)
}
