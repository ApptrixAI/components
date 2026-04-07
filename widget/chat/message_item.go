package chat

import (
	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/canvas"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/layout"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"
)

type messageItem struct {
	widget.BaseWidget
	container *fyne.Container
	top       *fyne.Container
	sender    *widget.Label
	text      *widget.Label
	bg        *canvas.Rectangle
	spacer    fyne.CanvasObject
	obj       *fyne.Container
	hideName  bool
}

func newMessageItem() *messageItem {
	m := &messageItem{}
	m.ExtendBaseWidget(m)

	m.sender = widget.NewLabel("")
	m.sender.SizeName = theme.SizeNameCaptionText
	m.text = widget.NewLabel("")
	m.text.Wrapping = fyne.TextWrapWord
	m.text.Selectable = true

	m.bg = &canvas.Rectangle{}
	m.bg.FillColor = theme.ColorForWidget(ColorNameRemoteMessageBackground, m)
	m.bg.CornerRadius = m.text.MinSize().Height / 6

	m.obj = container.NewPadded()
	m.obj.Hide()

	m.spacer = widget.NewLabel("")

	m.top = container.NewBorder(
		nil, nil,
		m.sender, nil,
	)
	m.container = container.NewBorder(
		m.top, nil,
		nil, m.spacer,
		container.NewStack(
			m.bg,
			m.obj,
			m.text,
		),
	)

	return m
}

func (m *messageItem) setMessage(msg Message) {
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

	if msg.IsMine() {
		m.bg.FillColor = theme.ColorForWidget(ColorNameLocalMessageBackground, m)
		m.top.Layout = layout.NewBorderLayout(nil, nil, nil, m.sender)
		m.container.Layout = layout.NewBorderLayout(m.top, nil, m.spacer, nil)
	} else {
		m.bg.FillColor = theme.ColorForWidget(ColorNameRemoteMessageBackground, m)
		m.top.Layout = layout.NewBorderLayout(nil, nil, m.sender, nil)
		m.container.Layout = layout.NewBorderLayout(m.top, nil, nil, m.spacer)
	}
	if m.hideName {
		m.top.Hide()
	} else {
		m.top.Show()
	}
	m.bg.Refresh()
	m.container.Refresh()
}

func (m *messageItem) CreateRenderer() fyne.WidgetRenderer {
	return widget.NewSimpleRenderer(m.container)
}
