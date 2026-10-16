# Rendering

> Owns the renderer, the repaint policy and what clients observe of the
> canvas. Output scale is [hidpi.md](hidpi.md); the camera itself is
> [../canvas/infinite-canvas.md](../canvas/infinite-canvas.md).

## Requirement

Every output paints its own view of the one canvas. An output renders
a frame exactly when something it shows changed, when it is
animating, or when wlroots asks for one — and never otherwise. A
client never sees the zoom: it renders in logical pixels at its
output's scale, and is told a frame only when its surface is shown.

## Design considerations

- **An own renderer, because wlr_scene cannot zoom a tree.** The canvas
  draws every window at a per-output zoom that changes under the
  fingers; wlr_scene places each surface once. The wlroots render API
  is used directly, and the wlr_scene sources stay the reference for
  surface and output handling
  ([../platform/wlroots.md](../platform/wlroots.md)).
- **Full repaint, not damage regions.** An output repaints in full when
  dirty; damage-region tracking is not part of the policy.
- **Idle costs nothing.** No change on an output, no render, no frame,
  no client work.

## Behavior

| Subject | Rule |
|---|---|
| Repaint | an output renders when its content is damaged, or while a camera animation or transient hint is running; an idle output renders nothing |
| wlroots-requested frames | wlroots sets `needs_frame` on the output when something outside the compositor's own damage needs a frame — screencopy clients such as grim, software cursors; the output renders then, and a frame is scheduled on that signal ([../bugs/screencopy-never-returned.md](../bugs/screencopy-never-returned.md)) |
| Frame callbacks | sent only to surfaces shown on some output |
| Presentation | presentation-time feedback accompanies every rendered frame ([protocols.md](../platform/protocols.md)) |
| Enter/leave | like wlr_scene: a window panned off every output keeps its last outputs, so the client does not re-render or re-layout just because the camera moved |
| Draw order | canvas clear; wallpaper and bottom layer, in screen space; a dim over a wallpaper while zoomed; the grid; windows bottom-to-top with border, title bar and surfaces including popups; desktop frames; the top layer, skipped when a fullscreen window covers the output; the overview UI — tabs, target highlight, previews, shelf, help line; the transient hint; the overlay layer; the drag icon; software cursors |
| Grid | a faint grid on the canvas, spacing kept at 48 screen px or more; drawn in the overview always, and in normal mode only when no wallpaper is mapped |
| Desktop frames | dashed outlines: always in the overview; in normal mode they fade in while panning ([../canvas/desktops.md](../canvas/desktops.md)) |
| Offscreen capture | `capture_output`/`capture_view` (render/capture.h) re-render offscreen what an output area or one window shows: the same drawing an on-screen frame does — at the output's scale, with no cursors, no surface enter/frame feedback (the output's own frames still own it), and none of the capture UI: the picker, toolbar and thumbnail never appear in a shot or a recording. Overview chrome, hints, notifications and the launcher do, as on screen. A window is rendered alone: title bar, border, content, nothing that overlaps it, whatever the canvas zoom ([../platform/protocols.md](../platform/protocols.md) owns the capture protocols fed from it) |
| Zoom scaling | boxes scale with the zoom, and edges are rounded from canvas coordinates individually, so neighbours leave no seams |

## Error states, corner cases

- A frame request against a still screen (grim on an idle desktop) is
  the `needs_frame` path, not damage.
- A window on no output keeps its buffers and its last outputs; it is
  not unmapped, hidden or re-laid-out.
- An offscreen capture is a re-render, not a readback of the last committed
  frame: what it shows is what the output would show now.

## Cross-links

- [hidpi.md](hidpi.md), [../canvas/infinite-canvas.md](../canvas/infinite-canvas.md),
  [../platform/outputs.md](../platform/outputs.md),
  [../platform/protocols.md](../platform/protocols.md).
