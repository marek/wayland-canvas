# Top bar

> Owns the top system bar: its look, its sections and widgets, what each
> click does, the panels under its indicators, and how it starts. The control
> socket the logo button uses is in [../platform/ipc.md](../platform/ipc.md);
> the launcher it opens is in [../apps/launcher.md](../apps/launcher.md).

## Requirement

Every output has a top system bar drawn by a separate Quickshell client
(`shell/`, the plugin host, whose `canvas.bar` plugin opens one layer-shell
panel per output with an exclusive zone), styled after the bar of the
user's previous desktop minus the workspace numbers.
Each indicator opens a panel below it with that indicator's controls — no
indicator launches a terminal program. The compositor stays free of widget
code: the bar is a client like any other, driving the compositor only
through the control IPC and standard system services.

## Design considerations

- **A separate client, not compositor UI.** The bar reads battery, network,
  bluetooth, audio and tray state over Quickshell's service backends and
  acts through them (plus `brightnessctl` for the backlight and `systemctl`
  for power actions); the only compositor-specific actions (opening the
  launcher, ending the session) go through the control socket
  ([../platform/ipc.md](../platform/ipc.md)).
- **Own theme.** The palette, font, glyphs and panel metrics live in the
  shell's UI kit (`shell/ui/WaylandCanvas/Ui/`, the `WaylandCanvas.Ui`
  module every plugin imports): a dark theme — background `#1a1b26`, text
  `#a9b1d6`, attention `#f7768e` — with the bar 26 logical px high. The
  bar never reads another shell's configuration.
- **Icons are font glyphs**, not images: the bar renders in the fontconfig
  `monospace` family (12 px text, 13 px icons) so it follows the system
  font, and the icon shapes come from that font's icon ranges.
- **Panels are the bar client's own overlay surfaces**, a layer-shell
  surface per panel holding a card anchored to its indicator. A popup
  cannot hang from a layer-shell bar in Qt, and an overlay surface that
  asks for the keyboard on demand gets it as it opens under the general
  focus rule ([../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)):
  no compositor-side special casing.

## Behavior

One flat panel across the top edge of every output, an 8 px margin to the
screen edge on each side, widgets in three sections — left, horizontally
centred, right. Buttons are text with no hover background; icon widgets
sit in 27 px slots.

| Section | Widgets, in order |
|---|---|
| Left | logo button |
| Center | clock, weather |
| Right | tray, bluetooth, network, audio, brightness, power |

Widgets and what a click does:

| Widget | Shows | Left | Other |
|---|---|---|---|
| Logo | logo glyph | toggle the launcher (IPC) | right: open `$TERMINAL` |
| Clock | `dddd HH:mm` | open the calendar panel | right: cycle to `d MMMM 'W'ww yyyy` |
| Weather | condition glyph + temperature (wttr.in) | open the forecast panel | middle/right: re-fetch |
| Tray | system-tray items | activate the item | right: the item's menu |
| Bluetooth | adapter state, connected mark with a device | open the Bluetooth panel | right: toggle the radio |
| Network | connection type and Wi-Fi strength | open the network panel | — |
| Audio | default-sink glyph, crossed when muted | open the audio panel | wheel: volume ±5 % |
| Brightness | display glyph | open the brightness panel | wheel: brightness ±5 % (`brightnessctl`) |
| Power | battery level and charging state | open the power panel | right: toggle the percentage |

Widgets without a working source hide themselves: weather until the first
successful fetch, bluetooth without an adapter, brightness without a
controllable backlight, power without a battery.

## Panels

Each indicator opens one panel: a card anchored under its icon and kept
inside the output. A panel closes on a click outside it, on Escape and on a
second click of its icon; only one panel is open at a time. While a panel is
open it holds the keyboard, so a text field in it receives typing without
anything else being clicked.

| Panel | Offers |
|---|---|
| Network | the Wi-Fi switch; the visible networks with signal (in percent) and lock; the connection in use. The Wi-Fi device scans while the panel is open, so networks the system has never joined are listed, and never in the background. Selecting a network arms the connect flow — a password field for secured networks the system has no credentials for, Connect, Disconnect — and never connects by itself: Connect or Enter in the field does |
| Bluetooth | the power switch; a scan switch; paired devices first, then found ones, each with its state. A device row's click is its natural action: connect or disconnect a paired device, pair a found one |
| Audio | a volume slider for the default sink; mute; the output device to make default |
| Brightness | a slider for the backlight, which never steps below the minimum |
| Power | battery level, state and time left; the power profile where the system offers one (balanced always, power saver with it, performance where the platform has it); suspend, restart, power off, log out |
| Weather | the forecast for the coming days, from the same reading the widget shows |
| Clock | a month calendar with today marked; ‹ and › step the month |
| Tray | the item's own menu — entries, checkboxes, separators and submenus (with a way back out); triggering an entry closes it. An item without a menu secondary-activates instead |

Out of scope: workspace numbers (asked without), a keyboard-layout
switcher (there is no compositor-side layout-switch surface to drive), and
state indicators or update/agent status (another shell's own state and
tooling, no standard source to read).

## Autostart

`Server::start()` launches the shell when `qs` is on `PATH` (constants in
`src/core/config.h`). The shell's config is found beside the compositor
binary — `../shell` in the build tree, `../share/wayland-canvas/shell` when
installed (meson installs `shell/` and the built-in `plugins/` under the
data dir, the plugins beside the shell where the host finds them) — or at
`$WAYLAND_CANVAS_SHELL_DIR` when set. It is started with `QML_IMPORT_PATH`
prefixed by the shell's `ui/` dir, so every plugin, wherever the host found
it, can `import WaylandCanvas.Ui`. `-B` (and `Options::start_bar`) starts
the compositor without it.

## Cross-links

- [../platform/ipc.md](../platform/ipc.md) — the socket the logo button
  and the power panel talk to.
- [../apps/launcher.md](../apps/launcher.md) — the launcher the logo button opens.
- [../platform/outputs.md](../platform/outputs.md) — layer-shell usable
  area: the bar's exclusive zone is why maximized windows start below it.
