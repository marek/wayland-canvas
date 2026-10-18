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
- [x] Canvas items removed, in one pass (nothing uses them now that notes draw themselves):
  - `src/canvas/items.{h,cpp}` deleted, with `Items items` off `Server` and the file off `meson.build`
  - `src/render/render.cpp`: `draw_items`, `item_texture`, the `ItemSlot` cache and its sweep
  - `src/input/input.cpp`: the item hit in `pointer_hit_test`, `item_press`, the double-click state; `src/input/grab.cpp`: `ItemDrag` and `begin_item`
  - `src/platform/ipc.cpp`: `item.set`, `item.remove`, `item.activate`, the `items` array in the state, the `item.moved` and `item.activated` events
  - `tests/scenarios/ipc.cpp`: its canvas-items phase, and `tests/scenarios/bar.cpp`'s leftover `NoteEntry`/`state_notes` helpers that read items
  - `docs/intents/platform/ipc.md` (the methods, the state row, the Canvas items section), `docs/intents/shell/notes.md`'s cross-link, and `docs/intents/shell/plugins.md` where items are named
- [ ] On screen a note hovers at the centre and cannot be used, in a fresh session too. Two things are in against it now: the compositor remembers a placement by name and applies it when the surface maps (so the ask can no longer race the surface or a shell reload), and a note that is still unplaced after six seconds logs the compositor's answer and hides itself instead of floating. Try it again and read that line: it names whether the call was refused, and why

## What was tried for the unusable note, and what it showed

Everything below is in the tree; none of it made a note usable on screen.

- The surface is created, mapped and visible: a real card appears. It is
  not placed on the plane, so the compositor treats it as screen
  furniture — centred, and hit-tested where it is not.
- The name was built from `api.canvas.surfaceName(...)`, so the layer
  surface could be created before its namespace existed. It is now built
  from the note's id alone, as the surface is created.
- `surface.place` now records the rectangle under the surface's name and
  applies it when a surface of that name maps, so an early ask cannot be
  lost, and a shell reload finds its places again.
- A surface whose name begins with the canvas prefix is never given a
  place on a screen: unplaced, it is configured to its own size so it can
  map and be found, and nothing of it is drawn.
- The plugin logs the compositor's answer after three refused asks, and
  names an older compositor when that is the reason.
- Growing works inside the plugin and is proven by its own log: typing
  takes a note from 220 px to 326 px and `Notes.resized` writes it. What
  the scenario reads back from the compositor stays 220 px, so the second
  and later `surface.place` calls are not reaching it or not being
  applied — the first placement lands, the updates do not.
- `refresh()` copies the notes map, because assigning the same object
  tells QML nothing; `onNoteChanged` re-arms the placement retry.

Where to look next, in order:

1. Log every `surface.place` in the compositor (name, rectangle, whether a
   surface of that name was found) and watch a note grow: that says at
   once whether the later asks arrive.
2. If they arrive, follow `arrange_layers` for a placed surface — the
   configure it sends and whether the client commits that size — and the
   drawn box in `draw_canvas_surfaces`.
3. If they do not arrive, the fault is in the plugin's call path:
   `onNoteChanged` firing, `api.canvas.place` reaching `Compositor.call`,
   and the socket's answer.
4. Only then the pointer: a placed surface is hit-tested by
   `Output::canvas_surface_at` through `shell_surface_at`; a note that is
   placed but still inert points there.
