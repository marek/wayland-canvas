# Case study: the modal state wlroots erased before it read it

> A bug case study, where history is told on purpose. The present-tense
> rule it produced is in [../windows/x11.md](../windows/x11.md).

## Symptom

The xwayland scenario's modal-dialog checks failed now and then: the dialog
was laid out as a plain tiled window, on the board and not centred over the
window it belongs to, though the client had set `WM_TRANSIENT_FOR` and
`_NET_WM_STATE_MODAL` before it mapped it. Seen once in 27 runs, and again
with four suites loading the machine; a rerun passed.

## Hypotheses

1. The hints arrive after the map and the window is never laid out again.
   True for a parent set after the map, and fixed (`set_parent` lays the
   window out again), but not what the scenario did: its client set both
   hints before the map.
2. wlroots reads the hints late. It reads every property once, when the
   window is paired with its surface (`associate`), before the surface
   maps; the parent and the window type were always there.

## Root cause

Only the modal state was missing, and only when the client sent the hints
and the map in one flush. wlroots' window manager selects property changes
on a window when it handles the window's creation, so a property set
before that moment never reaches it as a change. It learns the hint at the
association. Between the two, the map notify makes it write `_NET_WM_STATE`
from its own fields, none of which it has read yet: the client's modal state
is erased from the X server, and the association then reads an empty list.

A client that waited for round trips between its hints (the scenario's
client interned atoms between them) gave the window manager time to select
property changes first, which is why it failed only on a loaded machine.

## Fix

The compositor reads `_NET_WM_STATE` itself at the map request, which comes
before the map notify, and records `_NET_WM_STATE_MODAL` in wlroots' own
field, so wlroots' rewrite keeps it. The scenario's client now sends a
dialog's hints and its map in one flush, which fails every time without it.

## Playbook

1. A check that passes only when the client is slow is a race between the
   client and the window manager, not a flaky check: make the client fast
   and the failure is every run.
2. Log the order of the window manager's events (`xwayland->user_event_handler`
   sees each before wlroots does) against what wlroots had read at each
   point: create, map request, map notify, the surface-id message, associate.
3. wlroots writes state it has not read; read what the client asked for
   before the first write.
