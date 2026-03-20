package chat

import (
	"fmt"
	"time"
)

var dummyMessageTexts = []string{
	"foo",
	"bar",
	"baz",
	"lalala",
	"moo",
	"pew",
	"mew",
	"lalelu",
	"woop",
	"dee",
	"doo",
	"jajaja",
	"meep",
	"beep",
	"woosh",
}

func NewDummyMessageSource(n int) *listMessageSource {
	var msgs []Message
	for i := 0; i < n; i++ {
		msgs = append(msgs, NewMessage(
			fmt.Sprintf("msg-%d", i+1),
			"sndr-1",
			"Dummy",
			dummyMessageTexts[i%len(dummyMessageTexts)],
			time.Date(2026, time.February, 1, 12, i, 0, 0, time.UTC),
			i % 3 == 0,
		))
	}
	return NewMessageSourceFromList(msgs)
}
