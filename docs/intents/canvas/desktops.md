# Virtual desktops

> Owns what a desktop is, membership, ids, a desktop's scale and tiling,
> and the desktop operations: create, abandon, combine. The plane desktops
> sit on is [infinite-canvas.md](infinite-canvas.md); the gestures and UI
> that invoke the operations are in [overview.md](overview.md) and
> [../input/gestures.md](../input/gestures.md).

## Requirement

A desktop is a rectangle the user positions over the windows however they
want; moving a desktop frame never moves a window. Each desktop has its
own scale, set with Super and a two-finger pinch, and windows on it are
shown and used at that scale. Each desktop can be switched to tiling: while
it is on, the desktop keeps its windows tiled, and every window opened on
it joins the tiling.

## Design considerations

- **Frames, not containers.** A desktop is a mark laid over the plane,
  so it can be dragged anywhere without disturbing what it covers, and
  abandoning it never disturbs the windows inside it. The canvas is the
  storage; a desktop is a convenience on top of it
  ([infinite-canvas.md](infinite-canvas.md)).
- **Membership by frame centre** is a single rule that needs no
  bookkeeping when frames or windows move: a window belongs to the
  desktop its frame centre lies in, or to none.
- **Combine keeps relative layout**, translating by the offset between
  the two desktops, so a working arrangement survives the merge.
- **Ids are the smallest unused number**, matching Windows' virtual
  desktop numbering that `Super+1..9` addresses
  ([../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)).
- **Scale and tiling belong to the desktop**, not to the screen or a
  window, so going to a desktop brings its scale and its layout with it.
  A free view has neither: asking a free view for a scale or for tiling
  pins it as a desktop first.

## Behavior

| Subject | Rule |
|---|---|
| Definition | A rectangle on the canvas that fills the screen at the desktop's scale: the output's logical size, when the desktop is made, divided by the scale |
| Appearance | A dashed outline |
| Membership | A window belongs to the desktop its frame centre lies in |
| Overlapping frames | The topmost frame containing the centre owns the window |
| First desktop | Desktop 1 exists from the start, at the first output's initial view |
| Ids | Smallest unused number |

### Create

| Way | Result |
|---|---|
| Two-finger tap (right click) on empty overview canvas or a desktop interior | A new desktop at that spot |
| `n` in the overview | A new desktop at the target centre if that is a free spot, else at the cursor ([overview.md](overview.md)) |
| Super+Ctrl+D | Pins the current view as a new desktop |

### Abandon

Remove the desktop. Its windows stay on the canvas where they are;
each keeps belonging to whatever desktop's frame covers its centre, or
to none.

### Combine

The other desktop's windows move into this one, keeping their relative
layout (translated by the offset between the desktops); the other
desktop is removed. The combine is into the desktop under the pointer
or the target ([overview.md](overview.md)); the source is the nearest
other desktop.

### Scale

| Subject | Rule |
|---|---|
| Range | 50 % to 200 %; a new desktop starts at 100 % |
| Change | Super + 2-finger pinch on the desktop shown ([../input/gestures.md](../input/gestures.md)): pinching in scales down, spreading scales up, following the fingers |
| Frame | Resizes around its centre as the scale changes, so the desktop keeps filling the screen; its windows stay where they are on the canvas |
| Settling | Released, the scale settles on the nearest 5 % step, catching at 100 % |
| Windows | Shown at the scale and fully usable; told the scale on release, so they render for it ([../render/hidpi.md](../render/hidpi.md)) |
| Shown | The overview's tab names a scale other than 100 % |

### Tiling

A switch per desktop, off on a new desktop: the Tile switch on the
desktop's tab, `t` in the overview, Super+T in the normal view, or a
3-finger spread ([overview.md](overview.md),
[../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)).

| Subject | Rule |
|---|---|
| On | The desktop's windows are grid-tiled inside the area not covered by bars, with gaps (a compile-time constant in `src/core/config.h`), in reading order of where they stood |
| Kept | While on, the grid re-flows when a window opens on the desktop, closes, is minimized or restored, or is moved in or out of it, and when the scale or the frame changes |
| New windows | A window opened while the desktop is shown joins the tiling |
| Moving a tiled window | Dropped inside the desktop, it takes the grid slot nearest the drop; dropped outside, it leaves the tiling |
| Maximize, fullscreen | Take the window out of the grid while in that state; leaving the state puts it back |
| Frame | A tiled desktop keeps its windows tiled inside its frame wherever the frame goes: the one case where a frame carries windows |
| Off | Windows stay where the grid left them and are told they are no longer tiled |

The per-window tiling contract is
[../windows/window-management.md](../windows/window-management.md)'s.

## Error states, corner cases

- **A window straddling a frame edge**: the centre decides, nothing
  else.
- **Abandoning the desktop a window sits on**: the window does not
  move; it simply belongs to no desktop until a frame covers its
  centre again.
- **Combining with no other desktop**: nothing to combine; the gesture
  is ignored.
- **Switching tiling on for an empty desktop**: the switch is on; the
  first window opened there fills the area.
- **Combining into a tiled desktop**: the incoming windows join its grid;
  combining a tiled desktop into an untiled one leaves the windows where
  the translation puts them, no longer tiled.
- **Abandoning a tiled desktop**: its windows stay where they are, no
  longer tiled.
- **Windows that refuse the scale** (clients without fractional scaling):
  they are scaled by the compositor and may look soft
  ([../render/hidpi.md](../render/hidpi.md)).
- **Windows overlapping after a combine or a frame move**: allowed;
  stacking and focus rules apply
  ([../windows/window-management.md](../windows/window-management.md)).

## Cross-links

- [infinite-canvas.md](infinite-canvas.md), [overview.md](overview.md),
  [../input/gestures.md](../input/gestures.md),
  [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md),
  [../windows/window-management.md](../windows/window-management.md),
  [../platform/outputs.md](../platform/outputs.md).
