# Gestures, mouse and keyboard

Every way of driving the canvas by hand: the touchpad gestures, the mouse and
the keyboard. Shortcuts and gesture choices are yours to change — see
[Configuration](configuration.md).

## Touchpad gestures

| Where | Gesture | Action |
|---|---|---|
| Normal | 3-finger swipe | **Hop** to the closest desktop in that direction, at its zoom; none that way → you stay put (a hint says so). A swipe never leaves the desktops: reach a free area from the overview, or look there with Super |
| Normal | 4-finger swipe up | Zoom out into the overview (follows your fingers; release early to cancel) |
| Normal | Super + 2-finger pinch | **Zoom** the desktop shown, following your fingers; it settles on 5 % steps. Windows keep their size; Super+0 returns to 100 % |
| Normal | Super + 2-finger scroll | **Look away**: the view pans, over windows too; double Super goes back to the view you left. Without Super a scroll is the window's, and nothing over empty canvas |
| Overview | 3-finger swipe | Jump to the closest desktop in that direction; none that way → move one screen that way (free spot) |
| Overview | 4-finger swipe down | Zoom into the target |
| Overview | 4-finger swipe up | Zoom out further |
| Overview | 4-finger swipe left/right | Swipe the desktop under the pointer (or the target) away: **abandon** it; windows stay |
| Overview | 3-finger pinch out | **Tile** the windows of the desktop under the pointer (or the target) onto a one-shot board of slices |
| Overview | 3-finger pinch in | **Combine**: the nearest other desktop's windows move into this one (relative layout kept) |
| Overview | 2-finger pinch / scroll | Zoom / pan the overview |
| Overview | Super + 2-finger pinch | Zoom the desktop under the pointer (or the target); `0` returns the target to 100 % |

The finger counts are a setting: the `swipes` line of `[gestures]` in
`~/.config/wayland-canvas/canvas.conf` picks `three-finger-hops` (the table
above, the default), `four-finger-hops` (the same table with every 3 and 4
swapped: 4 fingers hop, 3 up open the overview, 3 down go in, 3 sideways
abandon, a 4-finger pinch tiles and combines) or `off` — no 3- or 4-finger
compositor gesture at all: in the normal view they are forwarded to the client
under the pointer like 2-finger ones, in the overview they do nothing.
`pinch-scale = off` in the same section makes a Super + 2-finger pinch a plain
pinch: the client's in the normal view, the overview's zoom in the overview.
The overview's help line names the gestures of the preset in force (keys only
under `off`).

## Mouse

| Where | Action | Result |
|---|---|---|
| Title bar | drag / double-click | Move (dragging a maximized window restores it) / maximize-restore |
| Title bar | `–` `☐` `✕` | Minimize, maximize/restore, close |
| Window edges/corners | drag | Resize |
| Anywhere | Super+left / Super+right drag | Move / resize |
| Empty canvas | Super + drag / Super + scroll | Look away (double Super goes back); without Super nothing moves |
| Overview | click desktop / window / empty canvas | Go there (empty canvas = free area) |
| Overview | drag a frame (tab or empty interior) | Reposition the desktop over the windows |
| Overview | drop a frame onto another | Combine them |
| Overview | right-click (two-finger tap or press) on empty canvas | New desktop at that spot |
| Overview | right-click a desktop's tab or its empty interior | Remove that desktop (its windows stay) |
| Overview | tab buttons ⊞ / ✕ | Tile / abandon |
| Overview | minimized shelf (bottom) | Restore that window |

## Keyboard

Super is the logo (Windows) key. Every shortcut is yours to change, in
`canvas.conf` (see [Configuration](configuration.md)) or by pressing the new
keys in the settings window; the table is the defaults. A lone tap of Super
runs nothing by default — it is a key any action can hold (`Super` in
`canvas.conf`) — and a double tap (`Super Super`) goes back from a look away.
VT switching (Ctrl+Alt+F1…F12) is fixed: in no file, and no chord can take it,
so the way out of a stuck session stays.

| Default keys | Name in `canvas.conf` | Action |
|---|---|---|
| Super+Space | `launcher` | App launcher |
| Super+Return | `terminal` | Terminal |
| Super+Q, Alt+F4 | `close` | Close window |
| Super+M / Super+Shift+M | `minimize` / `restore` | Minimize / restore last minimized |
| Super+arrows | `snap-left` … `snap-down` | Step the focused window's slice on a tiled desktop (Super+Up maximizes on a floating one) |
| Super+F | `fullscreen` | Fullscreen |
| Super+Tab, Super+W | `overview` | Toggle overview |
| Super+Ctrl+arrows | `hop-left` … `hop-down` | Go to the desktop in that direction (none that way: stay) |
| Super+Ctrl+Shift+arrows | `move-left` … `move-down` | Move the focused window to that desktop and follow |
| Super+Alt+arrows | `pan-left` … `pan-down` | Look a third of a screen away |
| Double Super | `pan-back` | Back to the view you looked away from |
| Super+Ctrl+D / Super+Ctrl+F4 | `pin-desktop` / `abandon-desktop` | Make the current view a desktop / abandon the current desktop |
| Super+T | `tiling` | Tile the current desktop (or view) |
| Super+1…9 | `desktop-1` … `desktop-9` | Go to desktop N |
| Super+0 | `zoom-reset` | Return the desktop shown, or a free area, to 100 % |
| Super+Shift+3 / 4 / 5 | `shot-screen` / `capture-pick` / `capture-toolbar` | Whole-screen shot / the capture picker / the capture toolbar (Ctrl on a shot: clipboard) |
| Print / Shift+Print / Alt+Print | `capture-pick` / `shot-screen` / `record-stop` | The capture picker / whole-screen shot / stop a recording, else the toolbar on its recordings |
| Super+L | `lock` | Lock the screen (the password is your login one, through PAM) |
| Super+N / Super+Shift+N | `notifications-dismiss` / `notifications-dismiss-all` | Take the newest notification popup, or every one, off the screen |
| Super+Ctrl+N | `night-light` | The night light on or off |
| Super+Shift+E, Ctrl+Alt+Backspace | `quit` | Quit |
| Overview: arrows, Return, Escape, Delete, T, M, N, 0, +/- | `[overview]`: `hop-left` … `leave` | Jump, enter, back, abandon, tile, combine, new desktop, target back to 100 %, zoom; Super+Tab / Super+W leave |

## See also

[Concepts](concepts.md) for what the gestures move you between, and
[Configuration](configuration.md) for changing any of these defaults.
