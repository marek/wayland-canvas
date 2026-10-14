# Outputs

> Owns outputs, the layout, hotplug and the usable area. What a camera
> is belongs to [../canvas/infinite-canvas.md](../canvas/infinite-canvas.md);
> what scale an output takes, to [../render/hidpi.md](../render/hidpi.md).

## Requirement

An output is one display showing the shared canvas through its own
camera. Outputs come and go at the plug; windows and desktops never
move because of it — they live on the canvas, not on any output.

## Behavior

| Subject | Rule |
|---|---|
| Camera | each output owns one camera (a centre on the canvas plus a zoom) and is in one mode at a time: normal or overview ([../canvas/infinite-canvas.md](../canvas/infinite-canvas.md), [../canvas/overview.md](../canvas/overview.md)) |
| Layout | outputs are arranged in a global layout; the pointer position is measured in layout coordinates, and each output knows its own layout position |
| Membership | nothing about a window is per-output: a window lives at a canvas position and is shown by whichever outputs' viewports cover it |
| Desktop 1 | the first output creates Desktop 1 at its initial view ([../canvas/desktops.md](../canvas/desktops.md)) |
| Usable area | a screen-sized box inset by that output's layer surfaces' exclusive zones; maximized windows, tiled windows and new windows keep inside it ([../windows/window-management.md](../windows/window-management.md)) |

## Error states, corner cases

- Hotplug adds and removes outputs at runtime; the layout changes, and
  windows and desktops stay exactly where they are on the canvas.
- A layer surface's output going away leaves the surface without an
  output; code that walks layer surfaces tolerates that
  ([protocols.md](protocols.md)).

## Cross-links

- [wlroots.md](wlroots.md), [protocols.md](protocols.md),
  [../canvas/infinite-canvas.md](../canvas/infinite-canvas.md),
  [../render/hidpi.md](../render/hidpi.md),
  [../render/rendering.md](../render/rendering.md).
