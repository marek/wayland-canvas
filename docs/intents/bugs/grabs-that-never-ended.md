# Case study: the grabs that never ended

> A bug case study, where history is told on purpose. The present-tense
> rules it produced are in
> [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md).

## Symptom

Two faults, found in one session of device-level scenario testing:

1. Every pointer grab stayed grabbed: after a move or a resize the seat
   never returned to passthrough, so the pointer stopped reaching
   clients.
2. Canvas shortcuts were bound without Super — plain Ctrl+arrows,
   Ctrl+D and friends were swallowed by the compositor before
   applications saw them.

## Why the earlier tests had shown neither

The unit tests called the action handlers directly, so the seat, its
grab state machine and the binding table's modifier matching were all
bypassed. The scenario tests announce fake wlr_keyboard and
wlr_pointer devices to the headless backend, and the injected events
travel the same path real input does.

## Root cause

1. The pointer grab's cancel path did not reset the seat to
   passthrough: every exit path of a grab must end somewhere, and this
   one ended nowhere.
2. The binding table listed canvas actions without the Super modifier.

## Rules it produced

- Input behaviour is tested through devices announced to the backend,
  never by calling handlers directly.
- Every grab ends on every path: release, cancel, and the client or
  object dying under it.
- Every canvas shortcut includes Super
  ([../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)).

## Playbook

1. When a test harness calls a handler directly, everything between
   the device and the handler — seat state, grabs, modifier matching —
   is untested. Test at the boundary the real world uses.
2. Audit the exit paths of any state machine you add: for each state,
   name the event that leaves it, including the abnormal ones.
3. When an application "loses" a key or a button, suspect a binding or
   a grab that did not end before suspecting the application.

## Cross-links

- [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md),
  [../input/gestures.md](../input/gestures.md),
  [../platform/wlroots.md](../platform/wlroots.md), the `verification`
  skill, the test venues in `AGENTS.md`.
