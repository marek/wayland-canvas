# X11 windows take the pointer where they are drawn

Date: 2026-10-18 19:06 EDT

## Context

An X11 window takes the pointer wherever the compositor draws it: a click
on a dialog lands on the dialog, not on the window it opened over, and a
window at any zoom and anywhere on the canvas answers the pointer over its
whole surface. Xwayland picks the X11 window for each pointer event from
its own stacking order and window positions, and keeps its pointer inside
its own screen, the outputs' layout box. So the compositor keeps both in
step with the canvas: X's stack follows the canvas's, and X positions are
canvas positions less one offset, which moves whenever the pointer over an
X11 window would leave X's screen. Keys need nothing: they follow the X
input focus.

The contract is [../intents/windows/x11.md](../intents/windows/x11.md).

## Tasks

- [ ] The intent states how X's stack and positions follow the canvas, and the corner cases (x11.md)
- [ ] X's stacking order follows the compositor's: a mapped or raised window is on top in X too, a minimized one at the bottom
- [ ] X positions are canvas positions less the offset, sent wherever a window is placed, moved or resized; an override-redirect window is placed back through the same offset
- [ ] The offset moves when the pointer over an X11 window would leave X's screen, every X11 window moving with it
- [ ] The xwayland scenario proves it: a window opened over another takes the click, the window under it takes clicks where it shows, a window opened past X's screen takes the click, a window larger than X's screen takes clicks at its far corners with the right coordinates, a minimized window takes no click; `tools/check.sh` green
- [ ] Krita's New and Open dialogs take the pointer in a headless run, read back from X, and with Krita maximized at 25 % its whole window does
- [ ] On screen, in the session: Krita's dialogs and a zoomed-out desktop (the user)
