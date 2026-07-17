package chat

import (
	"image/color"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/canvas"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"
)

// bubble is the speech bubble drawn behind a chat message.
// The body is a rounded rectangle, with a tail flaring out of its bottom edge
// down to a point on the side the message came from - left for a remote sender
// and right for our own messages.
type bubble struct {
	widget.BaseWidget

	fill   color.Color
	radius float32
	mine   bool

	shape *canvas.ArbitraryPolygon
}

func newBubble() *bubble {
	b := &bubble{shape: canvas.NewArbitraryPolygon(nil, color.Transparent)}
	b.ExtendBaseWidget(b)

	return b
}

// tailDrop is how far the tail hangs below the body of the bubble.
// Message content must be inset by this much so it does not sit over the tail.
func tailDrop(w fyne.Widget) float32 {
	return theme.SizeForWidget(theme.SizeNamePadding, w) * 1.5
}

// tailBase is how much of the bubble's bottom edge the tail flares out from.
func tailBase(w fyne.Widget) float32 {
	return theme.SizeForWidget(theme.SizeNamePadding, w) * 2
}

// points returns the bubble outline, clockwise from the top left, along with
// the rounding to apply at each vertex.
func (b *bubble) points(s fyne.Size) ([]fyne.Position, []float32) {
	w, h := s.Width, s.Height
	drop := fyne.Min(tailDrop(b), h)
	base := fyne.Min(tailBase(b), w)
	body := h - drop

	// the point is only just softened, rounding it would blunt the tail
	tip := fyne.Min(b.radius/4, 1)

	if b.mine {
		return []fyne.Position{
			{X: 0, Y: 0},
			{X: w, Y: 0},
			{X: w, Y: h},           // the point, hanging below the body
			{X: w - base, Y: body}, // the tail flares from here
			{X: 0, Y: body},
		}, []float32{b.radius, b.radius, tip, 0, b.radius}
	}

	return []fyne.Position{
		{X: 0, Y: 0},
		{X: w, Y: 0},
		{X: w, Y: body},
		{X: base, Y: body}, // the tail flares from here
		{X: 0, Y: h},       // the point, hanging below the body
	}, []float32{b.radius, b.radius, b.radius, 0, tip}
}

func (b *bubble) Refresh() {
	b.shape.FillColor = b.fill
	b.BaseWidget.Refresh()
}

func (b *bubble) CreateRenderer() fyne.WidgetRenderer {
	b.shape.FillColor = b.fill

	return &bubbleRenderer{b: b}
}

type bubbleRenderer struct {
	b *bubble
}

func (r *bubbleRenderer) Layout(s fyne.Size) {
	r.b.shape.Points, r.b.shape.CornerRadii = r.b.points(s)
	r.b.shape.Resize(s)
	r.b.shape.Refresh()
}

func (r *bubbleRenderer) MinSize() fyne.Size {
	return fyne.NewSize(tailDrop(r.b)+r.b.radius*2, tailBase(r.b)+r.b.radius)
}

func (r *bubbleRenderer) Objects() []fyne.CanvasObject {
	return []fyne.CanvasObject{r.b.shape}
}

func (r *bubbleRenderer) Refresh() {
	r.b.shape.FillColor = r.b.fill
	r.Layout(r.b.Size())
}

func (r *bubbleRenderer) Destroy() {
}
