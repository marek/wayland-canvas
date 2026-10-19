# Configuration

Everything you can change without rebuilding: keyboard shortcuts and gesture
choices in `canvas.conf`, the settings window that edits the same file, the
shell's `shell.json`, and what stays compile-time.

## canvas.conf

`~/.config/wayland-canvas/canvas.conf` ($XDG_CONFIG_HOME respected) holds
every keyboard shortcut and the touchpad gesture choices. The first run
writes it with every default, commented, so the file is the list of what
can change; a missing line means the default, `none` leaves an action
without keys, and saving it — written in place or renamed over, as editors
do — applies it to the running session at once. A line that cannot apply
(an unknown action or value, a chord that does not parse or is not allowed
there) is ignored with its line number, logged, and the first shows as a
hint on every screen; everything else applies.

```
[shortcuts]
launcher = Super+Space
terminal = Super+Return

[overview]
go = Return, KP_Enter

[commands]
# Super+B = foot --hold

[gestures]
swipes = three-finger-hops
pinch-scale = on
```

A chord written for one action is taken from any other that holds it only
by default, so a rebinding never collides with a default the file does not
show. Outside the overview a chord needs Super, Ctrl or Alt, or a key that
types nothing (the function, media and Print keys); in the overview any
chord works. In `[commands]` the chord runs its command through `/bin/sh`,
as the terminal shortcut runs its own.

## The settings window

The gear button in the overview toolbar opens the settings window, a panel
with five tabs. It edits the same `canvas.conf`, so the two never disagree,
and records a new shortcut by pressing it:

| Tab | Sets | Written to |
|---|---|---|
| Shortcuts | Every keyboard shortcut — click a row and press the new chord (Esc cancels); Reset restores the defaults, Clear leaves the action without keys; a chord another action holds is taken from it, and the row says where it came from. Below them, commands: a chord and a shell command line each | `canvas.conf` |
| Gestures | The swipe preset (three- or four-finger hops, or off) and the pinch-to-scale switch | `canvas.conf` |
| Session | How long the screen may idle before it locks (never is one of the choices), and the night light's switch and temperature | `canvas.conf` |
| Toolbar | The edge the overview toolbar sits on: right, left, top or bottom | `shell.json` |
| Bar | Which bar widgets show, every one by section with a switch | `shell.json` |

A change applies at once and is written to the file that holds it; a hand
edit of either file shows in the window while it is open. The footer names
both files.

## The shell's config

The shell's own settings — which bar widgets show, the toolbar's edge — are
its `shell.json` at `$XDG_CONFIG_HOME/wayland-canvas/shell.json`:
`plugins.<id>.enabled: false` turns a plugin off, `bar.layout` and
`toolbar.order` arrange widgets and buttons, and edits to it or to a plugin
apply while the shell runs. See [The shell and its plugins](shell.md) for
the plugins themselves.

## Compile-time settings

Sizes, colours and tuning stay compile-time in `src/core/config.h`
(dwl-style: edit and rebuild).

## See also

[Gestures, mouse and keyboard](controls.md) for the defaults these files
change, and [Working on wayland-canvas](development.md) for rebuilding.
