# Case study: the screencopy that never returned

> A bug case study, where history is told on purpose. The present-tense
> rule it produced is in [../render/rendering.md](../render/rendering.md).

## Symptom

grim, run against the compositor, hung forever: no screenshot, no
error, no exit. The compositor itself kept running and repainting
normally.

## The exchange, and where it broke

| Side | Fact |
|---|---|
| grim | a screencopy copy is satisfied only when the output commits a frame with that copy attached |
| The compositor | it rendered from its own damage alone: something on the output changed, or nothing |
| An idle screen | nothing changed, no frame was ever committed, and grim waited forever |

wlroots had been asking for the frame all along: `needs_frame` on the
output, raised for screencopy and for software cursors.

## Root cause

The repaint policy knew only its own damage. A request for a frame
coming from outside — wlroots' `needs_frame` signal — was not wired to
rendering.

## Fix

The output renders when `needs_frame` is set, and a frame is scheduled
on that signal. [../render/rendering.md](../render/rendering.md) owns
the rule.

## Playbook

1. When a Wayland client blocks forever, list what the compositor owes
   it — a commit, a callback, an event — and check the compositor's
   side of that exchange before suspecting the client.
2. wlroots asks for work through signals. A policy that listens only
   to its own state will starve whichever client needs the signal it
   ignores; when wiring a new policy, enumerate the wlroots signals on
   the objects involved.
3. Software cursors ask the same way a screenshot does, so the fix is
   named for the signal, not for grim.

## Cross-links

- [../render/rendering.md](../render/rendering.md),
  [../platform/protocols.md](../platform/protocols.md), the
  `verification` skill.
