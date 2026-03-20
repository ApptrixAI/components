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
		senderId := "sndr-1"
		senderName := "Dummy"
		isMine := i%3 == 0
		if isMine {
			senderId = "sndr-2"
			senderName = "Me"
		}
		msgs = append(msgs, NewMessage(
			fmt.Sprintf("msg-%d", i+1),
			senderId,
			senderName,
			dummyMessageTexts[i%len(dummyMessageTexts)],
			time.Date(2026, time.February, 1, 12, i, 0, 0, time.UTC),
			isMine,
		))
	}
	return NewMessageSourceFromList(msgs)
}
