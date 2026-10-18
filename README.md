# wayland-canvas

An infinite-canvas Wayland compositor (wlroots 0.20). Windows live anywhere on an
unbounded plane; **virtual desktops are screen-sized dashed frames** you drag over
the windows however you like. You can work on a desktop or anywhere in between.
Windows get Windows-11-style title bars with minimize, maximize/restore and close.

## Install

Binary packages for Arch and Arch-based distributions (`.pkg.tar.zst`, built
with makepkg) and for Debian/Ubuntu (`.deb`, built inside a rootless Debian
root filesystem):

```sh
tools/package.sh              # both formats, artifacts in dist/ (--arch, --deb
                              # build just one; ninja -C build packages too)
tools/install.sh              # builds the package for your distro, installs it
                              # with the system package manager, and asks
                              # whether to make wayland-canvas the default
tools/install.sh --uninstall  # restores the previous default, then removes it
```

`wayland-canvas-session status | set-default | restore` shows or changes the
default session later; `restore` puts every display-manager file back byte
for byte. SDDM (autologin and the remembered last session), GDM
(AccountsService), LightDM and greetd are covered; on anything else the tool
changes nothing and says how to start the compositor. Removing the package
directly (`pacman -R`, `apt remove`) restores the default as well.

Pushing a `v<version>` tag builds both packages on GitHub Actions — the same
`tools/package.sh` run inside an `archlinux` and a `debian:sid` container —
and drafts a release from them.

## Build

Needs clang (LLVM) — the compositor is C++26 and the wlroots subproject builds
with it too — meson ≥ 1.3, ninja, and the wlroots 0.20 dependencies
(wayland, wayland-protocols, libdrm, gbm/EGL/GLES2, libinput, libseat, xkbcommon,
pixman, libdisplay-info, hwdata) plus cairo, pango, librsvg, glib, json-glib and
sqlite3.

```sh
tools/build.sh             # configures with toolchain/llvm.ini, then compiles
tools/check.sh             # builds, then the unit and headless scenario suites
```

By hand that is `meson setup build --native-file toolchain/llvm.ini` and
`ninja -C build`.

`meson.build` stops with that same setup command if a build dir is configured
with another compiler.

If `wlroots-0.20` isn't installed system-wide, meson fetches 0.20.2 into
`subprojects/` and links it statically (no libliftoff/Xwayland needed).

## Run

- From a TTY: `./build/wayland-canvas` (DRM/KMS via libseat/logind).
- Nested in another compositor: run the same command from a terminal there. The
  host may swallow 3/4-finger gestures if it binds them itself.
- Options: `-s <cmd>` startup command, `-S <scale>` output scale (or
  `$WAYLAND_CANVAS_SCALE`; default is 2 on dense panels such as 2880×1800, else 1),
  `-d` debug logging, `-B` start without the shell (bar, toolbar, notes).
- Control a running compositor: `wayland-canvas msg <method> [<params as JSON>]`
  over `$WAYLAND_CANVAS_SOCKET` (set for every client it spawns): `state`,
  `subscribe`, `launcher.toggle`, `overview.toggle`, `theme.set`,
  `surface.place`, `surface.unplace`, `surface.move`, `surface.resize`,
  `window.focus`, `action.run`, `capture.pick`, `capture.stop`,
  `settings.get`, `settings.set`, `shortcut.capture`, `shortcut.cancel`,
  `quit`. `wayland-canvas -h`
  lists them; [docs/intents/platform/ipc.md](docs/intents/platform/ipc.md) is
  the protocol.

The terminal is `$TERMINAL` (falls back to `foot`).

### Screen sharing (Chrome, video calls)

Apps share a screen through xdg-desktop-portal and PipeWire; install the wlroots
backend once:

```sh
sudo pacman -S xdg-desktop-portal-wlr   # Debian: apt install xdg-desktop-portal-wlr
```

