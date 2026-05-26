package chat

import (
	"math"
	"strings"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/driver/desktop"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"
)

// input is the chat composer. It starts as a single visible row and
// grows to the traditional 3-row MultiLine height once the text spans
// more than one visible line (via an explicit newline or by wrapping
// past the entry's content width).
//
// Return submits while the text is still on a single line. Once the
// entry has grown to multiple lines the user is in compose mode:
// Return inserts a newline, and Shift+Return submits.
type input struct {
	widget.Entry
	onSubmit           func()
	onResize           func()
	shiftDown          bool
	rowsSet            int
	suppressNextReturn bool
}

func newInput(submit, resize func()) *input {
	i := &input{onSubmit: submit, onResize: resize, rowsSet: 1}
	i.MultiLine = true
	i.Wrapping = fyne.TextWrapWord
	i.ExtendBaseWidget(i)
	i.SetMinRowsVisible(1)
	return i
}

// computeRows estimates the number of visible rows the current text
// occupies, counting both explicit newlines and word wrapping against
// the entry's content width. Returns at least 1.
func (i *input) computeRows() int {
	text := i.Text
	if text == "" {
		return 1
	}

	th := i.Theme()
	innerPad := th.Size(theme.SizeNameInnerPadding)
	available := i.Size().Width - innerPad*2
	if available <= 0 {
		// Not laid out yet — fall back to explicit newline count.
		return strings.Count(text, "\n") + 1
	}

	textSize := th.Size(theme.SizeNameText)
	total := 0
	for _, line := range strings.Split(text, "\n") {
		if line == "" {
			total++
			continue
		}
		m := fyne.MeasureText(line, textSize, i.TextStyle)
		wrapped := int(math.Ceil(float64(m.Width / available)))
		if wrapped < 1 {
			wrapped = 1
		}
		total += wrapped
	}
	return total
}

// updateRowsVisible flips SetMinRowsVisible between 1 (single-line
// state) and 3 (traditional MultiLine height) based on the current
// row count. Returns true when the row count just changed, so the
// caller can ask the surrounding layout to re-run.
func (i *input) updateRowsVisible() bool {
	target := 1
	if i.computeRows() > 1 {
		target = 3
	}
	if i.rowsSet == target {
		return false
	}
	i.SetMinRowsVisible(target)
	i.rowsSet = target
	return true
}

// Refresh re-evaluates the row count on every text/state change.
// We delegate to Entry.Refresh so that Entry clears the internal MinSize cache.
func (i *input) Refresh() {
	changed := i.updateRowsVisible()
	i.Entry.Refresh()
	if changed && i.onResize != nil {
		i.onResize()
	}
}

// shouldSubmit reports whether the current key event should submit.
// Single-line text submits on plain Return; multi-line text submits
// on Shift+Return so plain Return can insert a newline in compose
// mode.
func (i *input) shouldSubmit() bool {
	if i.computeRows() > 1 {
		return i.shiftDown
	}
	return !i.shiftDown
}

// KeyDown intercepts Return to submit and tracks Shift state for the
// submit/newline decision. Non-Return keys fall through.
func (i *input) KeyDown(key *fyne.KeyEvent) {
	switch key.Name {
	case desktop.KeyShiftLeft, desktop.KeyShiftRight:
		i.shiftDown = true
		i.Entry.KeyDown(key)
		return
	case fyne.KeyReturn, fyne.KeyEnter:
		if i.shouldSubmit() {
			if i.onSubmit != nil {
				i.onSubmit()
			}
			i.suppressNextReturn = true
			return
		}
	}
	i.Entry.KeyDown(key)
}

// KeyUp keeps shift-state tracking honest.
func (i *input) KeyUp(key *fyne.KeyEvent) {
	if key.Name == desktop.KeyShiftLeft || key.Name == desktop.KeyShiftRight {
		i.shiftDown = false
	}
	i.Entry.KeyUp(key)
}

// TypedKey swallows a Return event that KeyDown has already turned
// into a submit, so the base Entry does not also insert a newline at
// the cursor afterwards. A Return that did NOT submit (multi-line
// compose, no shift) falls through to the underlying Entry so it
// inserts a newline as normal.
func (i *input) TypedKey(key *fyne.KeyEvent) {
	if key.Name == fyne.KeyReturn || key.Name == fyne.KeyEnter {
		if i.suppressNextReturn {
			i.suppressNextReturn = false
			return
		}
	}
	i.Entry.TypedKey(key)
	i.Refresh()
}
