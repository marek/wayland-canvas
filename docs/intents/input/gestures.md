# Touchpad gestures

> Owns the touchpad gesture table and the rules every gesture follows:
> follow-the-fingers transitions, commit or spring back, and which
> gestures reach clients. What the gestures do is
> [../canvas/infinite-canvas.md](../canvas/infinite-canvas.md),
> [../canvas/desktops.md](../canvas/desktops.md) and
> [../canvas/overview.md](../canvas/overview.md).

## Requirement

Multi-finger gestures are the compositor's vocabulary for moving over
the canvas: a 3-finger swipe pans the canvas anywhere, a 4-finger
swipe up opens the zoomed-out canvas. Only 2-finger gestures and
finger counts the compositor does not bind are forwarded to clients.

## Behavior

| Mode | Fingers | Motion | Action |
|---|---|---|---|
| Normal | 3 | Swipe | Pan the canvas anywhere, with momentum ([../canvas/infinite-canvas.md](../canvas/infinite-canvas.md)) |
| Normal | 4 | Swipe up | Zoom out into the overview, following the fingers ([../canvas/overview.md](../canvas/overview.md)) |
| Normal | 3 or 4 | Pinch | Treated as a swipe of the same finger count |
| Normal | 2 | Pinch with Super held | Zoom the canvas around the cursor, following the fingers: pinching in passes into the overview ([../canvas/overview.md](../canvas/overview.md)); spreading stops at 1:1 |
| Normal | 2 | Pinch or hold, without Super | Forwarded to the client under the pointer |
| Overview | 3 | Swipe | Hop: jump to the closest desktop in that direction ([../canvas/overview.md](../canvas/overview.md)) |
| Overview | 4 | Swipe down | Zoom into the target, following the fingers ([../canvas/overview.md](../canvas/overview.md)) |
| Overview | 4 | Swipe up | Zoom out further |
| Overview | 4 | Swipe sideways | Abandon the desktop under the pointer, else the target: the frame slides and fades with the fingers ([../canvas/desktops.md](../canvas/desktops.md)) |
| Overview | 3 | Pinch out (spread) | Tile that desktop's windows ([../canvas/desktops.md](../canvas/desktops.md)) |
| Overview | 3 | Pinch in (gather) | Combine the nearest other desktop into that one ([../canvas/desktops.md](../canvas/desktops.md)) |
| Overview | 2 | Pinch with Super held | Zoom around the cursor, on in up to 1:1; released above the overview's zoom ceiling it lands in the normal view with the point under the cursor kept there |
| Overview | 2 | Pinch / scroll | Zoom around the cursor / pan; Ctrl+scroll zooms |

Rules that hold across the table:

- **Natural direction.** Content follows the fingers, like dragging a
  map (a compile-time toggle in `src/core/config.h`).
- **Follow the fingers.** A gesture that drives a zoom transition moves
  the camera along the zoom path continuously; nothing jumps.
- **Commit or spring back.** Release past a threshold of progress — or
  a flick onward — commits the transition; released earlier, the camera
  animates back. The abandon swipe commits past a sideways travel
  distance; short of it the frame springs back. All thresholds are
  compile-time constants in `src/core/config.h`.
- **One action per gesture.** A hop triggers once past its travel
  threshold; a second jump takes a new gesture.
- **Previews while pinching.** A 3-finger pinch in tile or combine
  range shows what release will do; backing out of range clears it
  ([../canvas/overview.md](../canvas/overview.md)).
- **Hold stops momentum.** A hold-gesture begin stops camera coasting.
- **Super decides a 2-finger pinch at its start.** Held as the pinch
  begins, the pinch zooms the canvas in either mode and never reaches a
  client; letting Super go mid-pinch changes nothing, and the pinch never
  counts as a Super tap. Released below the overview's zoom ceiling
  (compile-time, `src/core/config.h`), the overview stays at that zoom;
  above it the view settles 1:1 — where it started when the pinch began in
  the normal view, under the cursor when it began in the overview.
- **Forwarding.** Only an interactive output
  ([../canvas/infinite-canvas.md](../canvas/infinite-canvas.md)) — zoom
  exactly 1, no transition in progress — forwards gestures to clients
  (2-finger gestures and unbound finger counts, via the
  pointer-gestures protocol, [../platform/protocols.md](../platform/protocols.md)).
  In the overview the compositor owns every gesture.

## Design considerations

- **3-finger pan, 4-finger up** is the primary interface: on one
  unbounded plane a pan is always meaningful, so it works in both
  modes, and the zoomed-out canvas is where desktops are arranged.
- **Sloppy pinches are swipes.** At 3 and 4 fingers, touchpad gesture
  classification is unreliable; the finger count decides the intent,
  so a misclassified pinch still pans.
- **3- and 4-finger gestures never reach clients.** They are the
  compositor's navigation vocabulary; a client that grabbed them could
  swallow canvas movement. 2-finger gestures stay the client's.

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
