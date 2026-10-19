# Case study: the X11 click lost after a jump

> A bug case study, where history is told on purpose. The present-tense
> rule it produced is in [../windows/x11.md](../windows/x11.md).

## Symptom

At 25 %, the xwayland scenario moves the pointer across a window larger
than X's screen and presses with no motion between. The press usually
reached the client where it was aimed; in three runs of four on one day it
never reached it at all, with the same scenario binary that had passed the
check that morning.

## What was there before

The offset that keeps X's pointer on its screen moves when the pointer would
leave it, and every X11 window is told its new X position. The pointer event
that caused the move could reach X first, so a timer told the pointer's
place again 16 ms later, as a wayland motion of a 1/256 px step.

## Evidence

Logging each offset move and each retell, and asking X where its pointer was
just before the press (`xcb_query_pointer` on the root, from the scenario):

- the retell fired on time, 17 ms after the move, over the X11 window;
- in a failing run X still had its pointer at its screen's edge
  (`1279,352`, child none) where it should have been `640,360` over the
  window; in a passing run it was at `640,360`.

So the retell had reached X before the windows' moves had: X put the
pointer by the old picture again, then moved the windows away under it.

## Root cause

X hears of its windows' moves on the window manager's connection and of the
pointer on Xwayland's wayland connection, and nothing orders the two. A
delay only made one order likelier. The same race sat under any move of an
X11 window while the pointer was over it, offset or not.

## Fix

After every batch of moves, the window manager's own connection warps X's
pointer to where the cursor is over the moved windows. X takes one
connection's requests in order, so the warp lands after the moves whichever
of the two connections it read first. The timer and its retell are gone.

## Playbook

1. Two connections into one server have no order between them; a delay
   between them is a probability, not a fix. Put the request that depends
   on another on the same connection.
2. Ask X itself: `xcb_query_pointer` on the root gives X's pointer and the
   window under it, which is what decides a press, whatever the compositor
   sent.
3. A check that fails some runs and not others with the same binary is
   timing; run it until it fails with logging on, rather than once.

## Cross-links

- [../windows/x11.md](../windows/x11.md), the `debugging-discipline` skill.
