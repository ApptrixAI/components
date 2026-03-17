package chat

import (
	"time"
)

type Message interface {
	Id() string
	SenderId() string
	SenderName() string
	Text() string
	SentAt() time.Time
}

type genericMessage struct {
	id         string
	senderId   string
	senderName string
	text       string
	sentAt     time.Time
}

func NewMessage(id, senderId, senderName, text string, sentAt time.Time) *genericMessage {
	return &genericMessage{id, senderId, senderName, text, sentAt}
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

func (msg *genericMessage) SentAt() time.Time {
	return msg.sentAt
}
