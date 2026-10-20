# Drawing over the canvas

Date: 2026-10-17 11:13 EDT

## Context

The compositor gains a full drawing surface over the canvas: freehand
strokes, shapes, arrows, text and inserted photos, drawn anywhere on the
plane and kept there. It is a shell plugin (`canvas.draw`) plus the
compositor support it needs, so it can be hidden with one key, switched
off in the config, or deleted outright without leaving a trace in how the
compositor behaves.

The requirements that shape it:

- **Applications are never affected.** With drawings shown, every click,
  key, scroll and gesture reaches clients exactly as it does today; no
  client is reconfigured, refocused or re-laid-out because a drawing
  exists. Drawing takes input only while the user asks for it.
- **Removable and hideable.** One shortcut hides every drawing; a panel
  hides one sheet or one group; disabling or deleting the plugin removes
  the feature and its UI entirely, and the compositor runs as before.
- **Several surfaces, organised.** A panel holds many drawing sheets,
  gathered into named groups, each shown, hidden, locked or removed on
  its own.
- **Depth is a first-class, draggable property.** A sheet sits above every
  window, below every window, or between two of them, and is moved there
  by dragging it in a strip that shows the windows and the sheets in
  stacking order — not by typing a number.
- **Photos and images are inserted** and then behave as shapes on the
  sheet.

Scope: this plan covers the design and the build. Styluses with pressure,
and dropping an image from a client onto a sheet, are named below as
separate boxes so the first build is not blocked on them. Collaboration
over a network, and any cloud storage, are out of scope.

## Design

### Three states, so apps are never affected

| State | What it is | Input |
|---|---|---|
| Shown (the default) | Sheets render on the canvas, panning and zooming with it, in the overview too | None. The compositor does not hit-test sheets; clients get everything |
| Draw mode | Entered per output with `draw` (proposed Super+D) or the overview toolbar's Draw button | The compositor owns the pointer and the keyboard on that output; the palette is up; clients keep their buffers, their focus and their title-bar state |
| Hidden | `draw-hide` (proposed Super+Shift+D), a sheet's eye, or a group's eye | As "Shown", with nothing drawn |

Draw mode never touches a client: no configure, no focus change, no
unmap. Leaving it hands typing straight back to the window that had it.
Multi-finger gestures keep working in draw mode (a hop or the overview is
always reachable), and the Select-and-pass tool returns pointer input to
the clients while the palette stays up, so a user can click a button in
an app and carry on drawing.

### The model

| Thing | Is |
|---|---|
| Sheet | A named drawing surface in canvas coordinates holding shapes; has visibility, a lock, an opacity, a depth anchor and a group |
| Group | A named set of sheets, shown, hidden or removed together ("Review", "Diagrams") |
| Shape | Stroke, highlighter stroke, rectangle, ellipse, line, arrow, text, image |
| Shape properties | Colour, stroke width, stroke style (solid, dashed), fill (none, solid), opacity, arrowheads, corner rounding, font size |

Sheets are not windows: no placement, tiling, desktop membership or
minimise, exactly as canvas items are not
([shell/notes.md](../intents/shell/notes.md)).

### Depth, and the strip that sets it

A sheet's depth is an anchor, not an index, so it survives stacking
changes:

| Anchor | Means |
|---|---|
| `top` | Above every window, below the bar and the palette |
| `bottom` | Below every window, above the wallpaper |
| `above:<window>` / `below:<window>` | Tied to that window: it follows as the window is raised, lowered or moved; when that window closes the sheet takes the anchor's neighbour |

The layers panel shows one vertical **stack strip** per output, bottom to
top: the wallpaper, each window with its icon and title, and the sheets
among them. Dragging a sheet card moves it between rows, the strip
reflowing under the pointer with the rows parting where it would land;
the drop sets the anchor. The same drag works from the palette's depth
grip. `Super+Shift+Up`/`Down` in draw mode steps the focused sheet one
row, and the panel has Top, Bottom and "Under the focused window"
buttons.

### Where the document lives

