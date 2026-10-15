# HiDPI

> Owns output scale and crispness. What a camera is belongs to
> [../canvas/infinite-canvas.md](../canvas/infinite-canvas.md); how to
> set the scale at launch, to
> [../workspace/build-and-run.md](../workspace/build-and-run.md).

## Requirement

Dense panels get scale 2 and a native feel: decorations and launcher
text drawn at the output scale, and the canvas at 1:1 sitting on the
pixel grid so windows are never blurry.

## Behavior

| Subject | Rule |
|---|---|
| Scale choice | `-S <scale>`, else `$WAYLAND_CANVAS_SCALE`, else 2 on a dense panel — at least 192 dpi and at least 1200 px tall, GNOME's rule — else 1. A 2880×1800 panel takes 2 |
| Nested | a nested run always renders at 1x, soft on a 2x host ([../workspace/build-and-run.md](../workspace/build-and-run.md)) |
| Clients | are told the scale they are shown at: the output's scale times the scale of the desktop they belong to (1 off every desktop), through fractional-scale and, rounded up, the preferred buffer scale; a window changes it when it moves to a desktop of another scale or the desktop's scale settles. The overview's zoom is invisible to them ([rendering.md](rendering.md), [../canvas/desktops.md](../canvas/desktops.md)) |
| Decorations | title-bar and launcher textures are drawn with cairo and pango at the scale they are shown at (the output's times the desktop's) and cached; a texture is redrawn only when its width, scale, active or maximized state, hover or title changes ([../windows/title-bars.md](../windows/title-bars.md)) |
| Crispness at rest | the camera snaps to the output's pixel grid at its zoom, so a window rendered for its desktop's scale maps its buffer pixels one to one ([../canvas/infinite-canvas.md](../canvas/infinite-canvas.md)) |

## Cross-links

- [rendering.md](rendering.md), [../platform/outputs.md](../platform/outputs.md),
  [../workspace/build-and-run.md](../workspace/build-and-run.md),
  [../windows/title-bars.md](../windows/title-bars.md).
