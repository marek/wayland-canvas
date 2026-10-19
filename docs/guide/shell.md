# The shell and its plugins

When `qs` (Quickshell) is on `PATH`, the compositor starts its shell, a
separate client in `shell/` (`-B` starts without it). The shell is a plugin
host: everything it shows comes from plugins, the built-in ones in `plugins/`
and your own.

- **Top bar**: a 26 px bar on every output, with an exclusive zone so a
  maximized window starts below it. The logo button left (click: launcher,
  right-click: terminal), clock and weather centred; tray, Bluetooth, network,
  audio, brightness, battery and the light/dark switch right. Each indicator
  opens its panel under it (Wi-Fi networks, Bluetooth devices, volume and the
  output device, the backlight, power profiles and the session menu, the
  forecast, a calendar); widgets without a working source hide themselves.
- **Overview toolbar**: on an edge of a screen while it shows the overview —
  the right by default, else the left, top or bottom as `toolbar.position` in
  the shell config names — holding the buttons plugins contribute: each runs
  an action, opens a panel or folds out a submenu, and everything it opens
  opens toward the screen's centre. A toolbar on the bottom edge carries the
  overview's shelf and help line above it.
- **Sticky notes**: the toolbar's Note button and its colour menu put a note on
  the canvas; type in it where it sits and it grows to hold the text, drag it
  by its strip, size it by its corner. The strip recolours it, sets bold,
  italic and underline on the selection and the note's font and size, and its
  × deletes it; each note is kept as an HTML file of its own.
- **Light and dark**: the switch flips the shell and everything the compositor
  draws, and the choice is kept. When wayland-canvas is the session itself
  (started from a TTY or a login manager), it also sets the applications'
  preference, GNOME's `color-scheme`, which GTK, Qt through the portal and
  browsers follow; nested and headless runs leave it alone.
- **Settings**: the toolbar's gear opens the settings window — every keyboard
  shortcut, the swipe presets, the toolbar's edge and which bar widgets show
  ([Configuration](configuration.md)).

## Plugins

A plugin is a folder with a `plugin.json` manifest. Plugins are found built
in, then in `$XDG_DATA_HOME/wayland-canvas/plugins`, then in the config's
`pluginPaths`; a later one replaces an earlier one with the same id. The
config is `$XDG_CONFIG_HOME/wayland-canvas/shell.json` (see
[Configuration](configuration.md)): `plugins.<id>.enabled: false` turns a
plugin off, `bar.layout` and `toolbar.order` arrange widgets and buttons, and
edits to it or to a plugin apply while the shell runs.
[../intents/shell/plugins.md](../intents/shell/plugins.md) has the manifest,
the API and the UI kit. The shell itself is found beside the compositor
binary (build tree or install prefix) or at `$WAYLAND_CANVAS_SHELL_DIR`.

## See also

[Configuration](configuration.md) for the settings window and `shell.json`,
and [Working on wayland-canvas](development.md) for the `shell/` and
`plugins/` folders.
