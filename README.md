# wayland-canvas

An infinite-canvas Wayland compositor (wlroots 0.20). Windows live anywhere on an
unbounded plane; **virtual desktops are screen-sized dashed frames** you drag over
the windows however you like. You can work on a desktop or anywhere in between.
Windows get Windows-11-style title bars with minimize, maximize/restore and close.

## Build

Needs a C11 compiler, meson ≥ 1.3, ninja, and the wlroots 0.20 dependencies
(wayland, wayland-protocols, libdrm, gbm/EGL/GLES2, libinput, libseat, xkbcommon,
pixman, libdisplay-info, hwdata) plus cairo and pango.

```sh
meson setup build
ninja -C build
meson test -C build        # unit tests for the navigation/tiling math
```

If `wlroots-0.20` isn't installed system-wide, meson fetches 0.20.2 into
`subprojects/` and links it statically (no libliftoff/Xwayland needed).

## Run

- From a TTY: `./build/wayland-canvas` (DRM/KMS via libseat/logind).
- Nested in another compositor: run the same command from a terminal there. The
  host may swallow 3/4-finger gestures if it binds them itself.
- Options: `-s <cmd>` startup command, `-S <scale>` output scale (or
  `$WAYLAND_CANVAS_SCALE`; default is 2 on dense panels such as 2880×1800, else 1),
  `-d` debug logging.

The terminal is `$TERMINAL` (falls back to `foot`).

## Opening apps

Tap the Windows (logo) key, or press Super+Space: a Start-style launcher lists
every installed app (XDG `.desktop` entries from `~/.local/share/applications`
and `/usr/share/applications`, with icons from your icon theme). Type to filter,
↑/↓ or the mouse to pick, Enter or click to launch, Esc or a click outside to
close. New windows open in the middle of the screen you're looking at (or of
the overview's target).

## Concepts

- **Canvas**: every output has its own camera onto one shared plane.
- **Desktop**: a screen-sized rectangle on the canvas. A window belongs to the
  desktop its centre is in. Moving a desktop frame never moves windows.
- **Free area**: any spot that isn't a desktop; you can pan or jump there too.
- **Overview**: a zoomed-out, live view of the canvas where desktops are arranged.
  It has a *target* (accent outline): a desktop, or a free screen-sized spot.

## Touchpad gestures

| Where | Gesture | Action |
|---|---|---|
| Normal | 3-finger swipe | Pan the canvas anywhere (content follows fingers), with momentum; landing near a desktop snaps onto it |
| Normal | 4-finger swipe up | Zoom out into the overview (follows your fingers; release early to cancel) |
| Overview | 3-finger swipe | Jump to the closest desktop in that direction; none that way → move one screen that way (free spot) |
| Overview | 4-finger swipe down | Zoom into the target |
| Overview | 4-finger swipe up | Zoom out further |
| Overview | 4-finger swipe left/right | Swipe the desktop under the pointer (or the target) away: **abandon** it; windows stay |
| Overview | 3-finger pinch out | **Tile** the windows of the desktop under the pointer (or the target) |
| Overview | 3-finger pinch in | **Combine**: the nearest other desktop's windows move into this one (relative layout kept) |
| Overview | 2-finger pinch / scroll | Zoom / pan the overview |

## Mouse

| Where | Action | Result |
|---|---|---|
| Title bar | drag / double-click | Move (dragging a maximized window restores it) / maximize-restore |
| Title bar | `–` `☐` `✕` | Minimize, maximize/restore, close |
| Window edges/corners | drag | Resize |
| Anywhere | Super+left / Super+right drag | Move / resize |
| Empty canvas | drag / scroll | Pan |
| Overview | click desktop / window / empty canvas | Go there (empty canvas = free area) |
| Overview | drag a frame (tab or empty interior) | Reposition the desktop over the windows |
| Overview | drop a frame onto another | Combine them |
| Overview | right-click (two-finger tap) | New desktop at that spot |
| Overview | tab buttons ⊞ / ✕ | Tile / abandon |
| Overview | minimized shelf (bottom) | Restore that window |

## Keyboard (Super = logo key)

| Keys | Action |
|---|---|
| Tap Super, Super+Space | App launcher |
| Super+Return | Terminal |
| Super+Q, Alt+F4 | Close window |
| Super+M / Super+Shift+M | Minimize / restore last minimized |
| Super+Up / Super+F | Maximize-restore / fullscreen |
| Super+Tab, Super+W | Toggle overview |
| Super+Ctrl+Arrows | Go to the desktop in that direction (or one screen that way) |
| Super+Ctrl+Shift+Arrows | Move the focused window to that desktop and follow |
| Super+Alt+Arrows | Pan a third of a screen |
| Super+Ctrl+D / Super+Ctrl+F4 | Make the current view a desktop / abandon the current desktop |
| Super+T | Tile the current desktop (or view) |
| Super+1…9 | Go to desktop N |
| Super+Shift+E, Ctrl+Alt+Backspace | Quit |
| Ctrl+Alt+F1…F12 | Switch VT |
| Overview: arrows, Return, Escape, Delete, T, M, N, +/- | Jump, enter, back, abandon, tile, combine, new desktop, zoom |

## Configuration

dwl-style: edit `src/config.h` (sizes, colours, gesture tuning, commands) and the
binding table in `src/input.c`, then rebuild.

## Protocols

xdg-shell, xdg-decoration and KDE server-decoration (server-side by default;
clients that negotiate neither keep their own decorations), layer-shell (bars,
launchers, wallpapers, notifications), foreign-toplevel management (taskbars can
restore minimized windows), xdg-activation, pointer-gestures, relative-pointer,
cursor-shape, screencopy/export-dmabuf (grim), data-control, primary selection,
viewporter, fractional-scale, presentation-time, single-pixel-buffer, idle-notify,
idle-inhibit, virtual-keyboard.

## Limitations

No Xwayland, no output-configuration protocol (set the scale with `-S`), no session
lock, full repaint of an output per changed frame (no damage regions), no window
open/close animations.
