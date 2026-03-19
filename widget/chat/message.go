package chat

import (
	"time"
)

type Message interface {
	Id() string
	SenderId() string
	SenderName() string
	Text() string
	Time() time.Time
	Owned() bool
}

type genericMessage struct {
	id         string
	senderId   string
	senderName string
	text       string
	time       time.Time
	owned      bool
}

func NewMessage(id, senderId, senderName, text string, time time.Time, owned bool) *genericMessage {
	return &genericMessage{id, senderId, senderName, text, time, owned}
}

func (msg *genericMessage) Id() string {
	return msg.id
}

func (msg *genericMessage) SenderId() string {
	return msg.senderId
}

func (msg *genericMessage) SenderName() string {
	return msg.senderName
}

func (msg *genericMessage) Text() string {
	return msg.text
}

func (msg *genericMessage) Time() time.Time {
	return msg.time
}

func (msg *genericMessage) Owned() bool {
	return msg.owned
}
