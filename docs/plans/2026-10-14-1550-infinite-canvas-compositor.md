# Infinite canvas compositor

Date: 2026-10-14 15:50 EDT

## Context

The compositor: one infinite canvas holds every window, and virtual desktops are screen-sized frames placed over it.
Three fingers pan the canvas anywhere; four fingers up open a zoomed-out overview in which the desktops show as dashed
frames that can be moved over the windows freely. In the overview three fingers hop to the nearest desktop in the
swipe's direction, and gestures abandon a desktop, combine two and tile a desktop's windows. Any area of the canvas is
usable, desktop or not. Windows have Windows-style title bars with minimize, maximize and close.

What was decided is in the intents: [canvas/infinite-canvas.md](../intents/canvas/infinite-canvas.md),
[canvas/desktops.md](../intents/canvas/desktops.md), [canvas/overview.md](../intents/canvas/overview.md),
[input/gestures.md](../intents/input/gestures.md), [input/keyboard-and-pointer.md](../intents/input/keyboard-and-pointer.md),
[windows/title-bars.md](../intents/windows/title-bars.md), [windows/window-management.md](../intents/windows/window-management.md),
[render/rendering.md](../intents/render/rendering.md), [render/hidpi.md](../intents/render/hidpi.md),
[platform/wlroots.md](../intents/platform/wlroots.md), [platform/protocols.md](../intents/platform/protocols.md),
[platform/outputs.md](../intents/platform/outputs.md), [workspace/build-and-run.md](../intents/workspace/build-and-run.md).

## Build

- [x] Repository in `~/workspaces/wayland-canvas`: meson project, wlroots 0.20.2 as a wrap built statically (no system wlroots, no root); meson and ninja in a repo-local `.venv`
- [x] Own renderer with a per-output camera (centre and zoom), pixel-grid snap at 1:1, full repaint of a changed output, nothing drawn when idle
- [x] Frames also when wlroots asks (`needs_frame`), so grim and other screencopy clients get an image
- [x] xdg-shell windows: placement, focus and raise, stacking, popups unconstrained to the screen
- [x] Windows 11 title bars: minimize, maximize/restore, close; hover and pressed states; close hover red; active and inactive colours
- [x] Server-side decorations only when negotiated (xdg-decoration, KDE server-decoration); clients negotiating neither keep their own
- [x] Title bar actions: double-click maximizes, dragging moves (a maximized window restores under the cursor), edges and corners resize
- [x] Resizing from a left or top edge keeps the opposite edge fixed until the client acks the configure that ended the resize
- [x] Maximize, fullscreen, minimize and every restore path (overview shelf, Super+Shift+M, foreign-toplevel, activation)
- [x] New windows open inside the usable area of the screen being looked at, title bar always on screen
- [x] 3-finger swipe pans anywhere, natural direction, momentum, snap onto a nearby desktop
- [x] 4-finger swipe up opens the overview following the fingers; 4-finger down zooms into the target
- [x] Desktops as dashed frames; frames dragged over windows never move them
- [x] Overview 3-finger swipe jumps to the closest desktop in the swipe's direction, or one screen that way to a free spot
- [x] Abandon: 4-finger sideways swipe, the tab's abandon button, Delete
- [x] Combine: 3-finger pinch in, dropping one frame onto another, `m`
- [x] Tile: 3-finger pinch out, the tab's tile button, `t`, Super+T
- [x] Free areas: clicking empty overview canvas goes there; free pan; free hops
- [x] Overview UI: tabs, help line, minimized shelf, transient hints, previews while pinching and dragging
- [x] Keyboard shortcuts, every canvas one including Super so applications keep Ctrl+arrows, Ctrl+D, Alt+arrows, Ctrl+F4
- [x] Layer shell: bars with exclusive zones (checked with a GTK4 layer-shell bar), wallpapers, overlays; the top layer hides under fullscreen
- [x] Output scale: `-S`, `WAYLAND_CANVAS_SCALE`, else 2 on dense panels; checked crisp at 2x
- [x] Touchpad settings: tap-to-click, natural scrolling, disable-while-typing
- [x] Virtual keyboard support (wtype drives shortcuts against a running compositor)
- [x] Pointer grabs return to passthrough when they end (found by the device-level scenario)
- [x] Unit tests for the canvas math (direction choice, tiling, zoom paths)
- [x] Headless scenarios with real foot clients: gestures, keyboard and pointer through fake devices (moved into `tests/scenarios/` by the adoption plan)
- [x] README: build, run, gestures, mouse, keys

## Owed

- [ ] On-screen run on real hardware from a TTY: touchpad gestures, the 2x panel, a real Super key (the user, on their machine)
- [ ] Nested run inside Hyprland (the user)

## Decisions for the user

- [ ] Xwayland for X11 applications (the user to decide)
- [ ] Output configuration protocol, so wlr-randr and kanshi can set modes and scale at run time (the user to decide)
- [ ] Session lock for screen lockers (the user to decide)
- [ ] Gamma control for night light (the user to decide)
- [ ] Repainting only damaged regions instead of whole outputs (the user to decide)
- [ ] Window open and close animations (the user to decide)
- [ ] A configuration file read at run time instead of `src/config.h` (the user to decide)
