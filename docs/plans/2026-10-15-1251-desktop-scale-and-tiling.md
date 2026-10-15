# Desktop scale and tiling

Date: 2026-10-15 12:51 EDT

## Context

Every desktop has its own scale and can be switched to tiling. Holding Super while pinching with two fingers on the
touchpad scales the desktop shown, or the one under the cursor in the overview: its frame grows or shrinks with the
fingers, the windows on it are shown and used at that scale and told it so they render sharply, and the scale stays
with the desktop. Tiling is a switch per desktop (its tab, `t` in the overview, Super+T, a 3-finger spread): while it
is on, the desktop keeps its windows in a grid and every window opened on it joins the grid. A free view asked for
either is pinned as a desktop first. Without Super, a 2-finger pinch keeps reaching the application under the
pointer. The rules: [canvas/desktops.md](../intents/canvas/desktops.md),
[canvas/infinite-canvas.md](../intents/canvas/infinite-canvas.md), [input/gestures.md](../intents/input/gestures.md),
[render/hidpi.md](../intents/render/hidpi.md), [windows/window-management.md](../intents/windows/window-management.md),
[canvas/overview.md](../intents/canvas/overview.md).

## Scale

- [ ] A desktop's scale (50 % to 200 %) sizes its frame; the camera rests at the scale of the desktop it shows, 1 elsewhere
- [ ] Windows on a scaled desktop take the pointer, the keyboard and gestures through the camera, title bars and popups included
- [ ] Clients are told the output's scale times the desktop's (fractional scale, preferred buffer scale), and title bars are drawn at it
- [ ] Super + 2-finger pinch scales the desktop shown or under the cursor, following the fingers, and settles on 5 % steps with a catch at 100 %; a free view is pinned first
- [ ] Going to a desktop (a pan's snap, Super+1..9, the overview) eases to its scale; the overview tab shows a scale other than 100 %

## Tiling

- [x] Tiling is a switch per desktop: the tab's Tile switch, lit while on; `t`; Super+T, a free view pinned first; the 3-finger spread
- [x] While on, the grid re-flows when a window opens, closes, is minimized or restored, or moves in or out, and when the scale or the frame changes; new windows are placed into it
- [x] A dropped tiled window takes the nearest slot; maximize and fullscreen leave the grid while in that state; switching off keeps positions and tells the windows
- [x] Combining into or from a tiled desktop, and abandoning one, follow the desktop rules

## Verification

- [ ] The gestures scenario covers the Super pinch: scaling, settling, a free view pinned, no launcher on Super's release, plain pinches still forwarded
- [ ] A scenario clicks and types into a window on a desktop at 80 %, landing exactly where the pointer is
- [x] A scenario opens windows on a tiled desktop and sees each join the grid; closing one re-flows it; switching off keeps the positions
- [ ] Screenshots of desktops at 80 % and 125 %, a tiled desktop, and the overview's tabs, inspected
- [ ] Warning-free build, every suite green
