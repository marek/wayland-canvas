---
name: canvas-autopilot
description: Work through the open wayland-canvas plans in docs/plans continuously without stopping - pick the next open box that does not need the user, read its intent, do it, verify, commit, tick, move on. Use when the user says to work through the plans, continue, keep going, or carry on with the remaining items. Do not use, and do not start plan items, when they ask a question or give a different task.
---

# Canvas autopilot

The standing instruction is **do not stop**: hours were lost when turns
ended with a summary while work remained. Ending a turn with "next I
will..." or a status list while plan items remain is the failure this
skill exists to prevent.

The user has also said: **do not start plan items while they are asking
about something else.** Autopilot is therefore something they switch
on, not a standing state.

## Switching on and off

- **On**: when the user says to work through the plans (or "continue",
  "keep going" about plan work), create `.claude/autopilot.on`. While
  it exists, the Stop hook (`tools/hooks/autopilot-stop.sh`, wired in
  `.claude/settings.json`) refuses to end a turn if any plan in
  `docs/plans/` has an open box that does not wait for a person, and
  names the next one. A box waits for a person when its title ends in
  `(the user)` or `(the user to decide)`; `tools/check-plans.sh` fails
  any other wording.
- **A question while on** ("what is left?", "progress check", "why X?"):
  answer it as the `answering-questions` skill says, from the plan
  files where it is about progress, and keep working in the same turn.
- **Off**: remove `.claude/autopilot.on` when the user says stop, when
  they give a different task or say to focus on one thing only, or when
  every remaining box needs them. A different task is done on its own;
  plan work does not ride along.
- **Paused on a real blocker**: write the blocker to
  `.claude/autopilot.pause` and stop. The next message from the user
  removes that file.

## Loop

1. Read `docs/plans/README.md`, then the plans with open boxes. Pick
   the next open box that does not need a decision from the user (skip
   the ones ending in `(the user)` or `(the user to decide)`).
2. Read the intent doc nearest the item (`docs/intents/README.md`
   finds it). If the item settles something the doc does not say,
   update the doc first (the `intent-docs` skill).
3. Do the work, in the role that fits (`docs/roles/`). wlroots'
   `tinywl` and `wlr_scene` are the reference for protocol handling;
   Windows 11 for the title bars and the launcher.
4. Verify (the `verification` skill): `tools/check.sh`, plus a nested
   or TTY run as the change calls for. Nothing temporary left in the
   tree; no compositor or client process left running.
5. Commit (the `incremental-commits` skill): source, tests and docs as
   the phase's proven commit. Never push.
6. Tick the box in the plan file, commit the tick, and **go straight
   to step 1**. No summary turn in between.

Decisions and questions for the user are written into the plan as open
boxes that say so, and are given to them together when nothing else is
left.

Do not ask for permission ("Don't ask for permission. Just proceed"):
make the call, do the work, note the decision.

## Already decided (never ask again, never call them optional)

Server-side title bars in the Windows 11 dark look (32 px bar, 46 px
caption buttons); the XDG desktop entry as the standard way to add
launcher items; wlroots 0.20 fetched as a meson subproject; the own
renderer (wlr_scene cannot zoom a tree); free areas of the canvas as
first-class; every canvas shortcut includes Super.

## Still the user's call

A taskbar; Xwayland; an output-configuration protocol (wlr-randr,
kanshi); session lock; gamma control; damage-region rendering; window
open/close animations; a configuration file instead of `src/core/config.h`;
a run on real hardware (TTY, touchpad gestures, the 2x panel); anything
that pushes. Write these into the plan and keep working on something
else.

## Hard rules

- `AGENTS.md` binds everything here: three strands per change, repo
  boundaries, author identity, no push.
- Protocol-facing changes move their intent doc first.
- Scratch under `tmp/`; kill only your own PIDs; delete temporary
  files.
- Brief progress lines while working are fine; they are not a reason to
  stop.
