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
- **The notes plugin owns the notes; the compositor draws them**, through
  the canvas items of the control socket. The plugin keeps text, colour and
  place, and puts every note back when the shell starts.
- **Editing happens over the note**: the editor opens exactly where the
  note is on screen and has the keyboard at once, so editing feels in
  place.
- **One event opens the editor**: the compositor's `item.activated` —
  whether a double-click or the create action's `item.activate` caused it
  — carries the output and the note's box on it, so the editor always
  opens exactly at the note, in the normal view and the overview alike.

## Behavior

| Subject | Rule |
|---|---|
| Create | The toolbar's note button expands into colour swatches; choosing one puts a note of that colour at the centre of what the overview is aimed at, and opens its editor |
| Action | `canvas.notes/create {color}`: the one action behind it all — the toolbar's entries run it, and so does the control socket's `action.run`, so a script or a keybinding can put a note too. It places the note (no place of its own, so the compositor centres it), keeps it, and activates it |
| Colours | Yellow, green, blue, pink, purple and orange; the values are the plugin's settings |
| Size | 220 × 220 canvas px, a plugin setting |
| Look | A card of the note's colour, a darker strip along the top, the text in dark ink |
| Edit | Double-click the note: the editor opens over it with the cursor at the end of the text; Escape or a click outside closes it, keeping the text |
| Recolour | In the editor, colour dots on the strip, ordered as the manifest orders the colours — the toolbar menu's order — because the settings map arrives alphabetised; a colour the menu leaves out follows after |
| Move | Drag the note, in the normal view or the overview |
| Delete | In the editor, the × at the right of the strip; a note closed with no text is deleted too |
| Kept | Every change is saved to the plugin's storage at once; the notes come back where they were when the shell starts |

## Error states, corner cases

- The control socket is missing: the note button is not offered and no
  note is drawn.
- The shell restarts: notes stay on the canvas meanwhile and are re-sent
  from storage, so none is lost or doubled.
- A note dragged while its editor is open: the editor closes first, keeping
  the text.
- A note not on any output's screen cannot be activated: `item.activate`
  answers an error and no editor opens.

## Cross-links

- [toolbar.md](toolbar.md) — the button and its colour submenu.
- [plugins.md](plugins.md) — the plugin model and its storage.
- [../platform/ipc.md](../platform/ipc.md) — canvas items: how notes are
  drawn, moved and activated.
