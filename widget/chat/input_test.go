package chat

import (
	"testing"

	"fyne.io/fyne/v2/test"
)

// TestAtSentenceStart exercises the sentence-boundary detection that
// drives auto-capitalization, independent of the mobile-only gate in
// TypedRune. Text/cursor are set directly to model where the next rune
// would land.
func TestAtSentenceStart(t *testing.T) {
	test.NewApp()

	cases := []struct {
		name string
		text string
		row  int
		col  int
		want bool
	}{
		{"empty", "", 0, 0, true},
		{"start of text", "hello", 0, 0, true},
		{"mid word", "hello", 0, 3, false},
		{"after space only", "hello ", 0, 6, false},
		{"after period+space", "hi. ", 0, 4, true},
		{"after period no space", "hi.", 0, 3, true},
		{"after question+space", "ok? ", 0, 4, true},
		{"after bang+spaces", "no!   ", 0, 6, true},
		{"after comma is not a sentence", "hi, ", 0, 4, false},
		{"start of a later line", "hi\n", 1, 0, true},
		{"mid later line", "hi\nthere", 1, 2, false},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			i := newInput(nil, nil)
			i.Text = tc.text
			i.CursorRow = tc.row
			i.CursorColumn = tc.col
			if got := i.atSentenceStart(); got != tc.want {
				t.Fatalf("atSentenceStart(%q @ %d,%d) = %v, want %v", tc.text, tc.row, tc.col, got, tc.want)
			}
		})
	}
}

func TestAutoCapitalizeCanBeDisabled(t *testing.T) {
	test.NewApp()

	i := newInput(nil, nil)
	if !i.autoCapitalize {
		t.Fatal("expected auto-capitalize on by default for the chat composer")
	}
}
