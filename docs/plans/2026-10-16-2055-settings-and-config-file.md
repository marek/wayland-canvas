# Settings and the config file

Date: 2026-10-16 20:55 EDT

## Context

Every keyboard shortcut and touchpad gesture changes without a rebuild, in two ways that stay in step: a settings
window opened from a gear button in the overview toolbar, and `~/.config/wayland-canvas/canvas.conf`, a commented
file in the manner of Omarchy's own, which the compositor writes with every default on first run and re-reads when
it is saved. In the window a shortcut is set by pressing it; swipe gestures are a choice of presets; the Super +
2-finger pinch that scales a desktop is a switch. The same window puts the overview toolbar on any edge of the
screen and turns each bar widget on or off, the agents widget included, which also shows when the compositor runs
from the build tree. Super+Space opens the launcher by default.

Settled:

- Two files, each read by the process it configures: `canvas.conf` (the compositor: shortcuts, commands, gestures)
  and `shell.json` (the shell: hidden bar widgets, the toolbar's edge). The window edits both.
- A `canvas.conf` line overrides its default; an action the file does not name keeps its default; `none` leaves an
  action without keys. The window rewrites only the lines it changes and keeps the comments.
- A shortcut is recorded through the compositor, which takes the chord so it runs nothing while it is being set.
- Swipe presets: three-finger hops (the current gestures), four-finger hops (the finger counts swapped), off (3- and
  4-finger gestures reach the apps). No free-pan preset: free panning stays with the pointer and the keys.
- The lone Super tap is no default; it is a key the user can give any action.
- Fixed, not configurable: VT switching (Ctrl+Alt+F1..F12) and the keys inside the launcher, the capture picker and
  the panels. Tuning constants stay compile-time in `src/core/config.h`.
- Out of scope: reordering bar widgets in the window (the file's `bar.layout` still does), recording a gesture.

The contract is in [input/config-file.md](../intents/input/config-file.md),
[shell/settings.md](../intents/shell/settings.md) and the docs they link to.

## Intents

- [x] The config file, the settings window, the binding table with action names, the launcher's keys, the gesture presets and the pinch switch, the toolbar's edge, the overview's bottom UI above a bottom toolbar, the shell config keys and api, the IPC methods and event

## Compositor

- [x] The settings library: actions with names, labels, groups and defaults; chords parsed and written; `canvas.conf` parsed with an error per bad line; the first-run file; one entry rewritten in place, comments kept
- [x] `canvas.conf` read at start (written when missing), re-read when saved; every shortcut, capture chord, overview key and the Super tap comes from it; `[commands]` chords run their commands
- [ ] `settings.get`, `settings.set`, `shortcut.capture` and `shortcut.cancel` on the control socket, and the `settings` event; a chord being recorded runs nothing and reaches no client (`shortcut.cancel` unexercised: check under way)
- [x] Super+Space opens the launcher, from the overview too; a lone Super tap only when the file gives it to an action
- [ ] The shell started from the build tree finds the build's helpers, so the agents widget shows there (the shell's PATH: check under way)

## Gestures

- [x] Swipe presets three-finger hops, four-finger hops and off, and the pinch-scale switch, taken from the config file

## Shell

- [ ] The settings plugin: the gear button in the toolbar and the settings panel — Shortcuts (record, reset, clear, commands), Gestures (preset, pinch switch), Toolbar (edge), Bar (a switch per widget) (reset, clear and commands from the panel unexercised: checks under way)
- [x] The bar leaves out the widgets `bar.hidden` names; the api the panel needs to write `shell.json` and list the widgets
- [x] The toolbar on any edge (`toolbar.position`), a row at the top and bottom, flyouts and panels opening toward the screen's centre; in the overview the shelf and help line sit above a bottom toolbar

## Verification

- [x] Unit: the settings library
- [x] Scenarios: the first run writes the file; a rebinding saved in the file takes effect; a chord recorded through the socket runs nothing; `settings.set` keeps the file's comments; a command chord runs; a bad line is reported; Super+Space opens the launcher and a lone tap does not
- [x] Scenarios: each swipe preset and the pinch switch
- [x] Scenarios: from the gear, recording a shortcut, choosing a preset, hiding the agents widget, moving the toolbar
- [x] Scenarios: the toolbar on each edge, its flyout toward the centre, the shelf above a bottom toolbar
- [ ] README and `-h`; warning-free build, every suite green
- [ ] On screen: the settings window, recording shortcuts, the presets on the touchpad, the toolbar on each edge (the user)
