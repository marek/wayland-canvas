# Modal dialogs float over their window; going to a desktop focuses its last window

Date: 2026-10-18 20:43 EDT

## Context

A dialog opens centred over the window it belongs to. A modal one floats
there, even on a tiled desktop: it never joins the board, so it sits over
the window it blocks instead of being tiled beside it. A dialog that
leaves its window usable tiles like any window. A window belongs to
another when it has a parent (an xdg toplevel given one, an X11 window
transient for another); it is modal by X11's `_NET_WM_STATE_MODAL`, or
for a wayland client through xdg-dialog-v1, which the compositor offers.

Going to a desktop — Super+N, a hop, leaving the overview onto it — gives
the keyboard to the window last used there, so typing goes on where it
left off without a click. The window already focused keeps it when it is
on that desktop; a desktop with no window leaves the keyboard where it
was.

The contracts are
[../intents/windows/window-management.md](../intents/windows/window-management.md)
and [../intents/canvas/desktops.md](../intents/canvas/desktops.md).

## Tasks

- [x] The intents: the dialog placement row, tiling's exception for modal dialogs, xdg-dialog-v1 in the protocol table, the going-to-a-desktop focus row
- [x] A dialog opens centred over its parent; a modal one never joins a tiled board
- [x] Going to a desktop gives the keyboard to its topmost window, the overview's exit and Super+N and hops alike
- [x] The xwayland scenario proves both: on a tiled desktop a modal dialog floats centred over its tiled window, and Super+N moves the keyboard to the last window used on each desktop; the gestures and input scenarios still pass
- [ ] On screen: Krita's dialogs on a tiled desktop, a shell taking keys after Super+N (the user)
