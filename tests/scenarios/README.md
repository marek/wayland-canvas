# Scenario tests

Each scenario runs the **real compositor in-process** on the wlroots headless
backend (`server_init`/`server_start`), spawns real `foot` clients, drives
input, and takes `grim` screenshots. Every check prints a PASS/FAIL line; the
scenario exits non-zero if any check failed. Scenarios run serially
(`is_parallel: false`, 180 s timeout) because they each own the machine's
Wayland display namespace and GPU.

## What each scenario proves

- **`gestures.c`** — touchpad gesture semantics through the `gesture_*()`
  entry points (the same ones the libinput listeners call): mapping three
  foot windows, title-bar button hit-testing, maximize, 3-finger panning with
  momentum, pinning a free area as a desktop, moving a window between
  desktops, 4-finger overview open/exit, 3-finger hops (including onto a free
  spot), pinch-out tiling, pinch-in combining, overview desktop
  create/abandon, minimize/restore via the shelf, landing on empty canvas,
  and the keyboard desktop-direction jump.
- **`input.c`** — device-level input through fake `wlr_keyboard`/`wlr_pointer`
  devices announced on `backend->events.new_input` (the same path libinput
  takes): title-bar drags, double-click maximize, restore/minimize/close
  buttons (incl. hover highlighting), anchored corner resize, Super+Return
  terminal launch, Super+Tab overview + Escape, desktop pin/jump chords,
  swipe events delivered by a device, right-click desktop creation, tab-drag
  combining, frame-vs-window drag, the tab Tile button, and clicking empty
  canvas to land on a free area.
- **`launcher.c`** — the app launcher: logo-key tap opens, panel renders,
  search filters, Enter and result-click launch foot and close the launcher,
  logo-as-modifier does not open it, Escape and outside-click close it,
  no-match queries, and a custom desktop entry installed into
  `$XDG_DATA_HOME/applications` before `server_init`: found by typing its
  name, launched with Enter, and verified by the marker file its spec-quoted
  `Exec` touches plus the foot window it opens.

## What they need

- `foot` and `grim` on `$PATH` at configure time (the tests are only
  registered when both are found).
- A GPU render node at run time (the headless backend still renders with
  GLES2); CI containers without `/dev/dri` cannot run them.
- Nothing else: they write only under `$CANVAS_TMP` (default: the repo's
  git-ignored `tmp/`, passed by meson) — screenshots and scratch state go to
  `tmp/scenarios/<scenario>/`.

Run with `meson test -C build --suite scenarios`, or `tools/check.sh` which
runs the `unit` suite first and then this one.
