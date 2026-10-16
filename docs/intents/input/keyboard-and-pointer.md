# Keyboard and pointer

> Owns the key bindings, the Super tap, and the pointer model: focus,
> drags, scroll, touchpad settings, nested-mode notes. Title bars and
> their hit zones are [../windows/title-bars.md](../windows/title-bars.md);
> window states and placement
> [../windows/window-management.md](../windows/window-management.md);
> the launcher [../apps/launcher.md](../apps/launcher.md); touchpad
> gestures [gestures.md](gestures.md).

## Requirement

Super (the Windows / logo key) is the compositor's key: every canvas
shortcut includes it, so application shortcuts without Super always
reach the client. A lone tap of Super opens the app launcher.

## Design considerations

- **Every canvas shortcut includes Super** because Ctrl+arrows, Ctrl+D,
  Alt+arrows and Ctrl+F4 are application vocabulary; taking them would
  break apps. The device-level tests hold this rule, and
  [../bugs/grabs-that-never-ended.md](../bugs/grabs-that-never-ended.md)
  tells how they caught the exception.
- **A lone Super tap opens the launcher** because the Windows key is
  the muscle memory for "start"; the moment Super is used with another
  key it is only a modifier and never opens anything.
- **Windows' own virtual-desktop keys are kept** (Super+Ctrl+arrows,
  Super+Ctrl+D, Super+Ctrl+F4), so the habits transfer.
- **Click-to-focus with raise** is the Windows pointer model the title
  bars belong to ([../windows/title-bars.md](../windows/title-bars.md)).
- **A shell surface that takes typing gets the keyboard as it appears**
  (a panel, a launcher): what a click opened is typed into at once, and
  Escape closes it, without a second click. Focusing a window takes the
  keyboard back; a lock screen's exclusive claim beats both.

## Behavior

### Bindings

| Keys | Action |
|---|---|
| Tap Super alone, Super+Space | App launcher ([../apps/launcher.md](../apps/launcher.md)) |
| Super+Return | Terminal: `$TERMINAL`, else `foot` |
| Super+Q, Alt+F4 | Close the window ([../windows/window-management.md](../windows/window-management.md)) |
| Super+M / Super+Shift+M | Minimize / restore the last minimized window |
| Super+Up | Maximize toggle |
| Super+F | Fullscreen toggle |
| Super+Tab, Super+W | Overview toggle ([../canvas/overview.md](../canvas/overview.md)) |
| Super+Ctrl+Arrows | Hop, as a 3-finger swipe does: the desktop in that direction, else one screen that way ([../canvas/infinite-canvas.md](../canvas/infinite-canvas.md)) |
| Super+Ctrl+Shift+Arrows | Move the focused window to that desktop and follow |
| Super+Alt+Arrows | Pan a third of a screen ([../canvas/infinite-canvas.md](../canvas/infinite-canvas.md)) |
| Super+Ctrl+D | Pin the current view as a new desktop ([../canvas/desktops.md](../canvas/desktops.md)) |
| Super+Ctrl+F4 | Abandon the current desktop ([../canvas/desktops.md](../canvas/desktops.md)) |
| Super+T | Switch tiling on or off for the current desktop; a free view is pinned as a desktop first ([../canvas/desktops.md](../canvas/desktops.md)) |
| Super+1..9 | Go to desktop N ([../canvas/desktops.md](../canvas/desktops.md)) |
| Super+0 | Return the view to 100 %: the desktop shown ([../canvas/desktops.md](../canvas/desktops.md)), or a free view ([../canvas/infinite-canvas.md](../canvas/infinite-canvas.md)) |
| Super+Shift+E, Ctrl+Alt+Backspace | Quit the compositor |
| Ctrl+Alt+F1..F12 | Switch virtual terminal |
| Overview keys | Arrows hop, Return go, Esc back, Delete/BackSpace abandon, `t` switch tiling, `m` combine, `n` new desktop, `0` the target desktop back to 100 %, `+`/`−` zoom ([../canvas/overview.md](../canvas/overview.md)) |

- Super+Ctrl+arrows and its kin are Windows' own virtual-desktop keys.
- Alt+F4 is the one close binding without Super, for Windows parity.
- Bindings live in a table in the input code next to the actions;
  sizes, colours and tuning in `src/core/config.h` (dwl-style: edit and
  rebuild).

### Pointer

| Subject | Rule |
|---|---|
| Focus | Click-to-focus, and the click raises the window |
| Drag over a window | Title-bar drag and edge/corner resize: [../windows/title-bars.md](../windows/title-bars.md) |
| Super+left-drag / Super+right-drag | Move / resize from anywhere on the window ([../windows/window-management.md](../windows/window-management.md)) |
| Press vs drag | A press becomes a drag only past a small travel (compile-time, `src/core/config.h`); under it, the release is a click |
| Drag or scroll on empty canvas | Pans ([../canvas/infinite-canvas.md](../canvas/infinite-canvas.md)) |
| Scroll over a client | Scrolls the client |
| In the overview | Pointer actions are [../canvas/overview.md](../canvas/overview.md)'s |

### Keyboard focus

| Holder | Rule |
|---|---|
| The focused window | Has the keyboard unless a shell surface above the windows holds it |
| A shell surface on the top or overlay layer asking for the keyboard on demand | Gets it when it appears and when it is clicked; gives it back to the focused window when it goes or a window is focused |
| A shell surface asking for the keyboard exclusively | Holds it while mapped, the topmost first; windows and on-demand surfaces cannot take it |
| No keyboard connected yet | Focus is assigned all the same, and the seat offers a keyboard from the start, so clients are ready: the first keyboard to connect (an on-screen one, `wtype`) types into the focus from its first key |

### Devices

| Setting | Rule |
|---|---|
| Tap to click | On |
| Natural scrolling | On |
| Disable touchpad while typing | On |

## Error states, corner cases

- **A canvas shortcut pressed with the overview open**: the overview's
  own unmodified keys (`t`, `m`, `n`, arrows) act on it; windows receive
  no keys while the compositor owns the view
  ([../canvas/infinite-canvas.md](../canvas/infinite-canvas.md)), so
  nothing is stolen. A shell surface holding the keyboard (a panel, the
  note editor) takes the keys before the overview does.
- **A lone Super tap nested inside another compositor**: passes through
  to the host, so the host's own Super bindings keep working; only on
  its own outputs does the tap open the launcher.
- **Gestures while a client holds a pointer grab**: compositor gestures
  still act on the canvas ([gestures.md](gestures.md)); client-bound
  gestures follow the client's grab.

## Cross-links

- [gestures.md](gestures.md),
  [../canvas/infinite-canvas.md](../canvas/infinite-canvas.md),
  [../canvas/desktops.md](../canvas/desktops.md),
  [../canvas/overview.md](../canvas/overview.md),
  [../windows/title-bars.md](../windows/title-bars.md),
  [../windows/window-management.md](../windows/window-management.md),
  [../apps/launcher.md](../apps/launcher.md),
  [../bugs/grabs-that-never-ended.md](../bugs/grabs-that-never-ended.md),
  [../workspace/build-and-run.md](../workspace/build-and-run.md).
