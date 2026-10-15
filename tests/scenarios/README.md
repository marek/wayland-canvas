# Scenario tests

Each scenario runs the **real compositor in-process** on the wlroots headless
backend (`Server::init`/`Server::start`), spawns real `foot` clients, drives
input, and takes `grim` screenshots. Every check prints a PASS/FAIL line; the
scenario exits non-zero if any check failed. Scenarios run serially
(`is_parallel: false`, 180 s timeout) because they each own the machine's
Wayland display namespace and GPU.

## What each scenario proves

- **`gestures.cpp`** — touchpad gesture semantics through the `Gestures`
  entry points (the same ones the libinput listeners call): mapping three
  foot windows, title-bar button hit-testing, maximize, 3-finger panning with
  momentum, pinning a free area as a desktop, moving a window between
  desktops, 4-finger overview open/exit, 3-finger hops (including onto a free
  spot), 3-finger spread tiling (on: the windows grid and are told they are
  tiled; off: they keep the grid's positions and are told they are not; back
  on: a pinch-in combine then joins the incoming window into the grid),
  overview desktop create/abandon, minimize/restore via the shelf, landing on
  empty canvas, and the keyboard desktop-direction jump.
- **`input.cpp`** — device-level input through fake `wlr_keyboard`/`wlr_pointer`
  devices announced on `backend->events.new_input` (the same path libinput
  takes): title-bar drags, double-click maximize, restore/minimize/close
  buttons (incl. hover highlighting), anchored corner resize, Super+Return
  terminal launch, Super+Tab overview + Escape, desktop pin/jump chords,
  swipe events delivered by a device, right-click desktop creation, tab-drag
  combining, frame-vs-window drag, and clicking empty canvas to land on a
  free area — then the per-desktop tiling switch end to end: the tab's Tile
  button and `t` toggling it, windows opened while it is on joining the grid
  and closing or minimizing re-flowing it, maximize and fullscreen taking a
  window out and restore putting it back, a drag onto another slot reordering
  the grid, Super+T pinning a free view with tiling on, and abandoning or
  combining a tiled desktop keeping positions and telling the windows.
- **`launcher.cpp`** — the app launcher: logo-key tap opens, panel renders,
  search filters, Enter and result-click launch foot and close the launcher,
  logo-as-modifier does not open it, Escape and outside-click close it,
  no-match queries, and a custom desktop entry installed into
  `$XDG_DATA_HOME/applications` before `Server::init`: found by typing its
  name, launched with Enter, and verified by the marker file its spec-quoted
  `Exec` touches plus the foot window it opens.

- **`bar.cpp`** — the top bar's panels, on a private world: before the
  compositor starts, the scenario brings up its own `dbus-daemon` system and
  session buses on sockets under `tmp/scenarios/bar/bus/` (a bar process must
  never touch the machine's buses) and launches
  `tests/scenarios/bar-standins.py` on them, which provides deterministic
  stand-ins — NetworkManager with one Wi-Fi device and three networks
  (`HomeNet` connected, `CafeGuest` secured-unknown, `LibraryFree` open),
  BlueZ with a powered adapter plus one paired and one unpaired device,
  UPower with a discharging 55 % battery, power-profiles-daemon, and a
  status-notifier tray item with a five-item menu. The scenario proves the
  bar's own process runs with exactly those bus addresses (read from
  `/proc/<pid>/environ`). Then, with the real bar client (qs) started by the
  compositor on those buses: every indicator is clicked with the fake pointer
  and its panel must map as a layer surface of the bar client (panels are
  told apart by their layer namespace, `wayland-canvas-panel-<name>`), hold
  the keyboard on its own surface while open, and close
  again — by an outside click, by Escape, or by a second click of the icon.
  The typing check selects `CafeGuest`, focuses its password field, types
  into it, and verifies password dots on the screenshot. The panels'
  actions are then exercised against the stand-ins and proven by what the
  mocks recorded (their `org.freedesktop.DBus.Mock` call log, read with
  `busctl --address=` against the private bus): Connect sends CafeGuest's
  activation with the typed password, selecting the connected `HomeNet` and
  clicking its action row deactivates the stand-in's Wi-Fi device, the
  paired Bluetooth device's row click calls Connect (Disconnect on the
  second click), the found device's row click calls Pair, and clicking
  "Power saver" makes the stand-in's active profile `power-saver`. No
  Enter is ever sent from the password field, and the Session rows
  (Suspend, Restart, Power off, Log out) are never clicked. Indicator
  positions are read off the bar screenshot itself; brightness stays the
  one machine-read widget (the bar hides it without a sysfs backlight, and
  the scenario follows that rule).
  Weather is not required: its widget hides until a first successful fetch,
  and the scenario skips it with a note when it did not load.

## What they need

- `foot` and `grim` on `$PATH` at configure time (the tests are only
  registered when both are found; `bar` also needs `qs`).
- `bar` also needs `dbus-daemon` and Python with `dbus`/`gi` bindings: at
  configure time `tests/scenarios/ensure-bar-standins.sh` provisions a
  python-dbusmock venv under the git-ignored `tmp/bar-standins/` (or finds
  one already there); without them the bar scenario is skipped, not failed.
  Its panel-action proofs additionally need `busctl` on `$PATH` to read
  what the stand-ins recorded.
- A GPU render node at run time (the headless backend still renders with
  GLES2); CI containers without `/dev/dri` cannot run them.
- Nothing else: they write only under `$CANVAS_TMP` (default: the repo's
  git-ignored `tmp/`, passed by meson) — screenshots and scratch state go to
  `tmp/scenarios/<scenario>/`.

Run with `meson test -C build --suite scenarios`, or `tools/check.sh` which
runs the `unit` suite first and then this one.
