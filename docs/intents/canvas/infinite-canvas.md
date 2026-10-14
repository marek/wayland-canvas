# The infinite canvas

> Cornerstone. Owns the plane, canvas coordinates and the per-output
> camera. The gestures that move the camera are in
> [../input/gestures.md](../input/gestures.md), the zoomed-out view in
> [overview.md](overview.md), what a desktop is in
> [desktops.md](desktops.md), the outputs themselves in
> [../platform/outputs.md](../platform/outputs.md).

## Requirement

One unbounded plane carries every window, and each output looks at it
through its own camera. Being on a desktop is never required: panning
to a free area of the canvas is as easy as going to a desktop.

## Design considerations

- **One plane, not a grid of desktops.** A desktop is a marked
  rectangle ([desktops.md](desktops.md)), not a container; that is
  what makes free areas, combining and abandoning cheap.
- **Clients are authored for 1:1.** A client surface is laid out for
  its output's scale; input delivered through a zoomed camera would be
  wrong in both position and size. So zoom exactly 1 is the only mode
  clients get input in, and any other zoom is the compositor's own
  view ([overview.md](overview.md)).
- **A resting camera sits on the pixel grid.** At 1:1 the camera
  position is quantised to the output's pixel grid so a window never
  renders subpixel-blurry. Rendering and hit testing use the same
  snapped camera.

## Behavior

### Coordinates and camera

| Subject | Rule |
|---|---|
| Canvas units | Logical pixels at zoom 1; the plane is unbounded in every direction |
| Camera | One per output: the canvas point at the output's centre, plus a zoom |
| Normal mode | Zoom exactly 1; clients get input |
| Zoomed mode | Any other zoom; the compositor owns the pointer and keyboard ([overview.md](overview.md)) |
| Interaction gate | An output forwards pointer and gesture events to clients only in normal mode, at exactly zoom 1, with no zoom transition in progress |
| Scale of the output itself | [../render/hidpi.md](../render/hidpi.md) |

### Panning

| Input | Rule |
|---|---|
| 3-finger swipe | Pans anywhere; the action per mode is in the gesture table ([../input/gestures.md](../input/gestures.md)) |
| Pointer drag on empty canvas | Pans |
| 2-finger scroll over empty canvas | Pans; over a client, scrolls the client |
| Super+Alt+arrows | Pans a third of a screen ([../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)) |

- Pan direction is natural: content follows the fingers, like dragging
  a map.
- A released 3-finger pan keeps momentum; the camera coasts and decays
  (decay time constant and minimum coasting speed are compile-time
  constants in `src/config.h`).
- A pan that lands close to a desktop (a compile-time fraction of the
  screen) snaps onto it; any other landing spot stays wherever the
  pan stopped.

### Free areas and orientation

| Subject | Rule |
|---|---|
| Free areas | Any spot that is not a desktop. First-class: the camera may rest there, windows may live there, the overview may target it ([overview.md](overview.md)) |
| Grid | A faint grid over the canvas, so panning over empty space is visible: in normal mode only when no wallpaper is mapped, always in the zoomed-out view |
| Desktop outlines | Dashed frames, faded in faintly while panning in normal mode. How frames and grid are drawn is [../render/rendering.md](../render/rendering.md)'s |

## Error states, corner cases

- **Panned far from every desktop** is a valid state, not an error: the
  grid keeps motion visible and the overview still fits everything on
  the canvas.
- **A wallpaper maps**: the grid hides in normal mode (the wallpaper is
  feedback enough) and stays in the zoomed-out view.
- **Windows lying off every desktop**: they belong to no desktop
  ([desktops.md](desktops.md)) and keep their canvas positions.
- **Camera passing through zoom 1 mid-transition**: still not
  interactive; input returns only when the camera rests at exactly 1.

## Cross-links

- [desktops.md](desktops.md), [overview.md](overview.md),
  [../input/gestures.md](../input/gestures.md),
  [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md),
  [../windows/window-management.md](../windows/window-management.md),
  [../render/rendering.md](../render/rendering.md),
  [../render/hidpi.md](../render/hidpi.md),
  [../platform/outputs.md](../platform/outputs.md).
