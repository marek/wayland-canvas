# Keyboard and pointer

> Owns the key bindings — every action, its name and its default keys —
> the Super tap, and the pointer model: focus, drags, scroll, touchpad
> settings, nested-mode notes. How the keys are changed is
> [config-file.md](config-file.md); title bars and their hit zones are
> [../windows/title-bars.md](../windows/title-bars.md); window states and
> placement [../windows/window-management.md](../windows/window-management.md);
> the launcher [../apps/launcher.md](../apps/launcher.md); touchpad gestures
> [gestures.md](gestures.md).

## Requirement

Super (the Windows / logo key) is the compositor's key: every default canvas
shortcut includes it, so application shortcuts without Super reach the
client. Super+Space opens the app launcher. Every shortcut is the user's to
change, in `canvas.conf` or by pressing the new keys in the settings window.

## Design considerations

- **Every canvas shortcut includes Super** because Ctrl+arrows, Ctrl+D,
  Alt+arrows and Ctrl+F4 are application vocabulary; taking them would
  break apps. The device-level tests hold this rule, and
  [../bugs/grabs-that-never-ended.md](../bugs/grabs-that-never-ended.md)
  tells how they caught the exception.
- **Super+Space opens the launcher**, the chord the user's other desktop
  opens apps with. A lone Super tap runs nothing by default; it is a key the
  user can give any action (`Super` in [config-file.md](config-file.md)),
  and the moment Super is used with another key it is only a modifier.
- **The table below is the defaults.** `canvas.conf` changes any action's
  keys ([config-file.md](config-file.md)), and the settings window records
  new ones by pressing them ([../shell/settings.md](../shell/settings.md)).
  The way out stays fixed: VT switching is in no file and no chord takes it.
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

| Default keys | Name in `canvas.conf` | Action |
|---|---|---|
| Super+Space | `launcher` | App launcher ([../apps/launcher.md](../apps/launcher.md)) |
| Super+Return | `terminal` | Terminal: `$TERMINAL`, else `foot` |
| Super+Q, Alt+F4 | `close` | Close the window ([../windows/window-management.md](../windows/window-management.md)) |
| Super+M / Super+Shift+M | `minimize` / `restore` | Minimize / restore the last minimized window |
| Super+arrows | `snap-left`, `snap-right`, `snap-up`, `snap-down` | On a tiled desktop, step the focused window's slice: the far half becomes the whole, the whole the near half, the near half stays (both orders reach the quarters); covered windows are pushed into the largest free slice, or the halving splits one. On a floating desktop only `snap-up` does anything: maximize toggle |
| Super+F | `fullscreen` | Fullscreen toggle |
| Super+Tab, Super+W | `overview` | Overview toggle ([../canvas/overview.md](../canvas/overview.md)) |
| Super+Ctrl+Arrows | `hop-left` … `hop-down` | Hop, as a 3-finger swipe does: the desktop in that direction; none that way, the view stays ([../canvas/infinite-canvas.md](../canvas/infinite-canvas.md)) |
| Super+Ctrl+Shift+Arrows | `move-left` … `move-down` | Move the focused window to that desktop and follow |
| Super+Alt+Arrows | `pan-left` … `pan-down` | Pan a third of a screen ([../canvas/infinite-canvas.md](../canvas/infinite-canvas.md)) |
| Super+Ctrl+D | `pin-desktop` | Pin the current view as a new desktop ([../canvas/desktops.md](../canvas/desktops.md)) |
| Super+Ctrl+F4 | `abandon-desktop` | Abandon the current desktop ([../canvas/desktops.md](../canvas/desktops.md)) |
| Super+T | `tiling` | Switch tiling on or off for the current desktop; a free view is pinned as a desktop first ([../canvas/desktops.md](../canvas/desktops.md)) |
| Super+1..9 | `desktop-1` … `desktop-9` | Go to desktop N ([../canvas/desktops.md](../canvas/desktops.md)) |
| Super+0 | `zoom-reset` | Return the view to 100 %: the desktop shown ([../canvas/desktops.md](../canvas/desktops.md)), or a free view ([../canvas/infinite-canvas.md](../canvas/infinite-canvas.md)) |
| Super+Shift+3 / 4 / 5 | `shot-screen` / `capture-pick` / `capture-toolbar` | Whole-screen shot / the capture picker / the capture toolbar ([../apps/capture.md](../apps/capture.md)); with Ctrl added, the `-copy` action of each copies the shot |
| Print / Shift+Print / Alt+Print | `capture-pick` / `shot-screen` / `record-stop` | The picker / whole-screen shot / stop a recording, else the toolbar on its recording choices; Ctrl added, the `-copy` actions ([../apps/capture.md](../apps/capture.md)) |
| Super+Shift+E, Ctrl+Alt+Backspace | `quit` | Quit the compositor |
| Ctrl+Alt+F1..F12 | — | Switch to that VT; fixed |
| In the overview | `[overview]`: `hop-left` … `hop-down`, `go`, `back`, `abandon`, `tiling`, `combine`, `new-desktop`, `zoom-reset`, `zoom-in`, `zoom-out`, `leave` | Arrows hop, Return go, Esc back, Delete/BackSpace abandon, `t` switch tiling, `m` combine, `n` new desktop, `0` the target desktop back to 100 %, `+`/`−` zoom, Super+Tab / Super+W leave ([../canvas/overview.md](../canvas/overview.md)) |

- A binding matches the keysym the layout produces, lower-cased, and —
  for the shot chords' digits — the key's unshifted symbol as well, so
  Super+Shift+3 is `3` on a US layout where Shift+3 is `#`.
- Super+Ctrl+arrows and its kin are Windows' own virtual-desktop keys.
- Alt+F4 is the one default close binding without Super, for Windows parity.
- In the overview its own keys come first, then the capture chords, the
  launcher, the terminal and the `[commands]` chords; every other key does
  nothing there. The capture chords also work while the capture UI holds
  the keyboard, so the chord that opened it closes it.
- While the launcher is open, its own keys close it.
- A lone Super tap runs whichever action holds `Super`, in the normal view
  and in the overview.
- Sizes, colours and tuning stay in `src/core/config.h` (dwl-style: edit and
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
| A chord being recorded for the settings | The compositor takes every key, modifiers included, until the chord is pressed or the recording is cancelled: nothing runs and no surface sees them ([../shell/settings.md](../shell/settings.md)) |

### Devices

| Setting | Rule |
|---|---|
| Tap to click | On: one finger is a left click, two a right click, three a middle click |
| Pressing the pad | Two fingers pressed anywhere are a right click, three a middle click (libinput's clickfinger method, as the other desktop sets it); off, the pad's bottom-right corner is the right button |
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
  its own outputs does the tap run the action that holds it.
- **Gestures while a client holds a pointer grab**: compositor gestures
  still act on the canvas ([gestures.md](gestures.md)); client-bound
  gestures follow the client's grab.

## Cross-links

- [config-file.md](config-file.md), [gestures.md](gestures.md),
  [../shell/settings.md](../shell/settings.md),
  [../canvas/infinite-canvas.md](../canvas/infinite-canvas.md),
  [../canvas/desktops.md](../canvas/desktops.md),
  [../canvas/overview.md](../canvas/overview.md),
  [../windows/title-bars.md](../windows/title-bars.md),
  [../windows/window-management.md](../windows/window-management.md),
  [../apps/launcher.md](../apps/launcher.md),
  [../bugs/grabs-that-never-ended.md](../bugs/grabs-that-never-ended.md),
  [../workspace/build-and-run.md](../workspace/build-and-run.md).
