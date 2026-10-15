# Virtual desktops

> Owns what a desktop is, membership, ids, and the desktop operations:
> create, abandon, combine, tile. The plane desktops sit on is
> [infinite-canvas.md](infinite-canvas.md); the gestures and UI that
> invoke the operations are in [overview.md](overview.md) and
> [../input/gestures.md](../input/gestures.md).

## Requirement

A desktop is a screen-sized rectangle the user positions over the
windows however they want. Moving a desktop frame never moves a
window.

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

## Behavior

| Subject | Rule |
|---|---|
| Definition | A screen-sized rectangle on the canvas: the output's logical size when the desktop is made |
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

### Tile

Grid-tile the windows whose centre is in the desktop — or in the
target spot when the target is a free spot — inside the area not
covered by bars, with gaps (a compile-time constant in `src/core/config.h`).
The windows are told they are tiled; the per-window tiling contract is
[../windows/window-management.md](../windows/window-management.md)'s.

## Error states, corner cases

- **A window straddling a frame edge**: the centre decides, nothing
  else.
- **Abandoning the desktop a window sits on**: the window does not
  move; it simply belongs to no desktop until a frame covers its
  centre again.
- **Combining with no other desktop**: nothing to combine; the gesture
  is ignored.
- **Tiling an empty desktop**: nothing to tile; a no-op.
- **Windows overlapping after a combine or a frame move**: allowed;
  stacking and focus rules apply
  ([../windows/window-management.md](../windows/window-management.md)).

## Cross-links

- [infinite-canvas.md](infinite-canvas.md), [overview.md](overview.md),
  [../input/gestures.md](../input/gestures.md),
  [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md),
  [../windows/window-management.md](../windows/window-management.md),
  [../platform/outputs.md](../platform/outputs.md).
