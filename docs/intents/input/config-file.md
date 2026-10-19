# Config file

> Owns `canvas.conf`: where it lives, the first run, its sections and
> chords, how a line resolves against the defaults, reloading, errors and
> edits. The shortcuts it names are
> [keyboard-and-pointer.md](keyboard-and-pointer.md)'s, the gesture choices
> [gestures.md](gestures.md)'s; the window that edits it is
> [../shell/settings.md](../shell/settings.md).

## Requirement

Every keyboard shortcut and the touchpad gesture choices change without a
rebuild, in a commented text file the compositor writes with every default
on its first run and re-reads whenever it is saved. The settings window edits
the same file, so the two never disagree.

## Design considerations

- **A file the user reads and edits by hand**, in the manner of Omarchy's
  own configs: `name = value` lines in sections, with comments. The first run
  writes every default into it, so the file is the list of what can change.
- **A line overrides, a missing line defaults.** An action the file does not
  name keeps its defaults, so deleting a line resets it and actions a later
  version adds arrive bound; `none` leaves an action without keys.
- **The file beats the defaults.** A chord written for one action is taken
  from any action that holds it only by default, so a rebinding never
  collides with a default the user cannot see in the file.
- **Edits keep the user's file**: the settings window rewrites only the line
  it changes; comments, order and every other line stay as written.
- **Only what the user acts with is here.** Tuning constants — thresholds,
  sizes, colours — stay compile-time in `src/core/config.h`; the shell's own
  settings (which bar widgets show, the toolbar's edge) are its `shell.json`
  ([../shell/plugins.md](../shell/plugins.md)).

## Behavior

### The file

| Subject | Rule |
|---|---|
| Place | `$XDG_CONFIG_HOME/wayland-canvas/canvas.conf` (default `~/.config/wayland-canvas/canvas.conf`) |
| First run | Missing at start, it is written, its directory created: a header saying how the file works, then every section with every action at its defaults in the order of the bindings table, a comment naming each group, an empty `[commands]` with a commented example, `[gestures]` at its defaults with a comment per choice, and `[session]` likewise. A file that exists is never rewritten whole |
| Reload | Saving it — written in place or renamed over, as editors do — applies it to the running session; deleting it puts every default back |
| Syntax | `[section]` headers, `name = value` lines, whole-line `#` comments, blank lines; a `#` inside a value is part of the value |

### Sections

| Section | Line | Meaning |
|---|---|---|
| `[shortcuts]` | `<action> = <chords>` | That action's keys everywhere but in the overview ([keyboard-and-pointer.md](keyboard-and-pointer.md)) |
| `[overview]` | `<action> = <chords>` | That action's keys while the overview is open ([../canvas/overview.md](../canvas/overview.md)) |
| `[commands]` | `<chord> = <command>` | The chord runs the command through `/bin/sh`, as the terminal shortcut runs its own: started from the overview, its windows open in the overview's target |
| `[gestures]` | `swipes = three-finger-hops`, `four-finger-hops` or `off`; `pinch-scale = on` or `off`; `mouse-scroll`, `touchpad-scroll` and `pan = natural` or `traditional`; `overview-swipe = up` or `down`; `hop-swipe = toward` or `away` | The gesture choices and the direction of each kind of scroll and swipe ([gestures.md](gestures.md)) |
| `[session]` | `lock-idle = <minutes>` or `off`; `night-light = on` or `off`; `night-light-temperature = <kelvin>` | How long the session may idle before it locks ([../shell/lock.md](../shell/lock.md)) and the night light ([../render/night-light.md](../render/night-light.md)) |

### Chords

| Subject | Rule |
|---|---|
| Form | Modifiers, then one key, joined by `+`: `Super+Shift+3`, `Ctrl+Alt+BackSpace`. Several chords for one action are separated by commas; `none` is no chord |
| Modifiers | `Super`, `Ctrl`, `Alt`, `Shift`: read in any case, written in that order |
| Key | An XKB key name, read in any case; written as the capital letter for a letter, the digit for a digit, else the key name with a capital first letter (`Space`, `Return`, `Print`, `Left`, `F4`, `Plus`) |
| Tap | `Super` alone is a lone tap of the Super key — pressed and released with nothing between ([keyboard-and-pointer.md](keyboard-and-pointer.md)). `Super Super`, two such taps within a compile-time interval, is the double tap. No other modifier stands alone |
| Outside the overview | A chord needs Super, Ctrl or Alt, or a key that types nothing (the function keys, Print, Pause, the media keys); `Super` and `Super Super` are allowed. A typing key alone or with only Shift would take typing away from the apps |
| In the overview | Any chord: the overview owns the keyboard |

### Resolution

| Case | Keys |
|---|---|
| The file has a line for the action | That line's chords |
| It has none | The action's defaults, less every chord the file gives to something else in the same scope |
| One chord on two lines of a scope | The first line keeps it; the later one is an error |
| Scopes | `[shortcuts]` and `[commands]` share one; `[overview]` is its own, so Super+Tab is the overview shortcut outside the overview and leaves it inside |

### Errors

A line that cannot apply — an unknown section or action, a chord that does
not parse or is not allowed in its scope, an unknown value, a line before any
section — is ignored with its line number, and everything else applies. The
errors are logged and listed by the settings window, and a reload that has
any shows the first as a hint on every screen (`canvas.conf line 12: unknown
action 'lancher'`, with how many more).

### Edits

| Subject | Rule |
|---|---|
| A change | Replaces the line that sets that entry, else is added at the end of its section — the section at the end of the file when missing. Every other byte stays |
| A chord taken | Setting a chord that another action or command holds takes it from there, rewriting that line too, and the window says where it came from |
| Reset | Writes the action's defaults into its line |
| Writing | The new file is written beside the old one and renamed over it, so neither an editor nor a crash sees half a file; the change applies at once |

## Error states, corner cases

- **The file unreadable, or its directory missing**: every default applies.
- **The file unwritable**: a change from the settings window fails with that
  reason; the running settings stay.
- **Every way to an action removed** (its keys `none`, its gestures off):
  allowed; the file can always be edited, and a deleted line restores the
  defaults.
- **VT switching** (Ctrl+Alt+F1..F12) is not in the file and no chord can
  take it: the way out of a stuck session stays fixed.

## Cross-links

- [keyboard-and-pointer.md](keyboard-and-pointer.md) — the bindings table:
  every action's name and default keys.
- [gestures.md](gestures.md) — the swipe presets, the pinch switch and the
  direction switches.
- [../shell/settings.md](../shell/settings.md) — the window that edits the
  file, and how a shortcut is recorded.
- [../platform/ipc.md](../platform/ipc.md) — `settings.get`, `settings.set`,
  `shortcut.capture`.
- [../shell/plugins.md](../shell/plugins.md) — `shell.json`, the shell's own
  config.
