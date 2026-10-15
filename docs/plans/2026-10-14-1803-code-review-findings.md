# Code review findings

Date: 2026-10-14 18:03 EDT

## Context

A reviewer read the compositor against the wlroots 0.20.2 sources after the first build and reported four bugs
the scenarios had not reached, most serious first, plus two areas to check. The rules each fix follows are in
[platform/wlroots.md](../intents/platform/wlroots.md), [input/gestures.md](../intents/input/gestures.md) and
[platform/outputs.md](../intents/platform/outputs.md).

## Fixes

- [x] A client drag-and-drop with a valid serial recursed until the stack overflowed: the handler re-requested the drag; it now starts it with `wlr_seat_start_pointer_drag`
- [x] A desktop destroyed while a 4-finger swipe was sliding it away dereferenced NULL on the next update: destroying the desktop ends the swipe (`tests/scenarios/gestures.cpp` abandons it mid-swipe)
- [x] An output destroyed mid-gesture dropped the end of a gesture forwarded to a client and left a swiped desktop drawn shifted: forwarded gestures keep their kind, an abandon swipe's offset is reset
- [x] A closed window stayed the double-click candidate and could turn the next window's first title click into a double-click: unmapping clears it (`tests/scenarios/input.cpp` checks it)
- [x] Layer surfaces whose output was unplugged: every handler already copes with no output
- [x] A surface newly drawn on two outputs of different scale was told the scale of the last one drawn: it is told the largest scale of the outputs it is on

## Owed

- [ ] A drag-and-drop between two clients in a scenario, or tried on screen with a file or a browser tab (the drag fix has not been run)
- [ ] An output unplugged mid-gesture in a scenario (a second headless output destroyed while a gesture is forwarded)
- [ ] Two outputs of different scale in a scenario, a popup opening across them
