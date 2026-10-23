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
  ([../platform/ipc.md](../platform/ipc.md)). The network panel reads what
  the backend does not expose through NetworkManager's own CLI (`nmcli`
  for the connection's profile, addresses, band and DNS), the interface's
  byte counters in sysfs, `ping` to the gateway, and the head's optional
  tools: `qrencode` for the share-as-QR, `speedtest`/`speedtest-cli` for
  the speed test, `wl-copy` for copying an address — every one of them
  read-only or scoped to the connection the panel names.
- **Own theme, both modes.** The palette, font, glyphs and panel metrics
  live in the shell's UI kit (`shell/ui/WaylandCanvas/Ui/`, the
  `WaylandCanvas.Ui` module every plugin imports): the dark and light
  palettes of [theme.md](theme.md) — background `#1a1b26` / `#e1e2e7`,
  text `#a9b1d6` / `#3760bf`, attention `#f7768e` / `#f52a65` — with the
  bar 26 logical px high. The bar never reads another shell's
  configuration.
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
| Right | tray, agents, bluetooth, network, audio, brightness, notifications, power, theme |

Any widget can be left out of the bar: the shell config's `bar.hidden`
([plugins.md](plugins.md)), which the settings window's Bar page writes
([settings.md](settings.md)); the others keep their places and order.

Widgets and what a click does:

| Widget | Shows | Left | Other |
|---|---|---|---|
| Logo | logo glyph | toggle the launcher (IPC) | right: open `$TERMINAL`, a window of its own that a rebuild of the bar leaves open |
| Clock | `dddd HH:mm` | open the calendar panel | right: cycle to `d MMMM 'W'ww yyyy` |
| Weather | condition glyph + temperature (wttr.in) | open the forecast panel | middle/right: re-fetch |
| Tray | system-tray items | activate the item | right: the item's menu |
| Bluetooth | adapter state, connected mark with a device | open the Bluetooth panel | right: toggle the radio |
| Network | connection type and Wi-Fi strength | open the network panel | — |
| Audio | default-sink glyph, crossed when muted | open the audio panel | wheel: volume ±5 % |
| Brightness | display glyph | open the brightness panel | wheel: brightness ±5 % (`brightnessctl`) |
| Power | battery level and charging state | open the power panel | right: toggle the percentage |
| Agents | the robot glyph while any AI provider has usage to show | open the agents panel | middle/right: re-run the collector |

Widgets without a working source hide themselves: weather until the first
successful fetch, bluetooth without an adapter, brightness without a
controllable backlight, power without a battery, agents while no provider
has any usage.

What a widget shows belongs to its plugin, not to the bar it is on: with
two outputs a wheel step on either bar's brightness shows on both, and the
weather and the agents' usage are fetched once however many bars there are.

## Panels

Each indicator opens one panel: a card anchored under its icon and kept
inside the output. A panel closes on a click outside it, on Escape and on a
second click of its icon; only one panel is open at a time. While a panel is
open it holds the keyboard, so a text field in it receives typing without
anything else being clicked.

A panel shows its plugin's state, not its indicator's: one opened by an
action or by another plugin shows what a click on its icon shows.

| Panel | Offers |
|---|---|
| Network | the head: the connection's name (SSID, "Ethernet", "Wi-Fi off"), state and band in use, share-as-QR where `qrencode` exists (the PSK fetched through NetworkManager's secrets, the `WIFI:` payload with `\ ; , : "` escaped in the name and the password, and handed to `qrencode` on its stdin so the password is on no command line), a speed test where `speedtest`/`speedtest-cli` exists — a button whose tool is missing is hidden — and the Wi-Fi switch. Live figures while the panel is open, two columns: ping (to the gateway), packet loss, receiving, sending, downloaded, uploaded, IP address and gateway (each address copies itself via `wl-copy`). The Wi-Fi band in use with an Automatic switch; off, the lock pills for the bands the connection's network offers (2.4, 5, 6 GHz), applied to the connection through NetworkManager. The DNS provider — DHCP, Cloudflare, Google or custom servers typed in — applied to the active connection. The networks: known ones first, the connected one on top (a click on its row disconnects), then what the scan finds (the Wi-Fi device scans while the panel is open, never in the background); each row shows signal and a lock where credentials are needed, known ones not in use carry Forget, which removes the network's own saved connection through NetworkManager, whatever that connection is named. A row click joins: an open or known network connects at once, a secured unknown one asks for a password inline — an enterprise one for an identity and passphrase too, fed to NetworkManager over stdin — and nothing is tried without credentials; Enter in the field submits |
| Bluetooth | the power switch; a scan switch; paired devices first, then found ones, each with its state. A device row's click is its natural action: connect or disconnect a paired device, pair a found one |
| Audio | a volume slider for the default sink; mute; the output device to make default |
| Brightness | a slider for the backlight, which never steps below the minimum |
| Power | battery level, state and time left; the power profile where the system offers one (balanced always, power saver with it, performance where the platform has it); suspend, restart, power off, log out |
| Weather | the forecast for the coming days, from the same reading the widget shows |
| Clock | a month calendar with today marked; ‹ and › step the month |
| Agents | one tab per AI provider family: the limits windows (meter, share, the moment one resets) for Claude and GPT, and the tokens by day (a bar scaled to the busiest day of the seven, today bold and bright) and by model for every family. A sign-in fault brings the card with the CLI command that restores authoritative usage |