The **compositor owns the live document** — geometry, hit testing, the
grab, undo — and the **plugin owns the chrome, the storage and the
export**, following the document through events. This inverts sticky
notes (the plugin owns a note, the compositor draws it) and the reason is
latency: a freehand stroke cannot round-trip through the control socket
per motion event. Two alternatives were weighed and dropped: a
layer-shell client drawing the ink (it cannot interleave with window
stacking, and it cannot zoom with the canvas), and the plugin owning
geometry (the stroke would lag the finger).

Rendering: each sheet is rasterised per output into a cached texture,
re-rasterised when its shapes change or the zoom settles; a stroke in
flight draws directly. A hidden sheet costs nothing, and an idle output
still renders nothing.

### The interface

```
  ┌───────────────────────────────┐        palette (draw mode only,
  │ ▣ ✋ ✎ ▰ ⌫ │ ▭ ◯ ╱ ↗ T 🖼 ✦ │        draggable, collapses to a pill)
  │ ●●●●●●●●  ▁▂▃▄  ─ ┄  ◻ ◼   │
  │ sheet: Review ▾   ⧉ layers    │
  └───────────────────────────────┘

  layers panel                 stack strip
  ┌──────────────────┬──────────────────────┐
  │ ▾ Review         │  ▬ top bar           │
  │   👁 🔒 Notes ⠿  │  ▤ Notes        ⠿ ←  │ drag a card up or down
  │   👁 🔒 Fixes ⠿  │  ▢ Firefox           │
  │ ▾ Diagrams       │  ▤ Fixes        ⠿    │
  │   👁 🔒 Flow  ⠿  │  ▢ foot              │
  │ + sheet  + group │  ▨ wallpaper         │
  └──────────────────┴──────────────────────┘
```

Tools and their keys in a `[draw]` scope of their own, so they never
collide with the canvas bindings: select, hand, pen, highlighter, eraser,
rectangle, ellipse, line, arrow, text, image, laser; brackets size the
stroke, Ctrl+Z/Ctrl+Shift+Z undo and redo, Ctrl+C/X/V, Delete, Ctrl+A,
Ctrl+D duplicate, Ctrl+G group, arrows nudge, Escape leaves.

## Decisions for the user

- [ ] The chords: `draw` on Super+D and `draw-hide` on Super+Shift+D (the user to decide)
- [ ] Whether a window keeps receiving keys while draw mode is on, or the keyboard belongs wholly to drawing until Escape (the user to decide)
- [ ] Whether a sheet is unbounded ink or a sized page with edges (the user to decide)
- [ ] Whether stylus pressure (tablet-v2) is in the first build or a follow-up (the user to decide)
- [ ] Whether drawings appear in screenshots and recordings — proposed yes, they are canvas content (the user to decide)

## Intents

- [ ] `shell/drawing.md`: the three states, sheets and groups, shapes and properties, depth anchors and the stack strip, the palette, the layers panel, images, hiding and removal, persistence and export
- [ ] `platform/ipc.md`: the `ink.*` methods, events and state, integrated into its tables
- [ ] `input/keyboard-and-pointer.md`: the `draw` and `draw-hide` bindings, and the `[draw]` key scope
- [ ] `render/rendering.md`: sheets in the draw order, the depth anchors they are placed by, the per-sheet texture cache
- [ ] `shell/plugins.md`, `shell/toolbar.md`, `glossary.md`, `intents/README.md`: the `canvas.draw` plugin row, its toolbar button, the new terms, the index row and reading path

## Compositor: the document and its drawing

- [ ] The document: sheets, groups, shapes with their properties, canvas-space geometry, per-shape bounds, and a hit test
- [ ] Stroke building: input points smoothed into a variable-width path, so a slow line is clean and a fast one keeps its shape
- [ ] Rendering sheets on the canvas in draw order by their depth anchors, with a per-sheet, per-output texture cache invalidated on change and on a settled zoom
- [ ] Anchor resolution as windows are raised, lowered, minimised and closed, with a sheet keeping its place in the stack
- [ ] Undo and redo per sheet, bounded, covering every edit including delete and clear

