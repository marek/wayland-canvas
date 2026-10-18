# Sticky notes that work on screen, end to end

Date: 2026-10-18 11:30 EDT

## Context

A sticky note is usable on screen. It sits on the plane where it was put
and pans, zooms and scales with the canvas on every output; it is typed
into where it sits and grows as text is added; it is dragged by its strip
and resized from its bottom-right corner. Its strip's colour dots recolour
it, its × deletes it, and the same strip chooses the note's font — any
family installed on the machine. Bold, italic, underline and the text
size apply to the selected span, the size set with − and + around a
number that can also be typed. Each note is a plain-HTML file of its own
in the plugin's storage.

A note stays one surface the plugin draws
(`plugins/canvas.notes/NoteSurface.qml`), placed on the plane by the
compositor through the generic canvas-surface role. The compositor draws
nothing of a plugin's content and gains no note-specific code.

Settled in this work:

- A note is one object with real properties, changed in place: a change
  reaches every binding on it — the surface's size, its placement, the
  card's colour — without copying the notes.
- A drag of a canvas surface is run by the compositor, as an interactive
  move or resize of a window is: the plugin asks on the press
  (`surface.move`, `surface.resize`), the compositor moves or sizes the
  surface with its own pointer until the release and answers with the
  rectangle. A drag driven by placing the surface again from its own
  pointer events races those events, since each placement moves the
  coordinates they are measured in.
- Every output draws and hit-tests the canvas surfaces its view shows,
  whichever output the surface was created on.

The contracts are [../intents/shell/notes.md](../intents/shell/notes.md),
[../intents/platform/ipc.md](../intents/platform/ipc.md) and
[../intents/shell/plugins.md](../intents/shell/plugins.md). This plan
continues [2026-10-17-1108-note-editing-in-place.md](2026-10-17-1108-note-editing-in-place.md)
(whose findings it starts from) and takes over the open tasks of
[2026-10-17-1109-note-text-and-size.md](2026-10-17-1109-note-text-and-size.md).

## What was found

- Every `surface.place` the compositor receives, logged headless and in a
  nested run while a note is typed into: one per note, 220 × 220, asked
  before the surface exists and applied when it maps. The plugin grows the
  note to 326 px in its storage; no second placement is ever sent.
- The plugin re-made its notes map by a shallow copy, so a surface's
  `note` binding came back as the same object, and QML signals no change
  for that: the surface's size, its placement and the card's colour never
  followed a change, while storage did. Growing, the strip drag, the
  corner and the colour dots all changed only the file.
- At the starting point the bar scenario fails three notes checks (the
  growth, its kept height, the corner) and, now and then, a reload check
  that tells a new bar surface from the old one by pointer and protocol
  id, both of which are reused.

## Tasks

- [x] Reproduce: each `surface.place` logged with its name, rectangle and whether the surface was found, headless and nested on screen, while a note is created from the overview's note action and typed into
- [x] Intent: notes.md gains the text model (per-span weight, slant and underline; the note's own font and size), the strip's controls, growth, the move by the strip, the resize by the corner and the HTML file per note; ipc.md gains `surface.move` and `surface.resize`, every output drawing a canvas surface, and loses the state's stale `items` and the contradicting error row of `surface.place`; plugins.md's storage gains text files, a listing and removal
- [x] Plugin: one object per note with real properties, changed in place; the surfaces made by `Variants`; a note grows to hold its text
- [x] Compositor: `surface.move` and `surface.resize` as pointer grabs answered at the release; canvas surfaces drawn and hit-tested on every output and kept when their output goes; no configure before a surface's first commit; a surface drawn at the size it committed
- [x] Plugin: the strip drags the note and the bottom-right corner sizes it, through those methods, at any zoom; the text re-wraps and the size is kept
- [x] Text model: bold, italic and underline on the strip, applied to the selection and continued by typing inside a span; the font of the whole note on the same strip, every installed family offered
- [x] Storage: each note its own plain-HTML file, read back on start and after a reload
- [x] Scenario: the bar scenario's notes phase checks the screen — the card recoloured, grown, moved by its strip, sized by its corner at 100 % and at a scaled desktop, a bold span and a plain one in the file and on the screenshot, the × — and two outputs drawing one note
- [x] The reload check tells a new bar surface by something that is never reused
- [x] Every build and link warning fixed
- [x] Found here: a plan commit had dropped the bar scenario's toolbar-edges phase and the jump out of it, so the run ended after the recording dot and the settings window, app bar and capture-with-a-panel phases no longer ran; the phase and the helpers deleted as unused since are back, and every phase runs and passes
- [x] Found here: the canvas-items removal had dropped the ipc scenario's state request, so its window-list check read nothing and failed; the request is back, beside checks of the surface role's own answers
- [x] `tools/check.sh` green, merged into main
- [x] On screen, nested: a note created, typed into, grown, panned and zoomed, driven by keys and the socket
- [x] Once verified: a skill for writing canvas plugins without these mistakes, and a case study of this hunt in `docs/intents/bugs/`
- [ ] On screen, nested, by hand: drag a note by its strip, size it by its corner, recolour it, format a selection, change its font and size, delete it (the user)
- [x] Text size per span, as bold is: on the selection, or with nothing selected on what is typed next; kept as the span's `font-size` in the file, the note's base size the rest's
- [x] The size control on the strip: − and + step the size at the caret or of the selection by a pixel, and the number between them can be typed into, Enter applying it
- [x] A button held on a client surface keeps the pointer on it until the release, outside its bounds too (Wayland's implicit grab), so a drag that leaves the surface — a text selection past a window's or a note's edge, a slider — still reaches it; drag-and-drop and the compositor's own grabs keep priority. Found while fixing notes, wider than them
- [x] Found here: the strip read its format from the character before the cursor, outside a selection made backwards, so it showed B off for a bold word and + stepped from the wrong size; the strip reads the selection's own characters, and − and + step each part from its own size
- [x] `tools/check.sh` green with the per-span size and the held press, merged into main
