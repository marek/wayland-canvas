# Editing a note happens on the note, not over it

Date: 2026-10-17 11:08 EDT

## Context

A note opened for editing is edited where it sits. Today the open editor
is a separate overlay surface laid on top of the note, so the note is
drawn twice: zooming or panning moves the canvas card out from under the
overlay and a second copy of the note hovers beside it, and what the user
types into is not the thing they clicked.

The result of this work: there is one note on screen at every zoom, pan
and desktop scale. Opening a note for editing changes that one card — a
cursor in its text, the colour dots and the × on its strip — and every
camera move carries it along with the rest of the canvas. Nothing of the
editing state is drawn in screen coordinates.

The decision of where the editing surface lives (the canvas item the
compositor draws, a client surface placed on the canvas, or the overlay
anchored to the item's canvas box every frame) is open and belongs to the
owner of [shell/notes.md](../intents/shell/notes.md) and
[platform/ipc.md](../intents/platform/ipc.md).

## Tasks

- [ ] Reproduce headlessly: a note opened for editing, then a zoom and a pan, with the screenshots showing the duplicate card and the drift
- [ ] (the user to decide) Where the editing surface lives: an item the compositor draws and types into, a client surface placed in canvas space, or the present overlay re-anchored every frame
- [ ] Intent: notes.md states that editing happens on the note itself, and what a camera move does while a note is open
- [ ] Implement it, with the bar scenario's notes phase extended to zoom and pan while the editor is open
- [ ] On screen, nested: open a note, zoom in and out, pan, change desktop scale — one card throughout (the user)
