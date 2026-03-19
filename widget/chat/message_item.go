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
	height    float32
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
	m.bg.FillColor = theme.Color(ColorNameRemoteMessageBackground)
	m.bg.CornerRadius = m.text.MinSize().Height / 6

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
			m.text,
		),
	)

	return m
}

func (m *messageItem) SetSender(s string) {
	m.sender.SetText(s)
}

func (m *messageItem) SetOwned(o bool) {
	if o {
		m.bg.FillColor = theme.Color(ColorNameLocalMessageBackground)
		m.top.Layout = layout.NewBorderLayout(nil, nil, nil, m.sender)
		m.container.Layout = layout.NewBorderLayout(m.top, nil, m.spacer, nil)
	} else {
		m.bg.FillColor = theme.Color(ColorNameRemoteMessageBackground)
		m.top.Layout = layout.NewBorderLayout(nil, nil, m.sender, nil)
		m.container.Layout = layout.NewBorderLayout(m.top, nil, nil, m.spacer)
	}
	m.text.Refresh()
	m.bg.Refresh()
	m.container.Refresh()
}

func (m *messageItem) SetText(s string) {
	m.text.Text = s
	m.text.Refresh()
}

func (m *messageItem) CreateRenderer() fyne.WidgetRenderer {
	return widget.NewSimpleRenderer(m.container)
}
