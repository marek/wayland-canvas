---
name: robust-architecture
description: Two patterns for less fragile code in wayland-canvas - derive from the source of truth for the thing in question (each output reads its own camera and usable box, a window is drawn from its own state), and give every client and every input path the same shared mechanism instead of a special path (no special cases for one client; gestures, keys and mouse drive the same canvas actions). Use when deciding where a piece of state lives, when writing anything that renders or decides for another output or window, and when tempted to add a one-client or one-output special case.
---

# Robust architecture: one source of truth, one shared mechanism

## 1. Read the source of truth for the thing itself

A per-output value read from another output, or a global remembered
"because it was at hand", is right in the single-output run and wrong
the moment a second output appears.

**The rule.** Whatever is derived for an output — its camera, its
usable box, its visibility of a window, its scale — is derived from the
state that output owns, never from a sibling's or a remembered global.
A window is drawn and hit-tested from its own view state. If the needed
fact is not reachable from the owner, extend what the owner holds
rather than approximating from what happens to be nearby.

Corollaries:

- **Hit-test with the same camera the renderer uses** (the quantized
  one): two slightly different cameras put the click a few pixels from
  the window that was drawn there.
- **Don't compare through a converted form when the original is
  available.** Canvas↔output coordinates are round-trips; compare and
  decide on the original, and expect the converted form to differ by
  its precision.
- **A single-output run hides this class of bug** the same way a
  listen server hides netcode bugs: test with two outputs (the
  scenarios and a nested run with a second headless output can).

## 2. One mechanism for every actor

Every client speaks the same protocols through the same paths; every
input device drives the same actions.

**The rule.** When a new kind of actor or case arrives, route it
through the mechanism the existing ones use. If the existing mechanism
cannot carry it, extend the mechanism for everyone. A special path is
allowed only for something that is genuinely a different thing (a
layer-shell surface is not a toplevel; a client that negotiates no
decoration protocol keeping its own decorations is the protocol, not an
exception to it).

This pays off twice: a hole one client trips over is a hole every
client can hit, so the odd client becomes a test of the real path; and
a rule changed once changes for all of them with no second edit.

The same holds outside clients:

- Gestures, keys and mouse drive the same canvas actions
  (`canvas_goto_direction`, `overview_tile`, ...) — no gesture-only
  copy of a keyboard action.
- Every output runs the same mode state machine; there is no
  "the overview" as a global.
- Per-window state (maximized, minimized, ssd) is kept on the view and
  read by every consumer of it, not re-derived per consumer.

## Before shipping

- Would this still be right with a second output, a second desktop, a
  client that negotiates no decoration protocol?
- Is there a copy of this fact somewhere that can now disagree with it?
- Is this path only for one client, only for one output, only for the
  gesture path? Why?