wayland-canvas names itself `wayland-canvas:wlroots` (`XDG_CURRENT_DESKTOP`) and
hands its display to the portal at start. Sharing works from a wayland-canvas
login (a TTY, or the session at the login screen): run `./build/wayland-canvas`
as usual — the hand-over happens on your normal session bus, so the keyring
(saved passwords and logins), notifications and the rest keep working. When it
exits, the portal goes back to the session it was taken from (Hyprland on
another VT shares its own screens again).

Apps pick what to share with wayland-canvas's own picker (a screen, or a window
by clicking it). A package installs the portal backend's config for that; a run
from the build tree needs it once, with the build's binary as the chooser:

```sh
mkdir -p ~/.config/xdg-desktop-portal-wlr
sed "s|= wayland-canvas msg|= $PWD/build/wayland-canvas msg|" \
    packaging/xdg-desktop-portal-wlr.conf > ~/.config/xdg-desktop-portal-wlr/wayland-canvas
```

The portal is one per login. While wayland-canvas runs nested inside another
compositor, apps share through *that* compositor's portal: pick the
wayland-canvas window in its picker. Don't start wayland-canvas under
`dbus-run-session` to get around this: it gets a portal of its own, but the apps
inside lose the keyring and every other session service.

### Screenshots and recordings

macOS-style, from the keyboard:

| Keys | Action |
|---|---|
| Super+Shift+3 | Screenshot of the whole screen |
| Super+Shift+4 (or Print) | Pick what to shot: drag a region, click a window, Space for windows only, Esc to cancel |
| Super+Shift+5 | The capture toolbar: shots and recordings of screen, window or region, with options (destination, timer, pointer) |
| Ctrl + any shot key | Copy to the clipboard instead of saving |
| Shift+Print | Screenshot of the whole screen |
| Alt+Print | Stop a running recording, else open the toolbar on its recordings |

A saved shot lands in `~/Pictures/Screenshots` (the XDG pictures dir), is copied
to the clipboard, and shows as a bottom-right thumbnail for a few seconds —
click it to open the file, swipe it away. A window is captured alone: title bar
and content, nothing over it, at the output's scale whatever the canvas zoom.
Recordings go to `~/Videos` (the XDG videos dir); a red dot in the bar marks a
running one and stops it on a click. The compositor itself renders every
recording frame offscreen (the same path a shot takes) and pipes them to
`ffmpeg`, so recordings work nested and from a TTY alike, and none of the
capture UI (picker, toolbar, thumbnail) is ever in them. Recordings need
ffmpeg (optional — shots never depend on it):

```sh
sudo pacman -S ffmpeg   # Debian: apt install ffmpeg
```

## Opening apps

Tap the Windows (logo) key, or press Super+Space: a Start-style launcher lists
every installed app (XDG `.desktop` entries from `~/.local/share/applications`
and `/usr/share/applications`, with icons from your icon theme). Type to filter,
↑/↓ or the mouse to pick, Enter or click to launch, Esc or a click outside to
close. New windows open in the middle of the screen you're looking at (or of
the overview's target). An app launched from the overview opens in the
overview's target even if you leave the overview before its window appears;
the window then waits there without taking the keyboard, and a hint says where.

To add your own entry, use the standard XDG desktop-entry file that every Linux
launcher reads: save e.g. `~/.local/share/applications/my-tool.desktop`

```ini
[Desktop Entry]
Type=Application
Name=My Tool
GenericName=What it does
Exec=/path/to/command --some-flag
Icon=utilities-terminal
Terminal=false
Keywords=tool;helper;
```

It shows up the next time the launcher opens (entries are re-read on every open).
`Icon` is a theme icon name or an absolute image path; `Terminal=true` runs the
command inside `$TERMINAL`. `Exec` only allows double quotes, so put anything
shell-heavy in a script, and check files with `desktop-file-validate`. To hide an
app, put a copy of its `.desktop` file here with `NoDisplay=true`; yours wins over
the system one.

## The shell and its plugins

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
- **Settings**: the toolbar's gear opens the settings window — every
  keyboard shortcut (set by pressing it), the swipe presets, the toolbar's
  edge and which bar widgets show.

