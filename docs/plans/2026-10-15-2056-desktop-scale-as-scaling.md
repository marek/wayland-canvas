# Desktop scale as scaling

Date: 2026-10-15 20:56 EDT

## Context

A desktop's scale, set with Super and a two-finger pinch, scales the picture of the desktop and nothing else: the
windows on it keep their size, place and layout, and their clients are never told, so zooming in and out any number of
times leaves every window as it was. The compositor scales what the windows drew, so above 100 % they look soft; a
reset returns the desktop to 100 %, where every window is drawn one to one again. A window that shrinks into a thin
bar and disappears after a few zooms in and out is root-caused and covered by a scenario. The rules:
[canvas/desktops.md](../intents/canvas/desktops.md), [render/hidpi.md](../intents/render/hidpi.md),
[input/gestures.md](../intents/input/gestures.md),
[input/keyboard-and-pointer.md](../intents/input/keyboard-and-pointer.md),
[canvas/overview.md](../intents/canvas/overview.md),
[canvas/infinite-canvas.md](../intents/canvas/infinite-canvas.md).

## Scaling

- [ ] Root cause of a window collapsing into a bar after a few Super-pinch zooms in and out, reproduced headless
- [ ] A settled scale tells the clients nothing: they keep the output's scale (fractional scale, preferred buffer scale), and the compositor scales their picture
- [ ] A settled scale re-lays out nothing: no re-flow of a tiled grid, no new size for any window
- [ ] On a scaled desktop the bars' inset (maximize, tiling, new-window placement) is the bars' size at that scale, not their size in screen pixels

## Reset

- [ ] Super+0 returns the desktop shown to 100 %, the camera easing with it; `0` in the overview does it for the target desktop; both are named in a hint

## Verification

- [ ] The gestures scenario zooms a desktop in and out six times with a floating, a maximized and a tiled window: sizes, places and the clients' scale never change, and nothing collapses
- [ ] The scenario resets with Super+0 and with `0` in the overview
- [ ] Screenshots at 50 %, 125 %, 200 % and after the reset, inspected
- [ ] README and `-h` name Super+0; warning-free build, every suite green
