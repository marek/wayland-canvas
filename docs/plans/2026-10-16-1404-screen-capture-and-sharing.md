# Screen capture and sharing

Date: 2026-10-16 14:04 EDT

## Context

Screenshots and recordings work the macOS way, plus the Print key of the user's previous desktop.

- Super+Shift+3 takes the whole screen.
- Super+Shift+4 takes a dragged region: a click without a drag takes the window under the pointer, Space switches
  to picking a window, Escape cancels.
- Super+Shift+5 opens a capture toolbar with shots and recordings of the screen, a window or a region, and options.
- Ctrl added to any of them copies to the clipboard instead of saving.
- Print opens the same picker as Super+Shift+4, and Shift+Print takes the whole screen.
- Alt+Print stops a running recording, or else opens the toolbar on its recording choices.

A shot is saved under `Screenshots` in the user's pictures directory, copied to the clipboard, and shown for a few
seconds as a thumbnail in the bottom-right corner: a click opens it, a swipe or a drag sends it away. A window is
captured alone — its title bar and content, nothing that overlaps it — at the output's scale, whatever the canvas
zoom. While a recording runs the bar shows a red dot that stops it; recordings go to the user's videos directory.

Chrome and other apps share a screen or a window through xdg-desktop-portal's ScreenCast over PipeWire. The
wlroots backend, xdg-desktop-portal-wlr, captures screens through wlr-screencopy (already offered) and windows
through ext-image-copy-capture with toplevel sources (to be offered). The session names itself `wayland-canvas` in
`XDG_CURRENT_DESKTOP`, puts that and `WAYLAND_DISPLAY` into the D-Bus and systemd activation environment at start,
and ships a portals config that sends ScreenCast and Screenshot to the wlr backend and the rest to gtk. The portal
is one service per user session: while Hyprland runs at the same time the portal is Hyprland's, so sharing works
from a wayland-canvas login, or nested under a private D-Bus session. The user installs xdg-desktop-portal-wlr; the
packages name it as an optional dependency.

## Capture core

- [x] Offscreen capture of an output region and of a single window (alone, at the output scale), shared by every path below
- [x] ext-image-copy-capture-v1 with output and toplevel sources, and ext-foreign-toplevel-list-v1, proven with a real client
- [x] The session: `XDG_CURRENT_DESKTOP=wayland-canvas:wlroots` for every client, the activation environment handed over at start (a login through systemd, never the host's bus), `wayland-canvas-portals.conf` shipped (wlr for ScreenCast and Screenshot, gtk for the rest), xdg-desktop-portal-wlr an optional dependency of both packages
- [x] On exit a login hands the portal back: the systemd values it replaced restored (unset ones unset) and the portals restarted, unless another session took them since — so Hyprland on another VT shares its screens again after a TTY run

## Shots and recordings

- [x] Super+Shift+3/4/5, Print, Shift+Print and Alt+Print, Ctrl for the clipboard
- [x] The picker: drag a region with its size shown, click for the window under the pointer, Space to pick windows, Escape to cancel
- [x] The capture toolbar: shots and recordings of the screen, a window or a region; options for where shots go, a timer and the pointer
- [x] After a shot: saved under Screenshots, copied to the clipboard, a corner thumbnail that a click opens and a swipe dismisses
- [x] Recording the screen, a window or a region into the videos directory, with a red dot in the bar that stops it, as Alt+Print and the toolbar do

## Verification

- [x] Scenarios: every key and toolbar action leaves the expected file and clipboard content, pixels checked against the screen; a window shot leaves out what overlaps the window; the picker and thumbnail flows; a recording starts, stops and leaves a playable file
- [x] Intents, README and `-h` state the keys and the portal setup; warning-free build, every suite green
- [ ] On screen: shots and a recording, and sharing a screen and a window in a Chrome presentation from a wayland-canvas session with xdg-desktop-portal-wlr installed (the user)