A plugin is a folder with a `plugin.json` manifest. Plugins are found built in,
then in `$XDG_DATA_HOME/wayland-canvas/plugins`, then in the config's
`pluginPaths`; a later one replaces an earlier one with the same id. The config
is `$XDG_CONFIG_HOME/wayland-canvas/shell.json`: `plugins.<id>.enabled: false`
turns a plugin off, `bar.layout` and `toolbar.order` arrange widgets and
buttons, and edits to it or to a plugin apply while the shell runs.
[docs/intents/shell/plugins.md](docs/intents/shell/plugins.md) has the
manifest, the API and the UI kit. The shell itself is found beside the
compositor binary (build tree or install prefix) or at
`$WAYLAND_CANVAS_SHELL_DIR`.

## Concepts

- **Canvas**: every output has its own camera onto one shared plane.
- **Desktop**: a screen-sized rectangle on the canvas. A window belongs to the
  desktop its centre is in. Moving a desktop frame never moves windows. Each
  desktop has its own zoom, 25 % to 200 %: it scales the picture only, so
  windows keep their size and layout (and look soft above 100 %); Super+0
  brings it back to 100 %.
- **Free area**: any spot that isn't a desktop; you can pan or jump there too.
  Panning or scrolling never changes the zoom: a free area keeps the zoom you
  panned onto it with (Super+0 returns it to 100 %).
- **Overview**: a zoomed-out, live view of the canvas where desktops are arranged.
  It has a *target* (accent outline): a desktop, or a free screen-sized spot.

## Touchpad gestures

| Where | Gesture | Action |
|---|---|---|
| Normal | 3-finger swipe | **Hop** to the closest desktop in that direction, at its zoom; none that way → you stay put (a hint says so). A swipe never leaves the desktops: reach a free area from the overview, or drag / 2-finger scroll on empty canvas, or Super+Alt+arrows |
| Normal | 4-finger swipe up | Zoom out into the overview (follows your fingers; release early to cancel) |
| Normal | Super + 2-finger pinch | **Zoom** the desktop shown, following your fingers; it settles on 5 % steps. Windows keep their size; Super+0 returns to 100 % |
| Overview | 3-finger swipe | Jump to the closest desktop in that direction; none that way → move one screen that way (free spot) |
| Overview | 4-finger swipe down | Zoom into the target |
| Overview | 4-finger swipe up | Zoom out further |
| Overview | 4-finger swipe left/right | Swipe the desktop under the pointer (or the target) away: **abandon** it; windows stay |
| Overview | 3-finger pinch out | **Tile** the windows of the desktop under the pointer (or the target) onto a one-shot board of slices |
| Overview | 3-finger pinch in | **Combine**: the nearest other desktop's windows move into this one (relative layout kept) |
| Overview | 2-finger pinch / scroll | Zoom / pan the overview |
| Overview | Super + 2-finger pinch | Zoom the desktop under the pointer (or the target); `0` returns the target to 100 % |

The finger counts are a setting: the `swipes` line of `[gestures]` in
`~/.config/wayland-canvas/canvas.conf` picks `three-finger-hops` (the table
above, the default), `four-finger-hops` (the same table with every 3 and 4
swapped: 4 fingers hop, 3 up open the overview, 3 down go in, 3 sideways
abandon, a 4-finger pinch tiles and combines) or `off` — no 3- or 4-finger
compositor gesture at all: in the normal view they are forwarded to the client
under the pointer like 2-finger ones, in the overview they do nothing.
`pinch-scale = off` in the same section makes a Super + 2-finger pinch a plain
pinch: the client's in the normal view, the overview's zoom in the overview.
The overview's help line names the gestures of the preset in force (keys only
under `off`).

## Mouse

