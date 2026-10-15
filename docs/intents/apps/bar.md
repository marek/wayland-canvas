# Top bar

> Owns the top system bar: its look, its sections and widgets, what each
> click does, and how it starts. The control socket the logo button uses is
> in [../platform/ipc.md](../platform/ipc.md); the launcher it opens is in
> [launcher.md](launcher.md).

## Requirement

Every output has a top system bar drawn by a separate Quickshell client
(`shell/`, one layer-shell panel per output with an exclusive zone), styled
after the bar of the user's previous desktop minus the workspace numbers.
The compositor stays free of widget code: the bar is a client like any
other, driving the compositor only through the control IPC and standard
system services.

## Design considerations

- **A separate client, not compositor UI.** The bar reads battery, network,
  bluetooth, audio and tray state over Quickshell's service backends and
  runs standard tools for actions; the only compositor-specific actions
  (opening the launcher, the overview, ending the session) go through the
  control socket ([../platform/ipc.md](../platform/ipc.md)).
- **Own theme.** The palette, font and glyphs live in `shell/`: a dark
  theme — background `#1a1b26`, text `#a9b1d6`, attention `#f7768e` — with
  the bar 26 logical px high. The bar never reads another shell's
  configuration.
- **Icons are font glyphs**, not images: the bar renders in the fontconfig
  `monospace` family (12 px text, 13 px icons) so it follows the system
  font, and the icon shapes come from that font's icon ranges.

## Behavior

One flat panel across the top edge of every output, an 8 px margin to the
screen edge on each side, widgets in three sections — left, horizontally
centred, right. Buttons are text with no hover background; icon widgets
sit in 27 px slots.

| Section | Widgets, in order |
|---|---|
| Left | logo button |
| Center | clock, weather |
| Right | tray, bluetooth, network, audio, monitor, power |

Widgets and what a click does (anything not listed does nothing):

| Widget | Shows | Left | Other |
|---|---|---|---|
| Logo | logo glyph | toggle the launcher (IPC) | right: open `$TERMINAL` |
| Clock | `dddd HH:mm` | — | right: cycle to `d MMMM 'W'ww yyyy` |
| Weather | condition glyph + temperature (wttr.in) | open the forecast in a browser | middle/right: re-fetch |
| Tray | system-tray items | activate the item | right: secondary-activate |
| Bluetooth | adapter state, connected mark with a device | open `bluetoothctl` in `$TERMINAL` | right: toggle the radio |
| Network | connection type and Wi-Fi strength | open `nmtui` in `$TERMINAL` | — |
| Audio | default-sink glyph, crossed when muted | toggle mute | wheel: volume ±5 % (`wpctl`) |
| Monitor | display glyph | — | wheel: brightness ±5 % (`brightnessctl`) |
| Power | battery level and charging state | power menu: suspend, restart, power off, log out | right: toggle the percentage |

Widgets without a working source hide themselves: weather until the first
successful fetch, bluetooth without an adapter, monitor without a
controllable backlight, power without a battery.

Out of scope: workspace numbers (asked without), a keyboard-layout
switcher (there is no compositor-side layout-switch surface to drive), and
state indicators or update/agent status (another shell's own state and
tooling, no standard source to read).

## Autostart

`Server::start()` launches the bar when `qs` is on `PATH` (constants in
`src/core/config.h`). The bar's config is found beside the compositor binary —
`../shell` in the build tree, `../share/wayland-canvas/shell` when
installed (meson installs `shell/` under the data dir) — or at
`$WAYLAND_CANVAS_SHELL_DIR` when set. `-B` (and `Options::start_bar`)
starts the compositor without it.

## Cross-links

- [../platform/ipc.md](../platform/ipc.md) — the socket the logo button
  and the power menu talk to.
- [launcher.md](launcher.md) — the launcher the logo button opens.
- [../platform/outputs.md](../platform/outputs.md) — layer-shell usable
  area: the bar's exclusive zone is why maximized windows start below it.
