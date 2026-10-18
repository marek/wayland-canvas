# Case study: the notes that never followed their own changes

> A bug case study, where history is told on purpose. The present-tense
> rules it produced are in [../shell/notes.md](../shell/notes.md),
> [../platform/ipc.md](../platform/ipc.md) and the `canvas-plugins`
> skill.

## Symptom

On screen a sticky note appeared as a real card in the middle of what the
overview showed, and then did nothing: it could not be moved, its colour
dots and its × had no effect that could be seen, and typed text ran on
below its bottom edge instead of growing it. Headless, most of the notes
checks passed.

## What was tried first

Several fixes went in and none changed what was seen, because each
assumed the compositor was not placing the surface:

- the surface's name built from the note's id as it is created, rather
  than from the plugin api that arrives later;
- a placement remembered by name and applied when the surface maps, so an
  early ask cannot be lost;
- an unplaced canvas surface never put on a screen;
- the map of notes copied on every change, so QML would see "a new map".

## The evidence

Logging every `surface.place` the compositor received, with the name, the
rectangle and whether a surface of that name existed, showed one call per
note — 220 × 220, before the surface existed, applied at its map — and no
other, while the plugin's storage grew the note to 326 px. The later asks
never left the plugin.

A two-line QML test then showed why: a `var` property bound to
`notes[id]` emits no change when the binding re-evaluates to the same
object, and a shallow copy of the map holds the same note objects. The
surface's `onNoteChanged`, which re-placed it, never ran; its size and
colour bindings never re-evaluated. Every change reached the file and
nothing else. The headless checks passed because they read the file.

## Root causes

1. The notes were plain JS objects in a `var` map: a change inside one
   notified nothing, and copying the map could not help.
2. The scenario checked storage, not the screen: a recolour, a move and a
   growth that changed only the file passed.
3. The drags were the plugin's: the strip moved the note by placing it
   again from pointer coordinates relative to the note itself, which the
   move then shifted, and the corner's pointer left the card it was
   growing. The scenario's drag sent its press, motions and release in
   one step, which no plugin can answer by handing the drag over.

Found on the way: the bar scenario had been ending early. A plan commit
had dropped the toolbar-edges phase with the jump out of it, so the run
went from the recording dot straight to its end, and the settings
window, app bar and capture-with-a-panel phases had stopped running; the
helpers the dropped phase used were then deleted as unused.

## Rules it produced

- A note is one object with notifying properties, changed in place
  ([../shell/notes.md](../shell/notes.md)).
- A drag of a canvas surface is the compositor's: `surface.move` and
  `surface.resize` from the press, as xdg-shell's interactive move and
  resize ([../platform/ipc.md](../platform/ipc.md)).
- Every output draws and hit-tests the canvas surfaces its view shows,
  and a surface outlives the output it was made on.
- A plugin's change is checked on the screenshot, at more than one zoom
  and on a second output, as well as in its storage.

## Playbook

1. When the screen and the storage disagree, log the boundary between
   them first — here every `surface.place` with its outcome — before
   changing either side.
2. A QML binding that should have re-run and did not: test the exact
   property in `qml6` before writing another workaround.
3. A second fix that does not change what is seen means the first theory
   was wrong; go back to evidence, not to a variant of the fix.
4. A count of checks that drops, or a run that ends sooner, is a
   failure: compare the last check a run prints with the last step the
   scenario has.

## Cross-links

- [../shell/notes.md](../shell/notes.md),
  [../platform/ipc.md](../platform/ipc.md),
  [../shell/plugins.md](../shell/plugins.md), the `canvas-plugins`,
  `debugging-discipline` and `verification` skills.
