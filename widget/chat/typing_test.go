package chat

import (
	"testing"
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/test"
	"fyne.io/fyne/v2/widget"
)

func testMessage(id, text string, mine bool) Message {
	return NewMessage(id, "sndr", "Sender", text, time.Now(), mine)
}

func testObjectMessage(id string) Message {
	return NewObjectMessage(id, "sndr", "Sender", widget.NewLabel("thing"), time.Now(), false)
}

// newTestChat returns a chat holding the given messages, all of which are treated
// as history, so that messages appended later are the ones that get typed out.
func newTestChat(msgs ...Message) (*Chat, *listMessageSource) {
	src := NewMessageSourceFromList(msgs)
	c := New(src, func(string) {})
	c.TypeIncoming = true
	c.CreateRenderer() // records how much history there is, as showing the chat would

	return c, src
}

// typing puts the chat into the state of being part way through revealing the
// message in the given position, without relying on a driver to run the animation.
func (c *Chat) typing(id widget.ListItemID, msg Message, shown int) {
	c.typer = &typewriter{
		index: id,
		text:  msg.Text(),
		runes: []rune(msg.Text()),
		shown: shown,
		anim:  fyne.NewAnimation(time.Second, func(float32) {}),
	}
}

func TestMessageText_offByDefault(t *testing.T) {
	test.NewApp()

	c, _ := newTestChat()
	c.TypeIncoming = false

	text, ready := c.messageText(0, testMessage("a", "hello", false))

	if text != "hello" || !ready {
		t.Errorf("expected the whole message right away, got %q (ready %v)", text, ready)
	}
	if c.typer != nil {
		t.Error("expected no message to be typed out when TypeIncoming is unset")
	}
}

func TestMessageText_notTyped(t *testing.T) {
	test.NewApp()

	history := testMessage("old", "was here before", false)

	for _, tt := range []struct {
		name string
		id   widget.ListItemID
		msg  Message
	}{
		{"history", 0, history},
		{"mine", 1, testMessage("mine", "sent by us", true)},
		{"object", 1, testObjectMessage("obj")},
		{"typing indicator", 1, testMessage("pending", ".", false)},
		{"longer typing indicator", 1, testMessage("pending", "...", false)},
	} {
		t.Run(tt.name, func(t *testing.T) {
			c, _ := newTestChat(history)

			text, ready := c.messageText(tt.id, tt.msg)

			if text != tt.msg.Text() || !ready {
				t.Errorf("expected the whole message right away, got %q (ready %v)", text, ready)
			}
			if c.typer != nil {
				t.Error("expected the message not to be typed out")
			}
		})
	}
}

func TestMessageText_typesIncoming(t *testing.T) {
	test.NewApp() // the test driver runs animations straight to the end

	c, _ := newTestChat(testMessage("old", "was here before", false))

	text, ready := c.messageText(1, testMessage("new", "hello", false))

	if text != "hello" || !ready {
		t.Errorf("expected the setRevealed message, got %q (ready %v)", text, ready)
	}
	if c.typer != nil {
		t.Error("expected the reveal to have finished")
	}
	if c.shown != 2 || c.lastRevealedText != "hello" {
		t.Errorf("expected the message to be recorded as setRevealed, got %d %q", c.shown, c.lastRevealedText)
	}
}

func TestMessageText_partway(t *testing.T) {
	test.NewApp()

	msg := testMessage("new", "hello", false)
	c, _ := newTestChat()
	c.typing(0, msg, 3)

	text, ready := c.messageText(0, msg)

	if text != "hel" || !ready {
		t.Errorf("expected the setRevealed part of the message, got %q (ready %v)", text, ready)
	}
}

// A message, or an object, that arrives while an earlier message is still being
// setRevealed has to wait for it - but the messages above it carry on showing.
func TestMessageText_waitsForEarlier(t *testing.T) {
	test.NewApp()

	c, _ := newTestChat(testMessage("old", "was here before", false))
	c.typing(1, testMessage("new", "hello", false), 3)

	for _, tt := range []struct {
		name string
		msg  Message
	}{
		{"message", testMessage("next", "and another", false)},
		{"object", testObjectMessage("obj")},
	} {
		t.Run(tt.name, func(t *testing.T) {
			text, ready := c.messageText(2, tt.msg)

			if ready {
				t.Error("expected the message to wait for the one being setRevealed")
			}
			if text != "" {
				t.Errorf("expected nothing to show while waiting, got %q", text)
			}
		})
	}

	text, ready := c.messageText(0, testMessage("old", "was here before", false))
	if text != "was here before" || !ready {
		t.Errorf("expected earlier messages to keep showing, got %q (ready %v)", text, ready)
	}
}

// Sources re-use ids, most obviously when a typing indicator becomes the message
// it announced, so the id of a message must not decide how it is setRevealed.
func TestMessageText_repeatedIds(t *testing.T) {
	test.NewApp() // the test driver runs animations straight to the end

	c, src := newTestChat()

	src.Append(testMessage("msg", "first message", false))
	if text, _ := c.messageText(0, src.GetMessage(0)); text != "first message" {
		t.Fatalf("expected the first message to be setRevealed, got %q", text)
	}

	src.Append(testMessage("msg", "second message", false))
	if text, _ := c.messageText(1, src.GetMessage(1)); text != "second message" {
		t.Fatalf("expected the second message to be setRevealed, got %q", text)
	}

	// the list re-draws the earlier message, which shares an id with the latest
	text, ready := c.messageText(0, src.GetMessage(0))
	if text != "first message" || !ready {
		t.Errorf("expected the earlier message to still show in full, got %q (ready %v)", text, ready)
	}
}

// A pending message, such as a typing indicator, is replaced in place by the real
// message - which is new text and so must be typed out.
func TestMessageText_textReplaced(t *testing.T) {
	test.NewApp() // the test driver runs animations straight to the end

	c, _ := newTestChat()

	if text, _ := c.messageText(0, testMessage("msg", "...", false)); text != "..." {
		t.Fatalf("expected the pending message to be setRevealed, got %q", text)
	}

	text, ready := c.messageText(0, testMessage("msg", "the real message", false))
	if text != "the real message" || !ready {
		t.Errorf("expected the new text to be setRevealed, got %q (ready %v)", text, ready)
	}
}
