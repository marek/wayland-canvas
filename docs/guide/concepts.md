# Concepts

The four ideas the rest of this guide leans on: the canvas, the desktop, the
free area and the overview.

- **Canvas**: every output has its own camera onto one shared plane.
- **Desktop**: a screen-sized rectangle on the canvas. A window belongs to the
  desktop its centre is in. Moving a desktop frame never moves windows. Each
  desktop has its own zoom, 25 % to 200 %: it scales the picture only, so
  windows keep their size and layout (and look soft above 100 %); Super+0
  brings it back to 100 %.
- **Free area**: any spot that isn't a desktop; the overview takes you there,
  and holding Super you can look there from a desktop (double Super brings
  you back). Panning never changes the zoom: a free area keeps the zoom you
  came with (Super+0 returns it to 100 %).
- **Overview**: a zoomed-out, live view of the canvas where desktops are
  arranged. It has a *target* (accent outline): a desktop, or a free
  screen-sized spot.

## See also

[Gestures, mouse and keyboard](controls.md) for how you move between these.
