package chat

import (
	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/layout"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"
)

type messageItem struct {
	widget.BaseWidget
	c *Chat

	container *fyne.Container
	top       *fyne.Container
	sender    *widget.Label
	text      *widget.Label
	bg        *bubble
	inset     *fyne.Container
	spacer    fyne.CanvasObject
	obj       *fyne.Container
	hideNames func() bool
	mine      bool

	// waiting is set while this message is queued behind another that is still being revealed.
	waiting bool
}

func newMessageItem(c *Chat) *messageItem {
	m := &messageItem{c: c}
	m.ExtendBaseWidget(m)

	m.sender = widget.NewLabel("")
	m.sender.SizeName = theme.SizeNameCaptionText
	m.text = widget.NewLabel("")
	m.text.Wrapping = fyne.TextWrapWord
	m.text.Selectable = c.Selectable

	m.bg = newBubble()
	m.bg.radius = c.Theme().Size(theme.SizeNamePadding)

	m.obj = container.NewPadded()
	m.obj.Hide()

	m.spacer = widget.NewLabel("")

	// the content is inset from the tail so that it cannot overlap it
	m.inset = container.New(layout.NewCustomPaddedLayout(0, 0, 0, 0), m.obj, m.text)

	m.top = container.NewBorder(
		nil, nil,
		m.sender, nil,
	)
	m.container = container.NewBorder(
		m.top, nil,
		nil, m.spacer,
		container.NewStack(
			m.bg,
			m.inset,
		),
	)
	m.applySide()

	return m
}

// applySide points the bubble tail, and pushes the message across, to whichever
// side of the conversation this message belongs to.
func (m *messageItem) applySide() {
	m.bg.mine = m.mine

	// the tail hangs below the bubble body, so keep the content clear of it
	m.inset.Layout = layout.NewCustomPaddedLayout(0, tailDrop(m), 0, 0)

	if m.mine {
		m.bg.fill = theme.ColorForWidget(ColorNameLocalMessageBackground, m)
		m.top.Layout = layout.NewBorderLayout(nil, nil, nil, m.sender)
		m.container.Layout = layout.NewBorderLayout(m.top, nil, m.spacer, nil)
		return
	}

	m.bg.fill = theme.ColorForWidget(ColorNameRemoteMessageBackground, m)
	m.top.Layout = layout.NewBorderLayout(nil, nil, m.sender, nil)
	m.container.Layout = layout.NewBorderLayout(m.top, nil, nil, m.spacer)
}

func (m *messageItem) setMessage(id widget.ListItemID, msg Message) {
	m.text.Selectable = m.c.Selectable
	m.mine = msg.IsMine()

	text, ready := m.c.messageText(id, msg)
	m.waiting = !ready
	if !ready {
		// an earlier message is still being setRevealed, so show nothing of this one
		m.bg.Hide()
		m.text.Hide()
		m.obj.Hide()
		m.Refresh()
		return
	}
	m.bg.Show()
	m.sender.SetText(msg.SenderName())

	if omsg, ok := msg.(ObjectMessage); ok {
		m.obj.Objects = []fyne.CanvasObject{omsg.Object()}
		m.obj.Show()
		m.text.Hide()
	} else {
		m.text.SetText(text)
		m.text.Show()
		m.obj.Hide()
	}

	m.applySide()
	m.Refresh()
}

func (m *messageItem) MinSize() fyne.Size {
	if m.waiting {
		return fyne.Size{}
	}

	return m.BaseWidget.MinSize()
}

func (m *messageItem) Refresh() {
	fn := m.hideNames
	if m.waiting || (fn != nil && fn()) {
		m.top.Hide()
	} else {
		m.top.Show()
	}

	m.bg.radius = m.Theme().Size(theme.SizeNamePadding)
	m.applySide()

	m.bg.Refresh()
	m.container.Refresh()
}

func (m *messageItem) CreateRenderer() fyne.WidgetRenderer {
	return widget.NewSimpleRenderer(m.container)
}
