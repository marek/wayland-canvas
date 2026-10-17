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

Settled: the editor is a client surface of the plugin's own, placed in
canvas space by the compositor. The compositor gains one generic
capability — a surface a plugin asks to be put at a canvas rectangle,
which then pans, zooms and scales with everything else on the plane — and
knows nothing about notes. Sticky notes stay a plugin that can be written,
changed or replaced without rebuilding the compositor, the editing surface
is the real thing the user clicked rather than a copy laid over it, and
the motion is the canvas's own rather than a position pushed frame by
frame from outside.

## Tasks

- [ ] Prove it headlessly: a note typed in, then a zoom and a pan, with one card throughout and the text where it was
- [x] Intent: the canvas-space surface a plugin can ask for — the rectangle it names, what a camera move does to it, how it takes input and the keyboard — in `platform/ipc.md` and `shell/plugins.md`
- [x] Compositor: that surface role, generic to any plugin: placed at its canvas rectangle, drawn with the canvas, hit-tested at any zoom (`surface.place`, `surface.unplace`, and each output's canvas rectangle in the state)
- [x] Shell: the notes editor becomes one of those surfaces instead of a screen-space overlay — a note is now one surface, written in where it is, with no editor over it and no canvas item behind it
- [ ] The bar scenario's notes phase is written for the old model (canvas items and an editor surface) and fails until it is rewritten for the surface model: the note surface by its name, typing straight into it, the strip's dots and ×, the storage, and one card through a zoom and a pan
- [ ] Intent: notes.md's cross-links and `platform/ipc.md`'s "one user" line still point at canvas items as the notes' mechanism
- [ ] Intent: notes.md states that editing happens on the note itself, and what a camera move does while a note is open

- [ ] On screen, nested: open a note, zoom in and out, pan, change desktop scale — one card throughout (the user)
