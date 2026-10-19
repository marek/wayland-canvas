# Super+N makes a missing desktop beside desktop 1

Date: 2026-10-18 19:30 EDT

## Context

Super+1..9 goes to desktop N, and first makes it when it does not exist.
A desktop made this way goes beside desktop 1 in a fixed order, clockwise
from the top: the four sides first (up, right, down, left), then the four
corners (top-right, bottom-right, bottom-left, top-left). Desktop N wants
slot N-1 of that order; a slot any existing desktop covers is passed over
for the next free one, and with every slot of the ring taken the same
order is tried one ring further out. A desktop made this way never
overlaps another.

The contracts are [../intents/canvas/desktops.md](../intents/canvas/desktops.md)
and [../intents/input/keyboard-and-pointer.md](../intents/input/keyboard-and-pointer.md).

## Tasks

- [x] The intent: the slot order and the rings in desktops.md's Create, and the Super+1..9 row
- [x] Super+N for a missing desktop makes it in its slot, or the next free one, rings outwards, and goes there
- [x] A scenario proves it: Super+2..5 from desktop 1 alone make desktops above, right of, below and left of it, and a covered slot is passed over; `tools/check.sh` green
- [ ] On screen (the user)
