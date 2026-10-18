---
name: canvas-plugins
description: How to write a shell plugin whose surfaces live on the canvas (sticky notes, drawings) in wayland-canvas without the mistakes that made notes unusable on screen - state in QML objects whose properties notify, never plain JS objects in var bindings; windows made by Variants; the surface named as it is created and placed only through surface.place; every drag handed to the compositor with surface.move or surface.resize; and the result checked on the screenshot and on a second output, never only in storage. Use before writing or changing a plugin that draws on the plane, a QML model that surfaces bind to, or a scenario that checks one.
---

# Canvas plugins

A plugin that puts something on the plane draws it in a surface of its
own and lets the compositor place it
([docs/intents/platform/ipc.md](../../intents/platform/ipc.md), "Surfaces
on the canvas"). Sticky notes are the reference
([docs/intents/shell/notes.md](../../intents/shell/notes.md),
`plugins/canvas.notes/`). Each rule below is a way that plugin once
failed on screen while its storage looked right; the case study is
[docs/intents/bugs/notes-that-never-followed.md](../../intents/bugs/notes-that-never-followed.md).

## 1. State the surface binds to lives in objects that notify

QML does not see a change made inside a plain JS object, and a `var`
binding that re-evaluates to the same object signals nothing — a copied
map of the same entries is the same entries. A surface bound to
`notes[id].height` then never resizes, never re-places and never
recolours, while the file it is saved to changes.

- One `QtObject` (an inline `component`) per item, with real properties;
  change it in place (`note.height = 326`). Every binding on that
  property follows, nothing is copied, no `refresh()`.
- Rebuild a list only when items are added or removed (the ids a
  `Variants` follows).
- Prove it in `qml6`/`qmltestrunner` when unsure: an `onXChanged` that
  never fires is the whole bug.

## 2. Windows come from `Variants`, never from a `Repeater`

A `Repeater` makes items; a `PanelWindow` delegate earns "Delegate must
be of Item type" and a lifecycle nobody owns. `Variants { model: ids;
Surface {} }` creates and destroys one window per id. Remove an item
with `Qt.callLater` from inside its own surface's handler: the surface
goes with the id.

## 3. The name is set as the surface is made; only `surface.place` places it

- The layer-shell namespace `wayland-canvas-canvas-<plugin id>/<id>` is a
  literal built from the item's id, never from something that arrives
  later (`api`): a namespace set after creation never reaches the
  compositor.
- Bind one `rect` property to the item's rectangle and call
  `api.canvas.place` when it changes; the compositor remembers it by
  name, so an ask before the surface maps is not lost and needs no retry.
- Never position the surface any other way (margins, anchors): the
  compositor ignores them on the plane.

## 4. Every drag is the compositor's

A surface moved by placing it again from its own pointer events moves
the coordinates those events are measured in: the drag chases itself and
lags, and the pointer leaves the surface. On the press, call
`api.canvas.move(name, cb)` or `api.canvas.resize(name, {edges,
minWidth, minHeight}, cb)`; the compositor moves or sizes the surface
with its own pointer and answers with the rectangle at the release. Keep
a `dragging` flag: nothing places the surface until the answer.

## 5. Text, fonts and formatting

- A `TextArea` with `textFormat: TextEdit.RichText` holds per-span bold,
  italic and underline; `cursorSelection.font` sets them on a selection.
  With nothing selected `cursorSelection` takes the word under the caret,
  so "for what is typed next" is a pending format applied to the typed
  range by a `TextSelection` whose `document` is set before its range.
- Save plain HTML, not Qt's: keep `p`, `br`, `b`, `i`, `u` and drop every
  font, size and colour, so the item's own font and size stay the only
  ones. Load it back with `p { margin: 0; white-space: pre-wrap }` and
  empty paragraphs marked `-qt-paragraph-type:empty`, or the text grows a
  line per reload.
- `Keys.onPressed` does not swallow keys unless it accepts them; give the
  shortcut keys (`Ctrl+B`) `event.accepted = true`.

## 6. The keyboard

`WlrKeyboardFocus.OnDemand` gets the keyboard on map and on a click. To
give it back (Escape), set `None`; the compositor returns the keyboard to
the window, the surface's window goes inactive, and only then set
`OnDemand` again — on `Window.active` turning false, not on a timer.

## 7. Check where the user looks

- A scenario reads the item's file **and** the screenshot: a colour, a
  size, a move, a bold span are proven by pixels on the card, not by the
  storage alone.
- Hold a drag's button across steps — press, wait for
  `PointerGrab::SurfaceDrag`, travel, release — as a hand does: a press,
  ten motions and a release in one step reach the client before it can
  hand the drag over.
- A key sent in the same step as a click can reach the client first;
  give it a step of its own when order matters.
- Add a second output and aim its camera at the item: every output draws
  and hit-tests the plane.
- A helper the compiler calls unused may be a test phase that went
  missing: find the commit that dropped its caller before deleting it.

## Before shipping

- Does every visible property bind to a notifying property?
- Is any surface positioned by anything but `surface.place`, or dragged
  by anything but the compositor?
- Does a scenario check the change on the screenshot, at a zoom other
  than 100 %, and on a second output?
