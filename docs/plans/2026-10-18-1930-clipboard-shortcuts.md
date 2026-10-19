# Super+C and Super+V copy and paste in every app

Date: 2026-10-18 19:30 EDT

## Context

Super+C copies and Super+V pastes in whichever app has the keyboard,
terminals included: the compositor sends the focused client Ctrl+Insert
for a copy and Shift+Insert for a paste, the chords GTK, Qt, Chrome and
Electron apps take natively and the user's terminals are set up to take.
Both are shortcuts like any other: listed in the settings window and
rebindable there and in `canvas.conf`.

The contract is
[../intents/input/keyboard-and-pointer.md](../intents/input/keyboard-and-pointer.md).

## Tasks

- [x] The intent: the two actions, the chords they send, rebindable like the rest
- [x] `copy` and `paste` actions under Super+C and Super+V, in the settings window, sending Ctrl+Insert and Shift+Insert to the surface with the keyboard with only that modifier held
- [x] A scenario proves a client with the keyboard receives each chord; `tools/check.sh` green
- [ ] On screen: Super+C and Super+V in foot, Krita and Chrome (the user)
