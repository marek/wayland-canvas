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
  spot), pinch-out tiling, pinch-in combining, overview desktop
  create/abandon, minimize/restore via the shelf, landing on empty canvas,
  and the keyboard desktop-direction jump.
- **`input.cpp`** — device-level input through fake `wlr_keyboard`/`wlr_pointer`
  devices announced on `backend->events.new_input` (the same path libinput
  takes): title-bar drags, double-click maximize, restore/minimize/close
  buttons (incl. hover highlighting), anchored corner resize, Super+Return
  terminal launch, Super+Tab overview + Escape, desktop pin/jump chords,
  swipe events delivered by a device, right-click desktop creation, tab-drag
  combining, frame-vs-window drag, the tab Tile button, and clicking empty
  canvas to land on a free area.
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
  again — by an outside click, by Escape, or by a second click of the icon;
  opening one panel closes the last. The typing check selects `CafeGuest`,
  focuses its password field, types into it, and verifies password dots on
  the screenshot — the Connect row is never clicked and no Enter is ever
  sent, so nothing is ever submitted. Indicator positions are read off the
  bar screenshot itself; brightness stays the one machine-read widget (the
  bar hides it without a sysfs backlight, and the scenario follows that rule).
  Weather is not required: its widget hides until a first successful fetch,
  and the scenario skips it with a note when it did not load.

## What they need

- `foot` and `grim` on `$PATH` at configure time (the tests are only
  registered when both are found; `bar` also needs `qs`).
- `bar` also needs `dbus-daemon` and Python with `dbus`/`gi` bindings: at
  configure time `tests/scenarios/ensure-bar-standins.sh` provisions a
  python-dbusmock venv under the git-ignored `tmp/bar-standins/` (or finds
  one already there); without them the bar scenario is skipped, not failed.
- A GPU render node at run time (the headless backend still renders with
  GLES2); CI containers without `/dev/dri` cannot run them.
- Nothing else: they write only under `$CANVAS_TMP` (default: the repo's
  git-ignored `tmp/`, passed by meson) — screenshots and scratch state go to
  `tmp/scenarios/<scenario>/`.

Run with `meson test -C build --suite scenarios`, or `tools/check.sh` which
runs the `unit` suite first and then this one.
