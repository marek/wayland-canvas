# Sticky notes

> Owns sticky notes: creating one from the overview toolbar, their colours
> and look, the text and its formatting, moving, sizing, deleting, and
> keeping them across restarts. The surfaces a plugin places on the plane
> are in [../platform/ipc.md](../platform/ipc.md); the toolbar is
> [toolbar.md](toolbar.md).

## Requirement

Coloured sticky notes can be put anywhere on the canvas from the zoomed-out
overview: the toolbar's note button expands into a choice of colours, and
choosing one puts a note of that colour on the canvas. Notes belong to the
canvas: they pan, zoom and scale with it on every output, show in the
overview and stay where they were put across restarts. A note is written in
where it sits, grows to hold what is written, is dragged by its strip and
sized by its bottom-right corner. Its text can be bold, italic,
underlined and sized span by span, in the note's own font, and each note
is a plain-HTML file that can be read and edited outside the shell.

## Design considerations

- **A note is an annotation of the canvas, not a window**: it never takes
  part in window management (placement, tiling, desktop membership,
  minimise), and it is drawn above windows so it is never hidden behind
  what it labels.
- **The plugin draws its notes; the compositor only places them.** A note
  is one surface of the plugin's own, placed on the plane
  ([../platform/ipc.md](../platform/ipc.md)): the same surface whether it
  is being written in or not, so there is never a second card to drift
  away from the first. The compositor carries no note-shaped code, and the
  plugin can be changed or replaced without rebuilding it.
- **There is no editing mode.** A note is always editable where it is: a
  click puts the caret in it, as a click in a window does; the colour, the
  font, the size and the formatting are chosen on the note's own strip.
- **The plane does the motion.** Panning, zooming and a desktop's scale
  move and size the note because they move and size everything on the
  plane; the plugin sets a note's canvas rectangle and nothing else. A
  drag of the strip or the corner is handed to the compositor, which moves
  or sizes the surface with its own pointer as it does a window's title
  bar: a note that moved itself from its own pointer events would move
  the coordinates those events are measured in, and chase the pointer.
- **A note is one object, changed in place.** Everything about a note —
  its colour, text, font, size and rectangle — lives on one object whose
  properties tell what is bound to them; the surface, its placement and
  its file all follow a change from there.
- **One font per note; emphasis and size per span.** The font belongs to
  the whole note, so a note reads as one hand; bold, italic, underline and
  the text size mark parts of it, as in any editor.
- **The files are the notes.** Each note is a plain-HTML file of its own
  and nothing is kept beside them: a browser shows a note as the card
  shows it, and a file edited or removed while the shell is down is a note
  edited or removed when it starts.

## Behavior

| Subject | Rule |
|---|---|
| Create | The toolbar's note button expands into colour swatches; choosing one puts a note of that colour at the centre of what the overview is aimed at, and gives it the keyboard |
| Action | `canvas.notes/create {color}`: the one action behind it all — the toolbar's entries run it, and so does the control socket's `action.run`, so a script or a keybinding can put a note too. It places the note at the centre of what a screen in the overview shows (the first screen's view when none is), keeps it, and gives it the keyboard |
| Colours | Yellow, green, blue, pink, purple and orange; the values are the plugin's settings |
| Card | 220 × 220 canvas px when created, a plugin setting |
| Look | A card of the note's colour, a darker strip along the top holding the note's controls, the text in dark ink, a grip in the bottom-right corner |
| Strip | From the left: the colour dots, bold, italic and underline, the font, the text size (− and + around the size), and the × at the right. A note is never narrower than its strip |
| Edit | Click the note and type: it is the note itself that takes the keys, at any zoom. Escape or a click elsewhere gives the keyboard back |
| Grow | A note grows downwards as it is written in, so nothing typed is ever hidden below its edge; it never shrinks by itself |
| Formatting | Bold, italic, underline and the text size apply to the selected span, and text typed inside a span continues it; with nothing selected the choice applies to what is typed next. The strip shows the format at the caret, and Ctrl+B, Ctrl+I and Ctrl+U are the same buttons |
| Text size | − and + step the size by a pixel, of the selection or of what is typed next; the number between them is the size at the caret and can be typed into, Enter applying it and Escape leaving it as it was. A new note's text takes the shell's body size |
| Font | The whole note's, chosen on the strip from every family installed on the machine, filtered by typing. A new note takes the shell's body font |
| Recolour | Colour dots on the note's strip, ordered as the manifest orders the colours — the toolbar menu's order — because the settings map arrives alphabetised; a colour the menu leaves out follows after |
| Move | Drag the note by its strip, in the normal view or the overview, on any output: it follows the pointer and stays where it is dropped |
| Resize | Drag the grip at the bottom-right corner, at any zoom: the note follows the pointer, never smaller than its strip and its text, and the text re-wraps to the new width |
| Delete | The × at the right of the note's strip; a note left empty when the keyboard goes is deleted too |
| Kept | Every change is written to the note's file at once: `<number>.html` in the plugin's storage, holding the text with its `<b>`, `<i>` and `<u>` spans and the `font-size` of a sized one, the colour, the font and the size of the rest as the page's style, and the note's canvas rectangle in a `canvas-note` meta element. The notes come back where they were when the shell starts |

## Error states, corner cases

- The control socket is missing: the note button is not offered, and a
  note that cannot be placed on the plane says so and is not shown.
- The shell restarts: its note surfaces go with it and come back from the
  files, each at its place, none lost or doubled.
- A note dragged while the caret is in it: the text stays as it was; the
  note moves with the pointer and is placed again when it is dropped.
- A note off every output's screen is drawn when the canvas brings it
  back; nothing of it is lost meanwhile. An output that goes takes none
  of the notes with it.
- Text pasted with formatting of its own is kept with only its bold,
  italic, underline and size: the note's file holds no other font or
  colour, and the card shows the note's font once the note is read back.
- A typed size that is not a whole number of pixels is not taken: the
  number shows the size at the caret again.

## Cross-links

- [toolbar.md](toolbar.md) — the button and its colour submenu.
- [plugins.md](plugins.md) — the plugin model and its storage.
- [../platform/ipc.md](../platform/ipc.md) — the surface a plugin places
  on the plane, which is what a note is, and the drags the compositor runs
  for it.
- [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md) —
  the keyboard a shell surface takes on demand and gives back.
