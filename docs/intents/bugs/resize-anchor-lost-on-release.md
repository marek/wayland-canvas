# Case study: the resize anchor lost on release

> A bug case study, where history is told on purpose. The present-tense
> rule it produced is in
> [../windows/window-management.md](../windows/window-management.md).

## Symptom

Resizing a window by its left or top edge made it drift: instead of
the dragged edge moving while the opposite edge stayed fixed, the
whole window crept away. Right- and bottom-edge resizes were correct.

## Why only left and top

Anchoring a left or top edge means holding the opposite edge fixed by
moving the window's position as its size changes, so every size change
needs its compensation — including the last one. For a right or bottom
edge the window's position never changes, and a late size commit costs
nothing.

## Root cause

A resize is a two-party exchange: the compositor sends a configure,
the client commits the new size, and the client's final commit — the
one that ends the resize — arrives after the pointer button release,
by which time the compositor had already stopped anchoring. The
compensation for that final size change was missing: anchoring was
tied to the input grab's lifetime, and a resize actually ends when the
client acks the configure that closed it.

## Fix

Anchoring lasts until the client acks the configure that ended the
resize. [../windows/window-management.md](../windows/window-management.md)
owns the rule.

## Playbook

1. Input-driven geometry settles asynchronously: the grab ends at
   input time, the window reaches its final geometry at protocol time.
   Tie geometry invariants to the protocol exchange (configure and
   ack), not to the grab.
2. A bug that appears "on release" is usually an ordering bug between
   two lifetimes. Name both lifetimes before patching either.
3. A symptom that picks out one edge or one corner is naming the
   quantity that is compensated and the one that is not; read the
   asymmetry.

## Cross-links

- [../windows/window-management.md](../windows/window-management.md),
  [../windows/title-bars.md](../windows/title-bars.md), the
  `debugging-discipline` skill.