| Where | Action | Result |
|---|---|---|
| Title bar | drag / double-click | Move (dragging a maximized window restores it) / maximize-restore |
| Title bar | `–` `☐` `✕` | Minimize, maximize/restore, close |
| Window edges/corners | drag | Resize |
| Anywhere | Super+left / Super+right drag | Move / resize |
| Empty canvas | drag / scroll | Pan |
| Overview | click desktop / window / empty canvas | Go there (empty canvas = free area) |
| Overview | drag a frame (tab or empty interior) | Reposition the desktop over the windows |
| Overview | drop a frame onto another | Combine them |
| Overview | right-click (two-finger tap or press) on empty canvas | New desktop at that spot |
| Overview | right-click a desktop's tab or its empty interior | Remove that desktop (its windows stay) |
| Overview | tab buttons ⊞ / ✕ | Tile / abandon |
| Overview | minimized shelf (bottom) | Restore that window |

## Keyboard (Super = logo key)

Every shortcut is the user's to change, in `canvas.conf` (below) or by
pressing the new keys in the settings window; the table is the defaults.
A lone tap of Super runs nothing by default — it is a key any action can
hold (`Super` in `canvas.conf`). VT switching (Ctrl+Alt+F1…F12) is fixed:
in no file, and no chord can take it, so the way out of a stuck session
stays.

| Default keys | Name in `canvas.conf` | Action |
|---|---|---|
| Super+Space | `launcher` | App launcher |
| Super+Return | `terminal` | Terminal |
| Super+Q, Alt+F4 | `close` | Close window |
| Super+M / Super+Shift+M | `minimize` / `restore` | Minimize / restore last minimized |
| Super+arrows | `snap-left` … `snap-down` | Step the focused window's slice on a tiled desktop (Super+Up maximizes on a floating one) |
| Super+F | `fullscreen` | Fullscreen |
| Super+Tab, Super+W | `overview` | Toggle overview |
| Super+Ctrl+arrows | `hop-left` … `hop-down` | Go to the desktop in that direction (none that way: stay) |
| Super+Ctrl+Shift+arrows | `move-left` … `move-down` | Move the focused window to that desktop and follow |
| Super+Alt+arrows | `pan-left` … `pan-down` | Pan a third of a screen |
| Super+Ctrl+D / Super+Ctrl+F4 | `pin-desktop` / `abandon-desktop` | Make the current view a desktop / abandon the current desktop |
| Super+T | `tiling` | Tile the current desktop (or view) |
| Super+1…9 | `desktop-1` … `desktop-9` | Go to desktop N |
| Super+0 | `zoom-reset` | Return the desktop shown, or a free area, to 100 % |
| Super+Shift+3 / 4 / 5 | `shot-screen` / `capture-pick` / `capture-toolbar` | Whole-screen shot / the capture picker / the capture toolbar (Ctrl on a shot: clipboard) |
| Print / Shift+Print / Alt+Print | `capture-pick` / `shot-screen` / `record-stop` | The capture picker / whole-screen shot / stop a recording, else the toolbar on its recordings |
| Super+L | `lock` | Lock the screen (the password is your login one, through PAM) |
| Super+N / Super+Shift+N | `notifications-dismiss` / `notifications-dismiss-all` | Take the newest notification popup, or every one, off the screen |
| Super+Ctrl+N | `night-light` | The night light on or off |
| Super+Shift+E, Ctrl+Alt+Backspace | `quit` | Quit |
| Overview: arrows, Return, Escape, Delete, T, M, N, 0, +/- | `[overview]`: `hop-left` … `leave` | Jump, enter, back, abandon, tile, combine, new desktop, target back to 100 %, zoom; Super+Tab / Super+W leave |

## Configuration — canvas.conf

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

The settings window (the gear in the overview toolbar) edits the same file,
so the two never disagree, and records a new shortcut by pressing it.
Sizes, colours and tuning stay compile-time in `src/core/config.h`
(dwl-style: edit and rebuild); the shell's own settings — which bar widgets
show, the toolbar's edge — are its `shell.json`.

## Settings

The gear button in the overview toolbar opens the settings window, a panel
with five tabs:

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

## Protocols

