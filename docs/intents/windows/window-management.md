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

Placement, from the first commit (so a window that starts maximized or
fullscreen fills the area around where it opens, whether it asks before
or after that commit):

| Rule | Behaviour |
|---|---|
| Centre | centred in the viewport of the output under the cursor; from the overview, centred on the target (the desktop, or the free spot) |
| Launched from the overview | centred on the target the overview had at the launch, even when the overview closed or its target moved before the window appeared: a window whose process (or an ancestor) the compositor launched from the overview in the last 30 s (launcher entries, Super+Return). A window handed to an instance already running, or started over D-Bus, cannot be traced and follows the rule above |
| Dialog | a window with a parent — an xdg toplevel given one, an X11 window transient for another — is centred over it (by the rules above when it has none). A modal one — X11's `_NET_WM_STATE_MODAL`, or a wayland dialog made modal through xdg-dialog-v1 — floats there even on a tiled desktop: it never joins the board, so it sits over the window it blocks. A dialog that leaves its window usable tiles like any window |
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
| New window | takes the focus, except a window launched from the overview that opens where no output shows it (the overview closed, or moved on, before it appeared): the keyboard stays with the window in front of you, and a hint says where it opened ("Opened in Desktop 3", "Opened in a free area") |
| Minimize | focus passes to the topmost visible window |
| Going to a desktop | Super+N, a hop or leaving the overview onto it: the keyboard goes to the window last used there, the topmost of its windows, unless the focused window is already on it; a desktop with no window leaves the keyboard where it was |
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
and the camera moves to it: to its desktop at that desktop's scale when
it is on one, else to a view centred on the window, and a screen in the
overview leaves the overview with the move.

| Restore path | Rule |
|---|---|
| Overview shelf | clicking a shelf item restores that window and zooms to it ([../canvas/overview.md](../canvas/overview.md)) |
| App bar | clicking an app's entry goes to its window the same way, minimized or not ([../shell/app-bar.md](../shell/app-bar.md)); the control socket's `window.focus` is that path for any client ([../platform/ipc.md](../platform/ipc.md)) |
| Key | Super+Shift+M restores the last minimized window |
| Activation | an xdg-activation request carrying a token granted for real input |
| Foreign-toplevel | an activate request from a foreign-toplevel-management client |

Tiling contract — when a desktop's tiling is on and which windows it takes
is decided in [../canvas/desktops.md](../canvas/desktops.md); what tiling
does to each of them is:

| Rule | Behaviour |
|---|---|
| Layout | a board of slices (the whole, halves, quarters) filling the desktop's area inset by the bars, with gaps (`TILE_GAP`); a join never moves the windows already on the board |
| Placement | a window opened on a tiled desktop takes the largest free slice, or halves the largest held one, not the usual new-window spot |
| Client | each tiled window is told it is tiled; told it no longer is when it leaves the board or tiling goes off |
| Move, resize | allowed; a move release swaps slices with the window under the drop or takes the largest free slice under it, a resize release returns the window to its own slice |
| States | maximize and fullscreen take a window out of the layout while in that state but keep its slice reserved; leaving the state returns it to that slice; minimize frees the slice while remembering it |

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
