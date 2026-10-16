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

- [x] Root cause of a window collapsing into a bar after a few Super-pinch zooms in and out, reproduced headless ([bugs/zoom-collapsed-the-grid.md](../intents/bugs/zoom-collapsed-the-grid.md))
- [x] A settled scale tells the clients nothing: they keep the output's scale (fractional scale, preferred buffer scale), and the compositor scales their picture
- [x] A settled scale re-lays out nothing: no re-flow of a tiled grid, no new size for any window
- [x] On a scaled desktop the bars' inset (maximize, tiling, new-window placement) is the bars' size at that scale, not their size in screen pixels
- [x] A desktop's frame resizes around an exact centre: any number of zooms leaves it where it began at 100 % (found by the root cause: the rounded centre walked)
- [x] A tiled desktop's grid holds its windows through a zoom: a re-flow after the frame's edges pass a window's centre takes it with the rest (found by the root cause: such windows were dropped, still marked tiled)
- [x] Title bars are drawn for the zoom the camera rests at, not for the desktop a window's centre is in

## Reset

- [x] Super+0 returns the desktop shown to 100 %, the camera easing with it; `0` in the overview does it for the target desktop; both are named in a hint

## Views at any zoom

- [ ] A 2-finger scroll or a 3-finger pan never changes the zoom in the normal view: a free view keeps the zoom it was panned at and stays interactive; the snap onto a desktop is a fraction of the screen as shown
- [ ] Super+0 returns a free view to 100 %; pinning a free view makes a desktop at its zoom; leaving the overview into a free spot fills the screen with it
- [ ] The scale reaches down to 25 %, and the overview always opens further out than the view it leaves

## Verification

- [x] The gestures scenario zooms a desktop in and out six times with a floating, a maximized and a tiled window: sizes, places and the clients' scale never change, and nothing collapses
- [x] The scenario resets with Super+0 and with `0` in the overview
- [x] Screenshots at 50 %, 125 %, 200 % and after the reset, inspected
- [x] README and `-h` name Super+0; warning-free build, every suite green
- [ ] The gestures scenario at 25 %: a 2-finger scroll, a plain 2-finger pinch and a 3-finger pan off the desktop leave the zoom alone and the view interactive; the overview round trip keeps the free view's zoom; Super+0 brings it to 100 %
- [ ] On screen: Super + 2-finger pinch on the touchpad, nested and on a TTY, zooming in and out with Super+0 to come back (the user)
