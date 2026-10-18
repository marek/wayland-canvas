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

- [x] Prove it headlessly: a note typed in, then a pan, with one card throughout and the text where it was
- [x] Intent: the canvas-space surface a plugin can ask for — the rectangle it names, what a camera move does to it, how it takes input and the keyboard — in `platform/ipc.md` and `shell/plugins.md`
- [x] Compositor: that surface role, generic to any plugin: placed at its canvas rectangle, drawn with the canvas, hit-tested at any zoom (`surface.place`, `surface.unplace`, and each output's canvas rectangle in the state)
- [x] Shell: the notes editor becomes one of those surfaces instead of a screen-space overlay — a note is now one surface, written in where it is, with no editor over it and no canvas item behind it
- [x] The bar scenario's notes phase rewritten for the surface model: the note surface by its name, its canvas placement, the storage, one card through a pan, a reload, and the strip's controls
- [x] Keys and clicks reach a placed surface: the compositor's half (placement, hit test, keyboard focus) measured right, and the plugin's two faults fixed — an `id` named `text` shadowed the property in the typed handler, and the strip's drag area covered its own controls. Typing, the colour dots and a reload all pass now
- [x] The × on the strip removes the note: the click was landing all along, but `delete` on the map QML holds left the key behind, so the surface was never torn down; the map is rebuilt without it
- [ ] Canvas items removed, in one pass (nothing uses them now that notes draw themselves):
  - `src/canvas/items.{h,cpp}` deleted, with `Items items` off `Server` and the file off `meson.build`
  - `src/render/render.cpp`: `draw_items`, `item_texture`, the `ItemSlot` cache and its sweep
  - `src/input/input.cpp`: the item hit in `pointer_hit_test`, `item_press`, the double-click state; `src/input/grab.cpp`: `ItemDrag` and `begin_item`
  - `src/platform/ipc.cpp`: `item.set`, `item.remove`, `item.activate`, the `items` array in the state, the `item.moved` and `item.activated` events
  - `tests/scenarios/ipc.cpp`: its canvas-items phase, and `tests/scenarios/bar.cpp`'s leftover `NoteEntry`/`state_notes` helpers that read items
  - `docs/intents/platform/ipc.md` (the methods, the state row, the Canvas items section), `docs/intents/shell/notes.md`'s cross-link, and `docs/intents/shell/plugins.md` where items are named
- [ ] On screen a note hovers at the centre and cannot be used: the sign of a layer surface with no anchors that was never placed, so `surface.place` is failing or arriving before the surface exists in the session (it passes headlessly). Find it from the shell's log line for the failed call and the compositor's answer, then make a note that cannot be placed say so instead of floating
