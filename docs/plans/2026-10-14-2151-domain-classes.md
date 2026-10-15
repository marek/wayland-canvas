# Domain classes

Date: 2026-10-14 21:51 EDT

## Context

The C++ conversion made each C function a member of the struct it took first: desktops stayed a struct with no
behaviour and eleven functions on `Server`, the camera and the overview were forty members of `Output`, gestures and
pointer grabs were shared public fields switched on a kind enum, and destroying a desktop cleared five pointers held
elsewhere by hand. This plan reorganises the code around the compositor's concepts, one commit per concept with the
scenario suites green after each, behaviour unchanged.

## Rules

- [x] `platform/cpp-standards.md`: names for things that can disappear, `std::variant` state machines, one concept per class

## Concepts

- [ ] Desktops: `Desktop` with behaviour, `Desktops` as the only owner, `DesktopId` everywhere else; `Canvas` for window arrangements over canvas regions
- [ ] Camera: `CameraMotion` per output (jumps, eased pans, zoom transitions, momentum)
- [ ] Overview: `Overview` per output (home, zoom, target, the overview actions)
- [ ] Gestures: one state type per gesture in a `std::variant`; other code asks `Gestures`
- [ ] Pointer grabs: `PointerGrab`, one state type per grab in a `std::variant`; client move/resize requests begin grabs through it
- [ ] Bindings: the key table, its actions and the overview keys out of `Input`

## Verification

- [ ] After each concept: a warning-free build, the unit and scenario suites green, one commit
- [ ] At the end: a headless run with real clients and a clean shutdown; the intents and skills name the new classes
