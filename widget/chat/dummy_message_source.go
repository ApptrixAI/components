package chat

import (
	"time"
)

type dummyMessageSource struct {
	msgs    []Message
}

var dummyMessages = []Message{
	NewMessage("msg-1", "sndr-1", "Dummy", "foo", time.Date(2026, time.February, 1, 12, 0, 0, 0, time.UTC)),
	NewMessage("msg-2", "sndr-1", "Dummy", "bar", time.Date(2026, time.February, 1, 12, 1, 0, 0, time.UTC)),
	NewMessage("msg-3", "sndr-1", "Dummy", "baz", time.Date(2026, time.February, 1, 12, 2, 0, 0, time.UTC)),
}

func NewDummyMessageSource() *dummyMessageSource {
	return &dummyMessageSource{dummyMessages}
}

func (src *dummyMessageSource) Length() int {
	return len(src.msgs)
}

func (src *dummyMessageSource) GetItem(n int) Message {
	if n < 0 || n >= len(src.msgs) {
		return nil
	}
	return src.msgs[n]
}

func (src *dummyMessageSource) Send(msg Message) {
	src.msgs = append(src.msgs, msg)
}
