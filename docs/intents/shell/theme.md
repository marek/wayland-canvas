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
- **The applications' preference is the compositor's to write**: every
  `theme.set` — the shell's switch, the shell's start push, an outside
  client's call — makes the compositor set `org.gnome.desktop.interface`
  `color-scheme` to match, but only when it is the real session (the DRM
  backend, started from a TTY). The compositor alone knows its own
  backend, so the shell needs to know nothing; a nested or headless run
  never writes the setting.
- **The switch is a plugin** (`canvas.theme`, the right-most bar widget)
  like every bar widget, so another plugin can read the mode, follow it or
  offer its own switch.

## Behavior

| Subject | Rule |
|---|---|
| Switch | The right-most bar widget: a sun glyph in dark mode, a moon in light mode; a click switches |
| Dark | The palette the bar was born with: background `#1a1b26`, text `#a9b1d6`, attention `#f7768e` |
| Light | A matching light palette: background `#e1e2e7`, text `#3760bf`, attention `#f52a65` |
| The shell | Every plugin colours itself from `api.theme.colors` and changes with it |
| The compositor | Title bars, the canvas background and grid, the overview UI, the launcher and hints switch palettes on `theme.set` |
| Contrast | Text on an accent fill — the overview's target tab, the launcher's selected row, the calendar's today, a field's selection — is the palette's contrasting on-accent colour in both modes |
| Kept | The mode is written to `shell.json` as `theme.mode` and applied when the shell starts; the compositor starts dark until told |
| Applications | Every applied mode sets the applications' preference (`color-scheme` `prefer-light` / `prefer-dark`) — the real session only; see Design considerations |

| Adoption | A mode the compositor reports in its `theme` event — an outside client's `theme.set` — is adopted as if the switch had been clicked: palette flipped, kept in `shell.json`, never told back; at start the kept mode still wins |

### The palettes

The UI kit's `UiTheme` carries the shell's; the compositor's (title bars,
canvas, overview, launcher) mirrors it in `src/core/config.h`. Every
colour the shell or the compositor draws is one of these roles — nothing
is hard-coded where a palette can follow the mode.

| Role | Dark | Light | What it colours |
|---|---|---|---|
| background | `#1a1b26` | `#e1e2e7` | the bar, the canvas |
| text | `#a9b1d6` | `#3760bf` | labels, glyphs, titles |
| attention | `#f7768e` | `#f52a65` | warnings, the active glyph |
| muted | `#565f89` | `#8489a3` | details, section headers, placeholders |
| accent | `#7aa2f7` | `#2e7de9` | selection fills, sliders' fills, today |
| on accent | `#1a1b26` | `#ffffff` | text on an accent fill |
| panel | `#1a1b26` | `#f4f5f8` | panel cards, overview labels |
| border | white 10 % | black 12 % | a card's hairline |
| selected | accent at 22 % | accent at 16 % | the chosen row |
| hover / fill / track | white 8 / 6 / 14 % | black 8 / 6 / 14 % | hovered rows, fields' fills, off switches |

The compositor's palette adds what only it draws (title and button
colours, frames, the dim over wallpapers, launcher surfaces), each with
its own light value, and the same on-accent rule: the target tab's label
and glyphs — the Tile switch's wash and glyph included when it is lit —
and the launcher's selected row are drawn in the contrasting accent text
colour, which near-black on the dark mode's mid-blue and white on the
light mode's dark blue is.

## Error states, corner cases

- No control socket: the shell switches alone.
- An unknown `theme.mode` in the config: dark, logged.
- No `org.gnome.desktop.interface` schema, or `color-scheme` not writable
  (a real session without the desktop's schemas): the preference is not
  set, logged; the mode itself is unaffected.
- A compositor `theme` event with an unknown mode: ignored, like an
  unknown `theme.mode` in the config.

## Cross-links

- [bar.md](bar.md) — the bar the switch sits on.
- [plugins.md](plugins.md) — `api.theme`.
- [../windows/title-bars.md](../windows/title-bars.md) — the title bars
  that follow the mode.