xdg-shell, xdg-decoration and KDE server-decoration (server-side by default;
clients that negotiate neither keep their own decorations), layer-shell (bars,
launchers, wallpapers, notifications), foreign-toplevel management (taskbars can
restore minimized windows), xdg-activation, pointer-gestures, relative-pointer,
cursor-shape, screencopy/export-dmabuf (grim, and screen sharing through
xdg-desktop-portal-wlr), ext-session-lock (the screen lock; swaylock and
hyprlock work too), wlr-gamma-control (wlsunset, gammastep), data-control,
primary selection,
viewporter, fractional-scale, presentation-time, single-pixel-buffer, idle-notify,
idle-inhibit, virtual-keyboard.

## The session

A login — picked at the display manager, or started from a TTY — brings up
what a graphical session needs:

| Part | What you get |
|---|---|
| Lock screen | Super+L, `loginctl lock-session`, suspend (the lock is drawn before the machine sleeps) and an idle timeout of 5 minutes, which the Session tab changes. The password is your own, checked through PAM against the service file the package installs. While locked, only the lock is on screen — in a screenshot and a shared screen too — and an idle inhibitor (a video player) holds the idle lock off |
| Notifications | Popups top-right with their actions, a bell in the bar with the history behind it, and a silence switch. Super+N dismisses the newest, Super+Shift+N all of them. Another notification daemon (mako, dunst) can serve the session instead by taking the bus name first |
| Password dialog | An app that asks for administrator rights (`pkexec`, a package manager) gets a dialog in the session; it holds the keyboard while it is open. A session that already has a polkit agent keeps it |
| Night light | A switch and a colour temperature, in the brightness panel and the Session tab, toggled with Super+Ctrl+N. It rides the output's gamma ramp, so a screenshot stays in true colours; wlsunset and gammastep work through the gamma-control protocol and take over while they run |
| X11 applications | Applications that speak only X11 run through Xwayland, which starts with the first of them. Their windows are windows like any other: title bar, desktop, overview, maximize, minimize, fullscreen, and a close button that asks them to close. Their menus and tooltips appear where the app puts them, and the clipboard works both ways with wayland apps |
| Session services | `wayland-canvas-session.target` is started once the compositor has handed its display to systemd, so `graphical-session.target` units and the XDG autostart entries come up with the session and stop with it |

A nested or headless run leaves all of this to the session that owns it: no
session services are started, no logind session is watched.

## Limitations

X11 windows are drawn at scale 1 on a hi-dpi output (right-sized, a little
soft); no output-configuration protocol (set the scale with `-S`), the night light needs an output with a
gamma ramp of its own (a nested or headless run shows none), full repaint of
an output per changed frame (no damage regions), no window open/close
animations.

## Working on this repo

Development rules and the design corpus live beside the code — see
[AGENTS.md](AGENTS.md): the triple helix of **intents** (what the compositor
must satisfy), **tests** and **implementation**, with plans, skills and roles
beside it.

| Folder | What |
|---|---|
| `src/` | Compositor source, one folder per area: `core/`, `canvas/`, `windows/`, `input/`, `render/`, `platform/`, `apps/` (`src/core/config.h` holds the tunables) |
| `shell/` | The Quickshell shell: the plugin host and the `WaylandCanvas.Ui` kit its plugins draw with |
| `plugins/` | The built-in plugins (`canvas.bar`, the indicators with their panels, the `canvas.theme` light/dark switch, …) the host loads beside the shell |
| `tests/` | Unit tests and headless scenario sources (`tests/scenarios/`) |
| `tools/` | `build.sh` (build), `check.sh` (build + test), `package.sh` (both packages), `install.sh`, `check-plans.sh`, hooks |
| `scripts/` | `agent-setup.sh` (links skills, roles and hooks into `.claude/`) |
| `protocols/` | Protocol XML used beyond wlroots' set |
| `docs/intents/` | Normative design specs (the intent corpus) |
| `docs/plans/` | Dated, tracked work plans |
| `docs/skills/` | How to work here |
| `docs/roles/` | Working personas (developer, tester, reviewer, planner) |
| `tmp/` | Git-ignored scratch space (`CANVAS_TMP` overrides) |
| `packaging/` | Package definitions: Arch `PKGBUILD` + install scriptlet, Debian control and maintainer scripts, the session file, the default-session tool, the source-tarball builder |
