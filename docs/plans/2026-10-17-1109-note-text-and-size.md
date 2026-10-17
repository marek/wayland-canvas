# Notes: formatted text and a resizable card

Date: 2026-10-17 11:09 EDT

## Context

A sticky note today holds one block of plain text at one size, in one
font, and its card is whatever size it was created at. This work gives a
note the text controls a writing surface needs and a card the user can
size by hand.

The result of this work:

- Text in a note can be bold, italic or underlined, at a chosen size and
  in a chosen font.
- Every one of those applies to the selection: a span of a note's text
  carries its own weight, slant, underline, size and font, and text typed
  inside it continues it. With nothing selected the choice applies to what
  is typed next.
- A note's formatting is kept with the note — in the canvas item's state
  and in the plugin's storage — so it survives a shell reload, a restart
  and the compositor's own drawing of the card.
- A note is resized by dragging a corner of its card, at any zoom, and the
  new size is kept the same way. The text re-wraps to the new width.

Open questions for the owner of [shell/notes.md](../intents/shell/notes.md)
and [platform/ipc.md](../intents/platform/ipc.md): what the formatting
looks like on the wire (the item's text as a marked-up string, or runs
with attributes), which fonts are offered (the system's families or a
short list), and where the controls live (the note's strip, a flyout from
it, or a panel). This work also depends on where the editing surface
lives, which
[2026-10-17-1108-note-editing-in-place.md](2026-10-17-1108-note-editing-in-place.md)
settles.

## Tasks

- [ ] (the user to decide) How a note's formatting is carried in the item state and the plugin's storage, and which fonts are offered
- [ ] (the user to decide) Where the controls sit: the note's strip, a flyout from it, or a panel
- [ ] Intent: notes.md gains the text model (spans and their attributes), the controls, and the resize by corner
- [ ] Shell: bold, italic, underline, size and font applied to the selection, continued by typing inside a span, and kept in the item and storage
- [ ] Compositor: the card drawn with the spans' attributes, so a note reads the same whether the editor is open or not
- [ ] Resize: a drag of a note's corner at any zoom sizes the card, the text re-wraps, and the size is kept
- [ ] Scenarios: a span made bold and a second left plain, read back from the item and the storage and seen on the screenshot; a corner drag at 100 % and at a scaled desktop
- [ ] On screen, nested: write in a note, format a selection, resize it by a corner (the user)