## Compositor: draw mode

- [ ] Entering and leaving per output: the pointer and keyboard grab, the cursor per tool, no client told anything, and every leave path (Escape, the action again, the palette's close, the output going away)
- [ ] The tools: select with transform handles, hand, pen, highlighter, eraser, rectangle, ellipse, line, arrow, text, laser
- [ ] Selection: click, rubber band, move, scale from handles, rotate, group, align, duplicate, clipboard
- [ ] The `[draw]` key scope, nudges, and multi-finger gestures still reaching the canvas while draw mode is on
- [ ] Select-and-pass: pointer input reaching the clients with the palette still up

## Control IPC

- [ ] `ink.state`, sheet and group methods (`ink.sheet.set`, `ink.sheet.remove`, `ink.sheet.clear`, `ink.group.set`, `ink.group.remove`), and the depth anchor as a parameter
- [ ] Shape methods for what the shell inserts (`ink.shape.add`, `ink.shape.update`, `ink.shape.remove`, `ink.image.add`) and `ink.undo` / `ink.redo`
- [ ] Mode and tool methods (`ink.mode`, the current tool and properties) and `ink.export`
- [ ] Events: `ink.sheets`, `ink.ops` (the deltas the plugin persists), `ink.mode`, `ink.selection`; `ink` in the state report
- [ ] A document too large, an unknown id, a locked sheet, an unwritable export: errors naming the reason, no partial edit

## The plugin

- [ ] `canvas.draw`: the manifest, the `draw` toolbar button, the actions the compositor's `action.run` can reach
- [ ] The palette: tools, colours, widths, styles, fill, opacity, the sheet chooser, the depth grip; draggable, collapsible, its place remembered per output
- [ ] The layers panel: groups and sheets, eye, lock, opacity, rename, add, remove, reorder
- [ ] The stack strip: windows and sheets in stacking order, live-reflowing drag, the drop setting the anchor, Top / Bottom / Under the focused window
- [ ] The empty state and the first sheet: drawing with no sheet creates one, named for the desktop or the free area it is on

## Images

- [ ] The image tool: a file picker (the portal's FileChooser), the image placed on the sheet and scaled from its handles
- [ ] Pasting an image from the clipboard in draw mode
- [ ] Images stored in the plugin's storage and referenced by the document, so a sheet survives the source file moving
- [ ] Dropping an image from a client onto a sheet (data-device drop onto the compositor)

## Keeping, exporting, removing

- [ ] Every change saved to the plugin's storage, debounced; sheets, groups and images restored when the shell starts, none lost or doubled
- [ ] Export: a sheet or a selection to PNG and SVG
- [ ] Hiding: `draw-hide` clears the screen of every sheet at once and brings them back; a sheet's and a group's eye
- [ ] Removal: disabling the plugin removes the palette, the panel, the button, the drawings and draw mode, with the compositor unchanged; re-enabling brings the sheets back from storage
- [ ] Settings page: the chords, whether drawings show in the normal view, the default tool and colours, the palette's place

## Verification

- [ ] Unit: stroke smoothing, shape hit tests, depth-anchor resolution through raise, minimise and close, document serialisation round-trip
- [ ] Scenario: with sheets shown and no draw mode, a client receives every click, key, scroll and gesture unchanged
- [ ] Scenario: draw mode draws a stroke, leaves, and the focused window types again with no reconfigure
- [ ] Scenario: a sheet dragged in the stack strip from above a window to below it, proven in screenshots
- [ ] Scenario: hide-all empties the screen and restores it; a group's eye hides its sheets only
- [ ] Scenario: sheets, groups and an inserted image come back after a shell restart
- [ ] Scenario: the plugin disabled — no palette, no drawings, no draw mode, the compositor otherwise identical
- [ ] Screenshots of the palette, the layers panel, the stack strip mid-drag, and drawings in the normal view and the overview, inspected
- [ ] Nested and TTY runs, owed on screen: the feel of drawing, the depth drag, and the palette at 2x (the user)
- [ ] Warning-free build, every suite green, `tools/check.sh` clean
