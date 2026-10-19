# A fullscreen window over others gets a desktop of its own

Date: 2026-10-19 08:13 EDT

## Context

A window that goes fullscreen while other windows stand where it would
cover moves to a desktop of its own, made in the first free slot beside its
desktop by Super+N's slot order, and the screen showing it goes there; the
desktop it left keeps everything else as it was, tiling included. Leaving
fullscreen by any path puts it back and removes that desktop, and a screen
still on it goes back too. A window alone where it would cover goes
fullscreen in place, as on Windows; a window that opens fullscreen opens in
place. The desktop it gets is numbered like every other made desktop, the
smallest unused number, so hops, Super+N and the overview reach it with no
special case.

The contract is
[../intents/windows/window-management.md](../intents/windows/window-management.md)
with [../intents/canvas/desktops.md](../intents/canvas/desktops.md)'s Create
row.

## Tasks

- [x] The intents: window-management.md's fullscreen rules and corner case, desktops.md's Create row
- [x] One slot search for every desktop made beside another: Super+N's and the fullscreen desktop's
- [x] `FullscreenDesktops` (`src/canvas/fullscreen_desktops.h`): enter, the area the window covers, and a once-a-frame reconcile that brings a window back however fullscreen ended
- [x] The xwayland scenario proves it: Super+F beside another window moves it to a new desktop overlapping none, the screen and keyboard with it; Super+F brings it back and the desktop goes; alone it goes fullscreen in place; the gestures scenario's Super+N steps pass on the shared search
- [ ] On screen: a video or Krita going fullscreen over other windows, and back (the user)
