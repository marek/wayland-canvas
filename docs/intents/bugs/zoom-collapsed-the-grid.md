# Case study: the zoom that collapsed the grid

> A bug case study, where history is told on purpose. The present-tense
> rules it produced are in [../canvas/desktops.md](../canvas/desktops.md)
> and [../render/hidpi.md](../render/hidpi.md).

## Symptom

On a tiled desktop, a few Super-pinch zooms in and out turned a window
into a full-width title-bar strip at the screen's edge, and further zooms
took it off the screen. Floating windows changed size on every settle.

## Why the scenarios had not shown it

The gestures scenario settled one zoom at a time and checked that the
grid filled the resized frame, which it did each time. Nothing zoomed out
and then back in, so no window's centre was ever left outside a frame that
shrank under it.

## Root cause

Four things together:

1. A settled scale re-flowed the tiled grid into the frame at that scale:
   zooming out to 50 % laid the grid out over a frame of four screens.
2. Grid membership was the window's centre inside the frame, and the frame
   shrinks as the scale rises. Settling at 100 % after 50 % left the outer
   windows' centres past the frame's edge (the right and bottom edges
   exclude the point), so the re-flow dropped them silently: no new size,
   still told they were tiled, left where the 50 % grid had put them,
   mostly off the screen.
3. The frame was resized around its own rounded centre on every change,
   each live pinch update included, so the centre walked a few pixels per
   settle and kept the dropped windows outside at later scales.
4. Clients were told every settled scale, and foot re-fits a floating
   window to keep its grid of cells, so floating windows changed size on
   every settle.

What looked like a window resizing into a bar was the camera showing the
edge of a window that was no longer laid out.

## How it was found

A throwaway scenario on the gestures harness pinched in and out with
Super held over floating, maximized and tiled windows (foot and ghostty,
output scales 1 and 2), logging after every settle the size the
compositor sent, the geometry the client committed, the scale it was told
and its buffer, next to the client's `WAYLAND_DEBUG` log. Only a tiled
desktop with two or more windows collapsed, and a dropped window's last
configure ever was the one from the 50 % settle.

## Rules it produced

- A desktop's scale scales the picture: it never re-flows a grid, never
  resizes a window and is never told to a client
  ([../canvas/desktops.md](../canvas/desktops.md),
  [../render/hidpi.md](../render/hidpi.md)).
- A tiled grid holds the windows it tiled until one leaves it; a frame's
  edge passing a window's centre never drops it.
- A frame resizes around an exact centre, so zooms never move a desktop.
- For a report of windows that drift, shrink or vanish after a repeated
  action: log per cycle what the compositor sent, what the client
  committed and what it was told, and look for state that changed without
  a message (here: grid membership against a moving edge).
