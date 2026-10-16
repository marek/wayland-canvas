# The overview

> Owns entering and leaving the zoomed-out view, the target model, hop
> semantics, and the overview's UI. Gesture recognition is
> [../input/gestures.md](../input/gestures.md)'s; what a desktop is is
> [desktops.md](desktops.md); the plane and modes are
> [infinite-canvas.md](infinite-canvas.md).

## Requirement

A zoomed-out, live view of the whole canvas in which desktops are
arranged, targeted and operated on — and from which any spot, desktop
or free area, is one gesture away.

## Design considerations

- **Follow the fingers, commit or spring back.** Zoom transitions are
  progressive, so a misfire costs nothing: release early and the view
  returns to where it was.
- **A target, not a click.** Going somewhere is two steps — point at
  it, then dive — so a stray fling cannot drop you somewhere by
  accident.
- **Hops go by direction, not by list.** Desktops sit on a plane, so
  "the one over there" is how they are addressed; a tolerance cone
  forgives sloppy swipes and a straight-ahead preference disambiguates.
- **Free spots are equal citizens**: the target may be a free
  screen-sized spot, so a desktop and an empty area are equally easy
  to go to ([infinite-canvas.md](infinite-canvas.md)).
- **The current screen stays centred when it still fits**, so entering
  the overview does not disorient.

## Behavior

### Entering and leaving

| Subject | Rule |
|---|---|
| Enter | 4-finger swipe up, following the fingers; release early to cancel ([../input/gestures.md](../input/gestures.md)). Also Super+Tab / Super+W |
| Fit | The zoom fits everything on the canvas — desktops and windows — with a margin (compile-time, `src/core/config.h`); the current screen is kept centred when that still fits |
| Zoom further | 4-finger swipe up again zooms out further; 2-finger pinch zooms around the cursor; scroll pans; Ctrl+scroll zooms; `+`/`−` zoom about the screen centre. Clamped to compile-time zoom bounds |
| Leave into the target | 4-finger swipe down zooms into the target, following the fingers, ending at the target desktop's scale (1 for a free spot); release early to cancel |
| Leave on choice | Return, or a click, zooms into the target and exits; Esc goes back to the view before entering |

### The target

| Subject | Rule |
|---|---|
| What it may be | A desktop, or a free screen-sized spot |
| On entering | Set to where you were: the desktop you were on, else a free spot centred on the view |
| How it moves | A 3-finger swipe (a hop), arrow keys, or clicking ([pointer table below](#pointer-and-keys)) |
| Marking | An accent outline ([../render/rendering.md](../render/rendering.md)) |

### Hop semantics

- A 3-finger swipe jumps to the closest desktop in the swipe's general
  direction: the target moves toward the fingers' direction, within a
  tolerance cone (compile-time, `src/core/config.h`); straight-ahead
  neighbours beat nearer off-axis ones.
- No desktop lies that way: the target moves one screen that way — a
  free spot.
- One jump per swipe: past a travel threshold (compile-time) the jump
  happens once; keeping the fingers down does nothing more.

### Pointer and keys

| Input | Action |
|---|---|
| Click a desktop (tab or interior) | Target it and go there |
| Click a window | Focus it; target its desktop, or a spot centred on it; go there |
| Click empty canvas | Target that free spot and go there |
| Click a shelf item | Restore that window and go to it |
| Drag a frame by its tab or empty interior | Reposition the desktop — never its windows, unless it is tiled ([desktops.md](desktops.md)); dropping it onto another frame combines them (past a compile-time overlap fraction); the drop target is highlighted while it would combine |
| Drag a window | Move it on the canvas |
| Drag empty canvas | Pan the overview camera |
| Two-finger tap (right click) on empty canvas or a desktop interior | New desktop at that spot ([desktops.md](desktops.md)) |
| Tab buttons | The Tile switch, Abandon ([desktops.md](desktops.md)) |
| Keys | Esc back; Return go; arrows hop; Delete/BackSpace abandon the target desktop; `t` switch tiling on the target; `m` combine; `n` new desktop; `0` returns the target desktop to 100 % ([desktops.md](desktops.md)); `+`/`−` zoom. Super+Tab / Super+W toggle |
| Shell surfaces | The bar, the toolbar, panels and the note editor take the pointer over them as in the normal view; one that holds the keyboard ([../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)) takes the keys, and the overview's own keys act only while none does |

A press becomes a drag only past a small travel; under it, the release
is a click ([../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)).

### Overview UI

| Element | Rule |
|---|---|
| Tabs | Each desktop has a tab: its name, its scale when not 100 %, the Tile switch (lit while tiling is on) and Abandon |
| Shelf | Minimized windows along the bottom; clicking one restores it and goes there ([../windows/window-management.md](../windows/window-management.md)) |
| Help line | Along the bottom, naming the gestures |
| Hints | Transient messages for completed actions (e.g. "Tiling on for Desktop 2", "Desktop 2 at 80 %") |
| Previews | While a pinch is in tiling or combine range, what release will do (e.g. "Release to combine Desktop 2 into Desktop 3"); backing out of range clears it |

## Error states, corner cases

- **An empty canvas**: the overview still opens; the target is the free
  spot where you were.
- **Abandon, tile or combine aimed at nothing**: act on the desktop
  under the pointer, else the target desktop; a free-spot target means
  tile works on the spot's windows and abandon and combine are ignored
  ([desktops.md](desktops.md)).
- **Hopping with no desktop in the cone**: the target still moves, one
  screen that way, as a free spot.
- **Zoom bounds reached**: the zoom clamps (compile-time bounds in
  `src/core/config.h`); the gesture keeps tracking but the view stops.

## Cross-links

- [infinite-canvas.md](infinite-canvas.md), [desktops.md](desktops.md),
  [../input/gestures.md](../input/gestures.md),
  [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md),
  [../windows/window-management.md](../windows/window-management.md),
  [../render/rendering.md](../render/rendering.md).
