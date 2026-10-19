# Touch and the pen

> Owns the touchscreen and the pen: what reaches apps, what drives the
> compositor's own UI and the canvas, and which screen they act on. The
> touchpad's gestures are [gestures.md](gestures.md); the mouse is
> [keyboard-and-pointer.md](keyboard-and-pointer.md).

## Requirement

A touchscreen and a pen work in every window and on the compositor's own
UI. Apps get touch as touch and the pen as a tablet tool with its pressure
and tilt; the compositor's UI takes a finger or the pen as the mouse's left
button, and a finger held still as a right click. Two fingers move the
view, and three or four fingers swipe as they do on the touchpad.

## Design considerations

- **Apps own what lands on them.** A contact on a client surface goes to
  that client in its own protocol — touch as touch, the pen through
  tablet-v2 — so apps keep their own scrolling, inking, palm handling and
  long press. An app with no touch support still gets a finger, and one
  with no tablet support the pen, as the pointer.
- **The compositor's UI has one pointer path.** A finger or the pen on a
  title bar, a button or the overview drives the same handling a mouse
  button does, so every press, drag and click there works by touch and by
  pen without a rule of its own, and the pointer moves to where they act.
- **Two fingers are the deliberate move.** One finger on empty canvas moves
  nothing, as a mouse drag without Super moves nothing, so a tap that slips
  never pans. Two fingers stand for Super held: a drag looks away, a pinch
  does what Super and a 2-finger pinch do on the touchpad.
- **One gesture vocabulary.** Three and four fingers on the screen run the
  touchpad's gestures through the same entry points, so the preset that
  reassigns the touchpad's swipes reassigns the screen's too.

## Behavior

### Devices

| Subject | Rule |
|---|---|
| The screen they act on | the output a touchscreen or a pen is built into, a laptop's built-in panel; where that cannot be told, the first output |
| What the seat offers | touch while a touchscreen is connected; tablet-v2 to every client ([../platform/protocols.md](../platform/protocols.md)) |

### Touch

| Where a finger lands | What happens |
|---|---|
| A window, a popup, a shell surface (the bar, a panel), a note, the locker | Its client gets the touch; each finger stays with the surface it landed on until it lifts, past its edges too. A client with no touch support gets its first finger as the pointer, as the compositor's UI does below: a tap clicks, a drag drags, held still a right click |
| The compositor's UI: a title bar and its buttons, a resize edge, the overview's tabs, frames and shelf, the launcher, the capture UI | One finger is the left button: a tap clicks, a drag drags — a title bar moves its window, an edge resizes it |
| The compositor's UI, the finger held still | A right click, once the finger has stayed within the press-vs-drag travel for a compile-time time (`src/core/config.h`): what a right click does there, such as a new desktop on the overview's empty canvas |
| Empty canvas, one finger | A tap is a click (in the overview it targets a free spot); a drag moves nothing |
| Empty canvas, two fingers | A drag looks away, and in the overview pans, the content following the fingers; a pinch does what Super and a 2-finger pinch do on the touchpad, by its pinch-scale switch ([gestures.md](gestures.md)). Which comes first decides it: their spread changing by a compile-time fraction, or their middle travelling the press-vs-drag travel (`src/core/config.h`); fingers moving together count once all of them have moved |
| Anywhere, three or four fingers | The touchpad's swipe for that count, by the preset ([gestures.md](gestures.md)); the fingers that had reached a client are cancelled for it. Under `off` every finger reaches the client under it |

### The pen

| Where the pen is | What happens |
|---|---|
| Over a window, a popup or a shell surface | Its client gets the pen as a tablet tool — in and out of range, motion, pressure, tilt, the tip and the buttons — or, with no tablet support, as the pointer: the tip is the left button, the barrel button the right |
| Over the compositor's UI or empty canvas | The mouse: the pointer follows the pen, the tip is the left button and the barrel button the right; on empty canvas a drag moves nothing |

### While locked

Touch and the pen reach the locker's surface and nothing else: no gesture,
no title bar, no canvas ([../shell/lock.md](../shell/lock.md)). A contact
made before the session locked ends as it locks.

## Error states, corner cases

- **A touchscreen or tablet unplugged mid-contact, an output going away,
  or the session locking**: every contact ends. A client's touches are
  cancelled, a drag on the compositor's UI ends as a release would, and
  the pen leaves the client it is on, the tip lifting first. Fingers still
  down reach nothing until they lift.
- **The pen leaving range with the tip down**: the tip lifts first, so no
  press is left held.
- **An X11 window**: a touch or a pen tip for it reaches X only once X has
  taken in the window moves sent before it ([../windows/x11.md](../windows/x11.md)).

## Cross-links

- [gestures.md](gestures.md), [keyboard-and-pointer.md](keyboard-and-pointer.md),
  [../windows/title-bars.md](../windows/title-bars.md),
  [../canvas/overview.md](../canvas/overview.md),
  [../canvas/infinite-canvas.md](../canvas/infinite-canvas.md),
  [../windows/x11.md](../windows/x11.md),
  [../shell/lock.md](../shell/lock.md),
  [../platform/protocols.md](../platform/protocols.md).
