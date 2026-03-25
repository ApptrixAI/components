package chat

type listMessageSource struct {
	msgs []Message
}

func NewMessageSourceFromList(msgs []Message) *listMessageSource {
	return &listMessageSource{msgs}
}

func (src *listMessageSource) Length() int {
	return len(src.msgs)
}

func (src *listMessageSource) GetMessage(n int) Message {
	if n < 0 || n >= len(src.msgs) {
		return nil
	}
	return src.msgs[n]
}

func (src *listMessageSource) Append(msg Message) {
	src.msgs = append(src.msgs, msg)
}
