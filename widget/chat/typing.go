package chat

import (
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/widget"
)

// typeSpeed is how long each character takes to appear when TypeIncoming is set.
const typeSpeed = 25 * time.Millisecond

// typewriter tracks the character by character reveal of a single message.
type typewriter struct {
	index widget.ListItemID
	text  string
	runes []rune
	shown int

	anim *fyne.Animation
	// starting is set while the animation is being started, as some drivers will
	// run it to completion right away.
	starting bool
}

// messageText returns the text that should currently be drawn for the message at
// the given position. When TypeIncoming is set, a message that arrives from
// someone else is setRevealed a character at a time instead of appearing complete.
//
// The second return is false while a message is waiting for an earlier one to
// finish being setRevealed - such a message must not be drawn at all yet.
func (c *Chat) messageText(id widget.ListItemID, msg Message) (string, bool) {
	if !c.TypeIncoming {
		return msg.Text(), true
	}

	if msg.IsMine() {
		c.setRevealed(id, msg.Text()) // our own messages appear as soon as we send them
		return msg.Text(), true
	}

	if t := c.typer; t != nil {
		switch {
		case id > t.index:
			// this arrived behind a message that is still being setRevealed, so it waits its turn.
			return "", false
		case id < t.index:
			return msg.Text(), true // an earlier message, setRevealed already
		case t.text == msg.Text():
			return string(t.runes[:t.shown]), true
		}

		// the text in this position changed part way through revealing it, so start
		// again with the new text
		t.anim.Stop()
		c.typer = nil
	}

	if id < c.shown-1 || (id == c.shown-1 && msg.Text() == c.lastRevealedText) {
		return msg.Text(), true // setRevealed already, do not type it out again
	}

	if msg.Text() == "" || isTypingIndicator(msg.Text()) {
		c.setRevealed(id, msg.Text())
		return msg.Text(), true
	}

	t := c.startTyping(id, msg)
	return string(t.runes[:t.shown]), true
}

func isTypingIndicator(text string) bool {
	return text == "." || text == ".." || text == "..."
}

// setRevealed records that the message in the given position is now fully shown, so
// that the messages behind it can follow.
func (c *Chat) setRevealed(id widget.ListItemID, text string) {
	if id < c.shown-1 {
		return // an older message, we have moved on since
	}

	c.shown = id + 1
	c.lastRevealedText = text
}

// startTyping begins revealing the given message, one character at a time.
func (c *Chat) startTyping(id widget.ListItemID, msg Message) *typewriter {
	t := &typewriter{
		index: id,
		text:  msg.Text(),
		runes: []rune(msg.Text()),
	}
	c.typer = t

	t.anim = fyne.NewAnimation(time.Duration(len(t.runes))*typeSpeed, func(done float32) {
		shown := int(float32(len(t.runes)) * done)
		if shown == t.shown {
			return
		}
		t.shown = shown

		if shown == len(t.runes) {
			c.finishTyping(t)
			return
		}

		c.list.RefreshItem(t.index)
		c.list.ScrollToBottom()
	})
	t.anim.Curve = fyne.AnimationLinear

	// a driver may run the whole animation as soon as it starts, so record that we
	// are mid-update - the caller will draw whatever we reach
	t.starting = true
	t.anim.Start()
	t.starting = false

	return t
}

// finishTyping marks a message as fully setRevealed and refreshes the list so that
// anything held back behind it - such as an object message - can now appear.
func (c *Chat) finishTyping(t *typewriter) {
	if c.typer != t {
		return
	}

	c.typer = nil
	c.setRevealed(t.index, t.text)

	if t.starting {
		return // we are inside a list update, which will draw the completed text
	}

	c.list.Refresh()
	c.list.ScrollToBottom()
}
