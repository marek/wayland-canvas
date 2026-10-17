# Settings window

> Owns the settings window: where it opens, its pages, how a shortcut is
> recorded, and what each page writes. The file most of it edits is
> [../input/config-file.md](../input/config-file.md); the shell's own config
> is [plugins.md](plugins.md)'s.

## Requirement

A gear button in the overview toolbar opens the settings: every keyboard
shortcut, set by pressing it; the swipe preset and the pinch switch; the
toolbar's edge; which bar widgets show. A change applies at once and is
written to the file that holds it, and a hand edit of that file shows in the
window while it is open.

## Design considerations

- **A shortcut is set by pressing it.** The compositor records the chord
  (`shortcut.capture`, [../platform/ipc.md](../platform/ipc.md)), so the chord
  runs nothing while it is being set — not even the shortcut it already is —
  and no app sees it.
- **Gestures are chosen, not recorded**: a few presets, each keeping the
  gesture vocabulary whole ([../input/gestures.md](../input/gestures.md)).
- **The window is a plugin**, `canvas.settings`, like the rest of the shell
  ([plugins.md](plugins.md)): it changes the compositor only through the
  control socket and the shell only through the host.
- **The files stay the truth.** The window keeps nothing of its own: it shows
  what the files say and writes them.

## Behavior

| Subject | Rule |
|---|---|
| Opening | The gear button in the overview toolbar ([toolbar.md](toolbar.md)); the window is a panel beside it, through the common trigger |
| Closing | As any panel: a click outside, Escape, a second click of the gear, leaving the overview. Closing while a shortcut is being recorded cancels the recording |
| Pages | Tabs: Shortcuts, Gestures, Session, Toolbar, Bar; a footer names the two files |

| Page | Holds | Writes |
|---|---|---|
| Shortcuts | The file's errors first, if any. Then every action by group in the bindings table's order, the overview's last: its label and its keys, or None. A click records new keys; reset puts the defaults back; clear leaves it without keys. Then the commands: each one's keys, recorded the same way, and its command line; add and remove | `canvas.conf`: `[shortcuts]`, `[overview]`, `[commands]` |
| Session | The minutes of idleness before the screen locks, or never ([lock.md](lock.md)); the night light's switch and temperature ([../render/night-light.md](../render/night-light.md)) | `canvas.conf`: `[session]` |
| Gestures | The three swipe presets, a line each saying what it does; the switch "Pinch to scale a desktop (Super + 2 fingers)" | `canvas.conf`: `[gestures]` |
| Toolbar | The edge: right, left, top or bottom | `shell.json`: `toolbar.position` |
| Bar | Every bar widget by section, in bar order, with a switch; a widget that hides itself without a source says when it shows | `shell.json`: `bar.hidden` |

### Recording a shortcut

| Input | What happens |
|---|---|
| A click on the action's row | The row says "Press the new keys — Esc cancels"; from then on the compositor takes every key until a chord is pressed |
| A chord | It replaces that action's keys; a chord another action or command held is taken from it, and the row says where it came from |
| Super pressed and released alone | The tap, `Super` |
| Esc alone | Cancels; the keys stay |
| A chord not allowed for the action | The row says why (a typing key without Super, Ctrl or Alt); the keys stay |

## Error states, corner cases

- **The file changed by hand while the window is open**: the window shows the
  new state at once, its errors included.
- **The file unwritable**: the change fails on its row with the reason.

## Cross-links

- [../input/config-file.md](../input/config-file.md) — the file, its
  resolution and its edits.
- [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md) — the
  actions and their default keys.
- [../input/gestures.md](../input/gestures.md) — the presets.
- [toolbar.md](toolbar.md) — the gear's toolbar and its edges.
- [bar.md](bar.md) and [plugins.md](plugins.md) — the widgets and the shell's
  config.
- [../platform/ipc.md](../platform/ipc.md) — the socket methods the window
  uses.
