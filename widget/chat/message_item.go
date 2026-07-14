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
	m.bg.radius = m.text.MinSize().Height / 6

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

func (m *messageItem) setMessage(msg Message) {
	m.text.Selectable = m.c.Selectable
	m.sender.SetText(msg.SenderName())

	if omsg, ok := msg.(ObjectMessage); ok {
		m.obj.Objects = []fyne.CanvasObject{omsg.Object()}
		m.obj.Show()
		m.text.Hide()
	} else {
		m.text.SetText(msg.Text())
		m.text.Show()
		m.obj.Hide()
	}

	m.mine = msg.IsMine()
	m.applySide()
	m.Refresh()
}

func (m *messageItem) Refresh() {
	if fn := m.hideNames; fn != nil && fn() {
		m.top.Hide()
	} else {
		m.top.Show()
	}

	// the theme may have changed, so re-measure the bubble
	m.bg.radius = m.text.MinSize().Height / 6
	m.applySide()

	m.bg.Refresh()
	m.container.Refresh()
}

func (m *messageItem) CreateRenderer() fyne.WidgetRenderer {
	return widget.NewSimpleRenderer(m.container)
}
