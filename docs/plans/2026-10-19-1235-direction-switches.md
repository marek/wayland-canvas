# A switch for each direction

Date: 2026-10-19 12:35 EDT

## Context

One `invert` setting reverses every scroll and every swipe at once. The
directions are set apart instead, each with a switch of its own, so natural
scrolling in apps can stay while the view, the overview and the desktops
move the way the user expects:

- Mouse-wheel scrolling in apps: natural or not.
- Touchpad scrolling in apps: natural or not.
- Panning: which way Super and a scroll look away in the normal view, and a
  scroll pans the overview. A Super + drag keeps grabbing the canvas, and
  fingers on a touchscreen keep the content under them.
- The overview: which way four fingers open the overview, zoom further out
  and go back in.
- Desktop hops: which way a swipe hops.

Each starts at today's default. They are set in canvas.conf's `[gestures]`
section and in the settings window, and the single `invert` goes. The
touchscreen's swipes follow the overview and hop switches, as the
touchpad's do.

The rules go into [input/gestures.md](../intents/input/gestures.md),
[input/config-file.md](../intents/input/config-file.md),
[input/keyboard-and-pointer.md](../intents/input/keyboard-and-pointer.md) and
[shell/settings.md](../intents/shell/settings.md).

## Tasks

- [ ] The intents: gestures.md's direction table, config-file.md's keys, keyboard-and-pointer.md's scrolling rows, settings.md's switches
- [ ] canvas.conf reads and writes the five switches and `invert` is gone; the settings unit tests cover parsing, defaults, errors and edits
- [ ] The mouse wheel and the touchpad each take their own natural-scrolling switch
- [ ] Looking away and the overview's scroll pan follow the panning switch, whichever way the device scrolls apps
- [ ] The overview's swipes and the hops each follow their own switch
- [ ] The settings window shows the five switches; `settings.set` and the state name them
- [ ] The gestures scenario proves each switch flips its own direction and no other; the bar scenario's settings window shows the five
- [ ] `tools/check.sh` green
- [ ] On screen: each switch on the laptop's touchpad and a mouse (the user)
