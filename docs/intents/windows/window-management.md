# Window management

> Owns what happens to a window as a whole: placement, focus and
> stacking, maximize, fullscreen, minimize and restore, tiling,
> resizing, closing. The title bar and its buttons are in
> [title-bars.md](title-bars.md); the key bindings in
> [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md).

## Requirement

Windows behave the Windows way: a new window appears where you are
looking, clicking focuses and raises, maximize fills the screen minus
the bars, minimize hides, and every restore path brings you back to
the window.

## Behavior

Placement, on map:

| Rule | Behaviour |
|---|---|
| Centre | centred in the viewport of the output under the cursor; from the overview, centred on the target (the desktop, or the free spot) |
| Bars | inside the area not covered by layer-shell exclusive zones |
| Title bar | clamped so the title bar is fully on screen |
| Taken spot | if another window already occupies the position, cascade by 32 px |
| Initial size | the client chooses, unless it asked to start maximized or fullscreen |

Focus and stacking — the pointer model (click-to-focus, focusing
raises) is owned by
[../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md):

| Rule | Behaviour |
|---|---|
| Click-to-focus | clicking a window focuses it and raises it |
| Minimize | focus passes to the topmost visible window |
| Fullscreen | a fullscreen window is raised above the others |

Maximize area, by precedence:

| Precedence | Area |
|---|---|
| 1 | the output viewport the window is visible on in normal mode (its frame centre inside the viewport) |
| 2 | else its desktop's box ([../canvas/desktops.md](../canvas/desktops.md)) |
| 3 | else a screen-sized box centred on the window |

Every step is inset by the usable area — what the bars leave free
([../platform/outputs.md](../platform/outputs.md)). A maximized
window keeps its title bar and loses its border
([title-bars.md](title-bars.md)); the pre-maximize content box is
saved and restored on unmaximize.

Fullscreen covers the output viewport with no decorations at all, and
is raised.

Minimize hides the window from rendering and from input; the state is
published on its foreign-toplevel handle
([../platform/protocols.md](../platform/protocols.md)). Restoring a
window always brings you to it: it is un-minimized, focused, raised,
and the camera moves to it.

| Restore path | Rule |
|---|---|
| Overview shelf | clicking a shelf item restores that window and zooms to it ([../canvas/overview.md](../canvas/overview.md)) |
| Key | Super+Shift+M restores the last minimized window |
| Activation | an xdg-activation request carrying a token granted for real input |
| Foreign-toplevel | an activate request from a foreign-toplevel-management client |

Tiling contract — when a desktop's tiling is on and which windows it takes
is decided in [../canvas/desktops.md](../canvas/desktops.md); what tiling
does to each of them is:

| Rule | Behaviour |
|---|---|
| Layout | a grid inside the desktop's area inset by the bars, with gaps (`TILE_GAP`), re-flowed whenever the desktop's tiled set changes |
| Placement | a window opened on a tiled desktop is placed into the grid, not at the usual new-window spot |
| Client | each tiled window is told it is tiled; told it no longer is when it leaves the grid or tiling goes off |
| Move, resize | allowed; on release a window still inside the desktop goes back into the grid, at the slot nearest where it was dropped |
| States | maximize and fullscreen take a window out of the grid while in that state; leaving the state returns it |

Resizing:

| Rule | Behaviour |
|---|---|
| Anchor | the edge opposite the grab stays fixed for the whole resize |
| End | anchoring lasts until the client acks the configure that ended the resize — the client's final commit arrives after the button release, so stopping the anchor at release lets the trailing edge drift ([../bugs/resize-anchor-lost-on-release.md](../bugs/resize-anchor-lost-on-release.md)) |

Close destroys the window. It is reached from the caption close button
([title-bars.md](title-bars.md)), the close keys
([../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)),
and a foreign-toplevel close request, all with the same effect.

## Design considerations

- **Free areas are first-class** ([../canvas/infinite-canvas.md](../canvas/infinite-canvas.md)):
  a window can be visible on an output while belonging to no desktop,
  so maximize falls back from the screen it is on, to its desktop, to
  a screen-shaped box around itself — it never fails to find an area.
- **The saved box is the content box**, not the frame, so unmaximize
  puts back exactly what the client had before.

## Error states, corner cases

- Minimizing while nothing else is visible leaves the desktop empty;
  focus goes to whatever is topmost and visible.
- A window placed from the overview lands on the target whether the
  target is a desktop or a free spot — a free spot is as good a place
  to open an app as a desktop.

## Cross-links

- [title-bars.md](title-bars.md) — the bar, the buttons, resize zones.
- [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)
  — the bindings table, pointer focus, Super+drag.
- [../canvas/infinite-canvas.md](../canvas/infinite-canvas.md),
  [../canvas/desktops.md](../canvas/desktops.md),
  [../canvas/overview.md](../canvas/overview.md) — where windows live
  and the shelf they are restored from.
- [../platform/protocols.md](../platform/protocols.md),
  [../platform/outputs.md](../platform/outputs.md) — foreign-toplevel,
  activation, the usable area.
- [../bugs/resize-anchor-lost-on-release.md](../bugs/resize-anchor-lost-on-release.md)
  — the resize-anchor case study.
