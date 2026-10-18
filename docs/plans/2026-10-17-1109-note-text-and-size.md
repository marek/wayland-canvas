# Notes: formatted text and a resizable card

Date: 2026-10-17 11:09 EDT

## Context

A sticky note today holds one block of plain text at one size, in one
font, and its card is whatever size it was created at. This work gives a
note the text controls a writing surface needs and a card the user can
size by hand.

The result of this work:

- Text in a note can be bold, italic or underlined, at a chosen size and
  in a chosen font; the strip offers every font family the machine has.
- Each note is a file of its own in the plugin's storage, written as plain
  HTML, so a note can be read, edited or kept outside the shell.
- Bold, italic and underline apply to the selection: a span of a note's
  text carries its own weight, slant and underline, and text typed inside
  it continues it. With nothing selected the choice applies to what is
  typed next.
- The size and the font belong to the note, not to a span: one note is
  written in one font at one size, and changing either changes the whole
  card.
- The controls live on the note's own strip, beside the colour dots: the
  same bar that recolours a note chooses its font and its size.
- A note's formatting is kept with the note — in the canvas item's state
  and in the plugin's storage — so it survives a shell reload, a restart
  and the compositor's own drawing of the card.
- A note is resized by dragging a corner of its card, at any zoom, and the
  new size is kept the same way. The text re-wraps to the new width.

Open questions for the owner of [shell/notes.md](../intents/shell/notes.md)
and [platform/ipc.md](../intents/platform/ipc.md): what the formatting
looks like on the wire (the item's text as a marked-up string, or runs
with attributes), and which fonts are offered (the system's families or a
short list). The editing surface is settled as a plugin surface placed in
canvas space, which
[2026-10-17-1108-note-editing-in-place.md](2026-10-17-1108-note-editing-in-place.md)
settles.

## Tasks

- [x] Settled: a note is its own file in the plugin's storage, written as plain HTML, and the strip offers every font family installed on the machine

The rest — the intent, the text controls, the resize by corner, the
scenarios and the look on screen — is carried on in
[2026-10-18-1130-notes-end-to-end.md](2026-10-18-1130-notes-end-to-end.md),
for a note that is one surface the plugin draws: the card is the
plugin's, and the compositor draws nothing of it.
