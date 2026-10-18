# The infinite canvas

> Cornerstone. Owns the plane, canvas coordinates and the per-output
> camera. The gestures that move the camera are in
> [../input/gestures.md](../input/gestures.md), the zoomed-out view in
> [overview.md](overview.md), what a desktop is in
> [desktops.md](desktops.md), the outputs themselves in
> [../platform/outputs.md](../platform/outputs.md).

## Requirement

One unbounded plane carries every window, and each output looks at it
through its own camera. Being on a desktop is never required: reaching
a free area of the canvas is as easy as going to a desktop.

## Design considerations

- **One plane, not a grid of desktops.** A desktop is a marked
  rectangle ([desktops.md](desktops.md)), not a container; that is
  what makes free areas, combining and abandoning cheap.
- **Each desktop has its own scale, a zoom that stays with it.** A
  desktop shown at 80 % fits more of the canvas on the screen and one at
  125 % reads larger. The compositor scales the picture and the windows
  keep their size and layout, so zooming never rearranges anything; input
  reaches them through the same camera, so a scaled desktop is as usable
  as one at 100 %.
- **Only a zoom action changes the zoom.** A Super pinch, Super+0, going
  to a desktop (a hop onto one included: it brings its scale) and the
  overview do; a pan, a scroll or a hop into a free area never does, so a
  free view keeps the zoom the camera brought and stays as usable as the
  desktop it was left from. The overview and a transition on its way are
  the compositor's own, not interactive ([overview.md](overview.md)).
- **Panning needs Super.** A drag or a scroll on a desktop's background
  is easily made by accident, and a pan it made would slide the desktop
  off the screen and lose the user's place. Held Super makes the pan
  deliberate, as it makes a pinch scale, and the look away it makes is
  undone by one key. Moving desktops is the overview's
  ([overview.md](overview.md)).
- **A resting camera sits on the pixel grid.** At rest the camera
  position is quantised to the output's pixel grid at its zoom, so a
  window never renders subpixel-blurry. Rendering and hit testing use the
  same snapped camera.

## Behavior

### Coordinates and camera

| Subject | Rule |
|---|---|
| Canvas units | Logical pixels at zoom 1; the plane is unbounded in every direction |
| Camera | One per output: the canvas point at the output's centre, plus a zoom |
| Normal mode | The camera rests at the zoom of what it shows: the scale of the desktop whose frame fills the screen, or on a free view the zoom it was brought there at (Super+0 returns a free view to 100 %); clients get input |
| Zoomed mode | The overview, and any zoom transition while it runs; the compositor owns the pointer and keyboard ([overview.md](overview.md)) |
| Interaction gate | An output forwards pointer and gesture events to clients only in normal mode, resting at that zoom, with no zoom transition in progress |
| Scale of the output itself | [../render/hidpi.md](../render/hidpi.md) |

### Panning and hops

| Input | Rule |
|---|---|
| 3-finger swipe | Hops; the action per mode is in the gesture table ([../input/gestures.md](../input/gestures.md)) |
| Super+Ctrl+arrows | Hops, as a 3-finger swipe does ([../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)) |
| Super with a pointer drag on empty canvas | Looks away |
| Super with a 2-finger scroll, over empty canvas or a window | Looks away; the window never sees the scroll |
| Super+Alt+arrows | Looks away by a third of a screen ([../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)) |
| Double Super | Back to the view the look away left |
| A drag or a 2-finger scroll over empty canvas, without Super | Nothing moves; a press there stays a click |

- A drag pans naturally: the content follows the pointer, like dragging
  a map.
- A pan in the normal view is a look away, not a move. The view stays
  where the pan leaves it, so what it shows can be used, and the output
  keeps the view it rested at before — a desktop at its scale, or a free
  view; double Super eases back there. Any other move — a hop, going to a
  desktop, the overview, a Super pinch — makes the view it brings the one
  kept. With no look away under way, double Super does nothing.
- A hop goes the way the fingers point, to the nearest desktop in that
  direction — within a compile-time cone, straight-ahead neighbours
  beating nearer off-axis ones, as the overview's hop
  ([overview.md](overview.md)) — easing onto it at its scale. With none
  that way the view stays where it is and a hint says so.
- A hop starts from where the camera is going: a swipe made while a hop
  still eases walks on from that hop's landing, so quick swipes pass
  desktop by desktop.
- A hop only ever lands on a desktop: no momentum, no landing in empty
  canvas or between places, so a swipe cannot lose the user's place. A
  free area is reached from the overview (its hop and its click target
  free spots); a look away shows one until double Super or another move.

### Free areas and orientation

| Subject | Rule |
|---|---|
| Free areas | Any spot that is not a desktop. First-class: the camera may rest there, windows may live there, the overview may target it ([overview.md](overview.md)) |
| Grid | A faint grid over the canvas, so panning over empty space is visible: in normal mode only when no wallpaper is mapped, always in the zoomed-out view |
| Desktop outlines | Dashed frames, faded in faintly while the camera pans or hops in normal mode. How frames and grid are drawn is [../render/rendering.md](../render/rendering.md)'s |

## Error states, corner cases

- **Panned far from every desktop** is a valid state, not an error: the
  grid keeps motion visible, double Super brings the view back, and the
  overview still fits everything on the canvas.
- **A look away on several outputs**: each output keeps its own view to
  go back to; double Super brings back the output under the pointer.
- **A wallpaper maps**: the grid hides in normal mode (the wallpaper is
  feedback enough) and stays in the zoomed-out view.
- **Windows lying off every desktop**: they belong to no desktop
  ([desktops.md](desktops.md)) and keep their canvas positions.
- **Camera passing through a resting zoom mid-transition**: still not
  interactive; input returns only when the camera rests.

## Cross-links

- [desktops.md](desktops.md), [overview.md](overview.md),
  [../input/gestures.md](../input/gestures.md),
  [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md),
  [../windows/window-management.md](../windows/window-management.md),
  [../render/rendering.md](../render/rendering.md),
  [../render/hidpi.md](../render/hidpi.md),
  [../platform/outputs.md](../platform/outputs.md).
