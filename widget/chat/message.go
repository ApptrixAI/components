package chat

import (
	"time"

	"fyne.io/fyne/v2"
)

type Message interface {
	Id() string
	SenderId() string
	SenderName() string
	Text() string
	Time() time.Time
	IsMine() bool
}

type ObjectMessage interface {
	Message
	Object() fyne.CanvasObject
}

type genericMessage struct {
	id         string
	senderId   string
	senderName string
	text       string
	time       time.Time
	mine       bool
}

type objectMessage struct {
	genericMessage
	object fyne.CanvasObject
}

func NewMessage(id, senderId, senderName, text string, time time.Time, mine bool) *genericMessage {
	return &genericMessage{id, senderId, senderName, text, time, mine}
}

func NewObjectMessage(id, senderId, senderName string, object fyne.CanvasObject, time time.Time, mine bool) *objectMessage {
	return &objectMessage{genericMessage{id, senderId, senderName, "", time, mine}, object}
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

func (msg *genericMessage) IsMine() bool {
	return msg.mine
}

func (msg *objectMessage) Object() fyne.CanvasObject {
	return msg.object
}
