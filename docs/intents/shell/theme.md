# Light and dark

> Owns the light/dark switch and what follows it: the switch's place on the
> bar, the two palettes, which parts follow the mode, and keeping the
> choice. The bar is [bar.md](bar.md); the compositor's half is the
> `theme.set` method of [../platform/ipc.md](../platform/ipc.md).

## Requirement

A button in the top-right corner of the bar switches between light and dark.
The bar, its panels, the overview toolbar and the note editor follow at once,
and so does what the compositor draws itself: title bars, the canvas, the
overview's tabs, frames, shelf and help line, the launcher and hints. The
choice is kept across restarts.

## Design considerations

- **One mode for the desktop, owned by the shell**: the shell keeps it in
  its config and tells the compositor when it starts and on every change,
  so the two never disagree for longer than a message.
- **The switch is a plugin** like every bar widget, so another plugin can
  read the mode, follow it or offer its own switch.

## Behavior

| Subject | Rule |
|---|---|
| Switch | The right-most bar widget: a sun glyph in dark mode, a moon in light mode; a click switches |
| Dark | The bar's palette as it is today: background `#1a1b26`, text `#a9b1d6`, attention `#f7768e` |
| Light | A matching light palette: background `#e1e2e7`, text `#3760bf`, attention `#f52a65` |
| The shell | Every plugin colours itself from `api.theme.colors` and changes with it |
| The compositor | Title bars, the canvas background and grid, the overview UI, the launcher and hints switch palettes on `theme.set` |
| Kept | The mode is written to `shell.json` as `theme.mode` and applied when the shell starts; the compositor starts dark until told |

## Error states, corner cases

- No control socket: the shell switches alone.
- An unknown `theme.mode` in the config: dark, logged.

## Cross-links

- [bar.md](bar.md) — the bar the switch sits on.
- [plugins.md](plugins.md) — `api.theme`.
- [../windows/title-bars.md](../windows/title-bars.md) — the title bars
  that follow the mode.
