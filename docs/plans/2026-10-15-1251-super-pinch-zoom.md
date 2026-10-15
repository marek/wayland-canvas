# Super pinch zoom

Date: 2026-10-15 12:51 EDT

## Context

Holding Super while pinching with two fingers on the touchpad zooms the canvas around the cursor, following the
fingers, in the normal view and in the overview alike: pinching in from the normal view passes into the overview,
spreading in the overview zooms back in up to 1:1 and lands in the normal view under the cursor. Where the zoom is on
release decides the mode. Without Super, a 2-finger pinch keeps reaching the application under the pointer. The rules
are in [input/gestures.md](../intents/input/gestures.md) and [canvas/overview.md](../intents/canvas/overview.md).

## Tasks

- [ ] Super + 2-finger pinch in the normal view zooms out around the cursor, following the fingers, into the overview; spreading stops at 1:1
- [ ] Super + 2-finger spread in the overview zooms in around the cursor up to 1:1
- [ ] Released below the overview's zoom ceiling the overview stays at that zoom; above it the view settles 1:1 where it started (from the normal view) or under the cursor (from the overview)
- [ ] The zoom ceiling is a named constant in `src/core/config.h`, used by every overview zoom path
- [ ] Without Super, a 2-finger pinch in the normal view still reaches the client; a pinch never counts as a Super tap
- [ ] The gestures scenario covers both directions, both release outcomes and the forwarding without Super; warning-free build, every suite green