Out of scope: workspace numbers (asked without), a keyboard-layout
switcher (there is no compositor-side layout-switch surface to drive),
state indicators or update status (another shell's own state and tooling,
no standard source to read), other tools' prepaid balances, cross-device
usage sync and a settings UI for the panel.

### The agents panel's family rule

Every provider tab is a family, decided one message at a time: the source
implies it (Claude Code transcripts are the Claude subscription; Codex
session files are GPT), and otherwise the provider label does — exactly
`anthropic` is the Claude subscription (the same rule Omarchy's own
counter applies, so the two agree), an `openai*` label is GPT, and any
other label roots its own family by its first segment: `fireworks-300k`
is Fireworks, `acme-gateway` is Acme, and an `anthropic-*`
gateway is a Claude (gateway) tab beside the Claude Code tab — the same
mark, named for what it shows, separate from the subscription figures
Omarchy's own counter equals. A message with no label of its own is
placed by its model id (`claude-*`, `gpt*`/`codex-*`, `gemini-*`), and anything
unplaceable is Other. The same model behind two labels never merges two
families, but one model id written two ways (`openai/gpt-5.5`,
`fireworks:…/glm-5p3-fast`) is one row.

The figures come from `wayland-canvas-agent-usage`, a helper the widget
runs by name on the manifest's refresh interval (`refreshIntervalSec`,
which a user's `shell.json` overrides) and on every panel open:
it reads the tools' own records (Claude Code transcripts, Codex session
files, pi/omp session logs, the opencode database) under an incremental
stat-keyed cache, and probes the two limits services only through the
CLIs' own saved sign-ins — Anthropic's usage endpoint through curl, the
Codex app-server's JSON-RPC — reusing a fresh answer for a short window
so an opened-and-shut panel is never a probe per flick.

The panel keeps the provider it shows by its id, never by its position:
the collector orders the families after Claude and GPT by the week's
tokens, so a refresh can move a family to another tab, and the selection
moves with it. A provider that is gone puts the panel back on the first.

## Autostart

`Server::start()` launches the shell when `qs` is on `PATH` (constants in
`src/core/config.h`). The shell's config is found beside the compositor
binary — `../shell` in the build tree, `../share/wayland-canvas/shell` when
installed (meson installs `shell/` and the built-in `plugins/` under the
data dir, the plugins beside the shell where the host finds them) — or at
`$WAYLAND_CANVAS_SHELL_DIR` when set. It is started with `QML_IMPORT_PATH`
prefixed by the shell's `ui/` dir, so every plugin, wherever the host found
it, can `import WaylandCanvas.Ui`, and with the compositor's own directory
first on `PATH`, so the shell runs the helpers of the build it belongs to
(`wayland-canvas-agent-usage`, `wayland-canvas`) — in the build tree too.
`-B` (and `Options::start_bar`) starts the compositor without it.

## Cross-links

- [../platform/ipc.md](../platform/ipc.md) — the socket the logo button
  and the power panel talk to.
- [../apps/launcher.md](../apps/launcher.md) — the launcher the logo button opens.
- [../platform/outputs.md](../platform/outputs.md) — layer-shell usable
  area: the bar's exclusive zone is why maximized windows start below it.
