# Title bars

> Owns the server-side decorations: the Windows 11 look, metrics,
> button states and clicks, resize zones, and the decoration
> negotiation. What the buttons do to the window as a whole is in
> [window-management.md](window-management.md).

## Requirement

Server-side title bars in the Windows 11 dark look — minimize,
maximize/restore, close — drawn by the compositor for every client
that negotiates server-side decorations, and only for those clients.
No client ever shows two title bars.

## Behavior

Metrics, logical pixels (Windows 11; constants in `src/config.h`):

| Part | Rule |
|---|---|
| Title bar | 32 px tall |
| Caption buttons | 46 px wide, right-aligned: minimize, maximize/restore, close |
| Title text | left-aligned, ellipsized when it does not fit |
| Border | 1 px on every side — accent colour while the window is focused, neutral otherwise |

Text position, fonts and all decoration colours are compile-time
constants in `src/config.h`.

Window state:

| State | Decorations |
|---|---|
| Normal | title bar and border |
| Maximized | title bar kept, border dropped |
| Fullscreen | none at all |

Button states:

| State | Minimize, maximize/restore | Close |
|---|---|---|
| Normal | the bar's own colour | the bar's own colour |
| Hover | a light overlay | red, with a white glyph |
| Pressed | a dimmer overlay | a darker red |

An unfocused window is drawn with the dimmer title bar and text
colours.

Clicks:

| Action | Effect |
|---|---|
| Press a caption button | shows the pressed state; a button acts on release over the same button — press, drag off, release does nothing |
| Minimize | hides the window ([window-management.md](window-management.md)) |
| Maximize/restore | toggles maximize |
| Close | closes the window |
| Double-click the title | toggles maximize |
| Drag the title | moves the window; a maximized window restores under the cursor |

Resize zones — every edge and corner of the frame resizes when grabbed
through:

| Zone | Rule |
|---|---|
| An invisible band just outside the frame | its width is a compile-time constant (`RESIZE_MARGIN`) |
| The border itself | all sides |
| The top of the title bar | acts as the top edge |

A corner resizes both axes, an edge one; the edge opposite the grab
stays fixed — anchoring is owned by
[window-management.md](window-management.md).

Who gets a title bar:

| Client | Decorations drawn by |
|---|---|
| Negotiates xdg-decoration | the compositor — server-side is the answer to any mode the client requests |
| Negotiates KDE server-decoration | the compositor — the manager's default mode is server |
| Negotiates neither | the client — it keeps its own decorations |

## Design considerations

- **No doubled title bar.** A client that has not negotiated cannot be
  stopped from drawing its own decorations; drawing a compositor bar
  as well would stack two frames on one window. Negotiation is
  therefore the only switch for the server-side look.
- **Windows 11 is the reference look** for the bar, the caption
  metrics and the close-button red: the product asked for standard
  Windows-style bars, not a novel design.

## Error states, corner cases

- The top of the title bar is a resize zone, so a grab there resizes
  instead of dragging the window, even though it lies inside the bar.
- The title bar texture is drawn at the output's scale so it stays
  crisp at any output scale
  ([../render/hidpi.md](../render/hidpi.md)).

## Cross-links

- [window-management.md](window-management.md) — maximize, fullscreen,
  minimize and the rest of the window lifecycle.
- [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)
  — Super+drag move/resize, pointer focus, the bindings table.
- [../platform/protocols.md](../platform/protocols.md) —
  xdg-decoration and KDE server-decoration.
- [../render/hidpi.md](../render/hidpi.md),
  [../render/rendering.md](../render/rendering.md) — the title bar
  texture and its redraw rules.
