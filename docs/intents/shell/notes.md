# Sticky notes

> Owns sticky notes: creating one from the overview toolbar, their colours
> and look, editing, moving, deleting, and keeping them across restarts.
> The canvas items the compositor draws are in
> [../platform/ipc.md](../platform/ipc.md); the toolbar is
> [toolbar.md](toolbar.md).

## Requirement

Coloured sticky notes can be put anywhere on the canvas from the zoomed-out
overview: the toolbar's note button expands into a choice of colours, and
choosing one puts a note of that colour on the canvas. Notes belong to the
canvas: they pan and zoom with it, show in the overview and stay where they
were put across restarts.

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
  font and the size are chosen on the note's own strip.
- **The plane does the motion.** Panning, zooming and a desktop's scale
  move and size the note because they move and size everything on the
  plane; the plugin sets a note's canvas rectangle and nothing else.

## Behavior

| Subject | Rule |
|---|---|
| Create | The toolbar's note button expands into colour swatches; choosing one puts a note of that colour at the centre of what the overview is aimed at, and opens its editor |
| Action | `canvas.notes/create {color}`: the one action behind it all — the toolbar's entries run it, and so does the control socket's `action.run`, so a script or a keybinding can put a note too. It places the note (no place of its own, so the compositor centres it), keeps it, and activates it |
| Colours | Yellow, green, blue, pink, purple and orange; the values are the plugin's settings |
| Size | 220 × 220 canvas px, a plugin setting |
| Look | A card of the note's colour, a darker strip along the top, the text in dark ink |
| Edit | Click the note and type: it is the note itself that takes the keys, at any zoom. Escape or a click elsewhere gives the keyboard back |
| Recolour | Colour dots on the note's strip, ordered as the manifest orders the colours — the toolbar menu's order — because the settings map arrives alphabetised; a colour the menu leaves out follows after |
| Move | Drag the note, in the normal view or the overview |
| Delete | The × at the right of the note's strip; a note left empty when the keyboard goes is deleted too |
| Kept | Every change is saved to the plugin's storage at once; the notes come back where they were when the shell starts |

## Error states, corner cases

- The control socket is missing: the note button is not offered and no
  note is drawn.
- The shell restarts: notes stay on the canvas meanwhile and are re-sent
  from storage, so none is lost or doubled.
- A note dragged while the caret is in it: the text stays as it was; the
  note moves with the pointer and is placed again when it is dropped.
- A note off every output's screen is drawn when the canvas brings it
  back; nothing of it is lost meanwhile.

## Cross-links

- [toolbar.md](toolbar.md) — the button and its colour submenu.
- [plugins.md](plugins.md) — the plugin model and its storage.
- [../platform/ipc.md](../platform/ipc.md) — canvas items: how notes are
  drawn, moved and activated.
