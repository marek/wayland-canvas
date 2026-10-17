# Touchpad gestures

> Owns the touchpad gesture table, the presets that reassign it, and the
> rules every gesture follows: follow-the-fingers transitions, commit or
> spring back, and which gestures reach clients. What the gestures do is
> [../canvas/infinite-canvas.md](../canvas/infinite-canvas.md),
> [../canvas/desktops.md](../canvas/desktops.md) and
> [../canvas/overview.md](../canvas/overview.md); the choice is made in
> [config-file.md](config-file.md).

## Requirement

Multi-finger gestures are the compositor's vocabulary for moving over
the canvas: a 3-finger swipe hops to the nearest desktop in its
direction — in the normal view only ever onto a desktop, in the
zoomed-out one onto a free spot too — and a 4-finger swipe up opens the
zoomed-out canvas. The user picks the preset that assigns the finger
counts, or turns them off, and whether Super + a 2-finger pinch scales a
desktop. Only 2-finger gestures and finger counts the compositor does not
bind are forwarded to clients.

## Presets

| Preset | `swipes =` | The table below |
|---|---|---|
| Three-finger hops (the default) | `three-finger-hops` | As written |
| Four-finger hops | `four-finger-hops` | Every 3 and 4 swapped: 4 fingers hop in both modes, and pinch to tile or combine in the overview; 3 fingers up open the overview, and in it 3 down go in, 3 up zoom further, 3 sideways abandon |
| Off | `off` | No 3- or 4-finger compositor gesture: in the normal view they reach the client under the pointer like 2-finger ones; in the overview they do nothing. Keys and the pointer are unchanged |

| Pinch scale | `pinch-scale =` | Super + 2-finger pinch |
|---|---|---|
| On (the default) | `on` | Scales a desktop, as the table says |
| Off | `off` | A plain 2-finger pinch: the client's in the normal view, the overview's zoom around the cursor in the overview |

A change applies from the next gesture; one in progress keeps going.
Finger counts below are the default preset's.

## Behavior

| Mode | Fingers | Motion | Action |
|---|---|---|---|
| Normal | 3 | Swipe | Hop between desktops: ease onto the nearest desktop in the swipe's direction, at its scale; none that way, the view stays and a hint says so ([../canvas/infinite-canvas.md](../canvas/infinite-canvas.md)) |
| Normal | 4 | Swipe up | Zoom out into the overview, following the fingers ([../canvas/overview.md](../canvas/overview.md)) |
| Normal | 3 or 4 | Pinch | Treated as a swipe of the same finger count |
| Normal | 2 | Pinch with Super held | Scale the desktop shown, following the fingers: pinching in scales it down, spreading scales it up ([../canvas/desktops.md](../canvas/desktops.md)) |
| Normal | 2 | Pinch or hold, without Super | Forwarded to the client under the pointer |
| Overview | 3 | Swipe | Hop: jump to the closest desktop in that direction ([../canvas/overview.md](../canvas/overview.md)) |
| Overview | 4 | Swipe down | Zoom into the target, following the fingers ([../canvas/overview.md](../canvas/overview.md)) |
| Overview | 4 | Swipe up | Zoom out further |
| Overview | 4 | Swipe sideways | Abandon the desktop under the pointer, else the target: the frame slides and fades with the fingers ([../canvas/desktops.md](../canvas/desktops.md)) |
| Overview | 3 | Pinch out (spread) | Switch tiling on or off for that desktop ([../canvas/desktops.md](../canvas/desktops.md)) |
| Overview | 3 | Pinch in (gather) | Combine the nearest other desktop into that one ([../canvas/desktops.md](../canvas/desktops.md)) |
| Overview | 2 | Pinch with Super held | Scale the desktop under the cursor, else the target desktop; its frame grows or shrinks with the fingers |
| Overview | 2 | Pinch / scroll | Zoom around the cursor / pan; Ctrl+scroll zooms |

Rules that hold across the table:

- **Hops go where the fingers point.** A 3-finger swipe to the left
  hops to the desktop on the left, in either mode.
- **Follow the fingers.** A gesture that drives a zoom transition moves
  the camera along the zoom path continuously; nothing jumps.
- **Commit or spring back.** Release past a threshold of progress — or
  a flick onward — commits the transition; released earlier, the camera
  animates back. The abandon swipe commits past a sideways travel
  distance; short of it the frame springs back. All thresholds are
  compile-time constants in `src/core/config.h`.
- **One action per gesture.** A hop triggers once past its travel
  threshold; a second jump takes a new gesture. A swipe made while a
  hop still eases starts from where that hop lands, so quick swipes walk
  desktop by desktop.
- **Previews while pinching.** A 3-finger pinch in tile or combine
  range shows what release will do; backing out of range clears it
  ([../canvas/overview.md](../canvas/overview.md)).
- **Super decides a 2-finger pinch at its start.** Held as the pinch
  begins, the pinch scales a desktop and never reaches a client; letting
  Super go mid-pinch changes nothing, and the pinch never counts as a Super
  tap. On a free view the view is pinned as a desktop first. The view
  follows the scale live and settles on release; the windows are never
  told ([../canvas/desktops.md](../canvas/desktops.md)).
- **Forwarding.** Only an interactive output
  ([../canvas/infinite-canvas.md](../canvas/infinite-canvas.md)) — resting
  at its zoom, no transition in progress — forwards gestures to clients
  (2-finger gestures and unbound finger counts, via the
  pointer-gestures protocol, [../platform/protocols.md](../platform/protocols.md)).
  In the overview the compositor owns every gesture.

## Design considerations

- **3-finger hop, 4-finger up** is the default interface. In the normal
  view a hop lands only on a desktop, never in empty canvas or between
  places, so a swipe cannot lose the user's place; the zoomed-out canvas
  is where desktops are arranged and free spots targeted. Panning freely
  stays with the pointer and keys: a drag or a 2-finger scroll over empty
  canvas, Super+Alt+arrows.
- **Presets, not single gestures**: each preset keeps the vocabulary
  whole — the count that hops also tiles and combines, the other zooms and
  abandons — so no choice leaves a gesture doing two things. No preset pans
  freely: a free pan loses the user's place.
- **Sloppy pinches are swipes.** At 3 and 4 fingers, touchpad gesture
  classification is unreliable; the finger count decides the intent,
  so a misclassified pinch still hops.
- **3- and 4-finger gestures never reach clients, unless the preset is
  off.** They are the compositor's navigation vocabulary; a client that
  grabbed them could swallow canvas movement. 2-finger gestures stay the
  client's.

## Error states, corner cases

- **A gesture over a client window in normal mode**: 3- and 4-finger
  gestures still act on the canvas; 2-finger gestures are the client's.
- **A zoom gesture released at the threshold**: past it commits.
- **Zooming out past the bound**: the zoom clamps; the gesture keeps
  tracking without effect ([../canvas/overview.md](../canvas/overview.md)).
- **An abandon swipe with no desktop under the pointer and no target
  desktop**: ignored.
- **A host compositor binds the same gestures**: nested, the gestures
  may not arrive at all — [../workspace/build-and-run.md](../workspace/build-and-run.md).

## Cross-links

- [../canvas/infinite-canvas.md](../canvas/infinite-canvas.md),
  [../canvas/desktops.md](../canvas/desktops.md),
  [../canvas/overview.md](../canvas/overview.md),
  [../input/keyboard-and-pointer.md](keyboard-and-pointer.md),
  [../platform/protocols.md](../platform/protocols.md),
  [../workspace/build-and-run.md](../workspace/build-and-run.md).
