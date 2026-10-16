# Scenario tests

Each scenario runs the **real compositor in-process** on the wlroots headless
backend (`Server::init`/`Server::start`), spawns real `foot` clients, drives
input, and takes `grim` screenshots. Every check prints a PASS/FAIL line; the
scenario exits non-zero if any check failed, or if the compositor stopped
before the run reached its last step (a lost backend, its own shutdown).
Scenarios run serially (`is_parallel: false`, 180 s timeout) because they each
own the machine's Wayland display namespace and GPU.

## What each scenario proves

- **`gestures.cpp`** — touchpad gesture semantics through the `Gestures`
  entry points (the same ones the libinput listeners call): mapping three
  foot windows, title-bar button hit-testing, maximize, a 3-finger swipe in
  the normal view toward no desktop staying put with a hint (one short of
  the hop threshold moving nothing), pinning the free area beside it as a
  desktop, moving a window between desktops, 4-finger overview open/exit, 3-finger hops
  (including onto a free spot), 3-finger spread tiling (on: the windows
  take the board's halves; off: they keep the board's positions and
  are told they are not; back on: a pinch-in combine then joins the
  incoming window by halving the left half),
  overview desktop create/abandon, minimize/restore via the shelf, landing on
  empty canvas, and the keyboard desktop-direction jump. Then a desktop's
  scale: the Super + 2-finger pinch following the fingers and settling on 5 %
  steps, clicks and typing through the camera at 80 %, settles that never
  re-place a boarded window or tell a client a scale, twelve settles
  between 50 % and 200 % that change no floating, maximized or tiled
  window, the frame back where it began at 100 %, Super+0 and `0` in
  the overview returning to 100 %, a board laid out at 50 % whose
  membership holds at 200 % although the frame's edges have passed its
  windows' centres, and a window opened at 200 % joining the board
  there by halving the right half, every other window keeping its box. Then,
  views at any zoom: a desktop down at 25 %, where a 2-finger scroll, a
  plain 2-finger pinch and a 3-finger swipe toward no desktop leave the
  zoom alone and the view interactive, the overview opens further out and
  returns to the free view at its zoom, and Super+0 brings a free view to
  100 %. Last, normal-view hops along a row of three desktops (the middle
  one at 80 %): two quick swipes walking two desktops, a swipe past the
  last one staying on it with a hint, a long swipe hopping only once, onto
  the 80 % desktop at its scale, a 3-finger pinch with travel hopping like
  a swipe, and from a free view below the row a swipe toward nothing
  staying and one toward the row landing on it.
- **`input.cpp`** — device-level input through fake `wlr_keyboard`/`wlr_pointer`
  devices announced on `backend->events.new_input` (the same path libinput
  takes): title-bar drags, double-click maximize, restore/minimize/close
  buttons (incl. hover highlighting), anchored corner resize, Super+Return
  terminal launch, Super+Tab overview + Escape, desktop pin/jump chords,
  swipe events delivered by a device (a 3-finger hop onto the desktop on the
  right, a 4-finger overview), right-click desktop creation, tab-drag
  combining, frame-vs-window drag, and clicking empty canvas to land on a
  free area — then the per-desktop tiling switch end to end: the tab's Tile
  button and `t` toggling it, the two standing windows taking the board's
  halves, Super+arrows stepping a window's slice through the halves and
  quarters by both key orders, a covered window pushed into the free space,
  a join with no free slice left halving a held one, a close freeing its
  slice for the next taker, a drag onto another slice swapping them,
  maximize and minimize keeping the slice reserved or remembered with no
  re-layout, restore retaking it, a hop away and Super+Ctrl+Shift+arrow
  moving the window out to a floating view, Super+Up maximizing there,
  Super+T pinning that view as a desktop the standing window joins, travel
  back to the first desktop with every slice kept, and combining a tiled
  desktop into an untiled one keeping positions and telling the windows.
- **`launcher.cpp`** — the app launcher: logo-key tap opens, panel renders,
  search filters, Enter and result-click launch foot and close the launcher,
  logo-as-modifier does not open it, Escape and outside-click close it,
  no-match queries, and a custom desktop entry installed into
  `$XDG_DATA_HOME/applications` before `Server::init`: found by typing its
  name, launched with Enter, and verified by the marker file its spec-quoted
  `Exec` writes plus the foot window it opens. The marker holds the launched
  command's ignored signals and its `XDG_CURRENT_DESKTOP`: SIGPIPE must be as
  the scenario process itself was started (the compositor's own ignore never
  reaches what it launches), and the desktop name `wayland-canvas:wlroots`.

- **`sharing.cpp`** — the capture core, its protocols and the portal. The
  offscreen captures first: two foot windows mapped (they start overlapped
  at the centre), pulled apart with a title-bar drag, then the offscreen
  whole-output capture and a region capture compared pixel for pixel
  against grim's screenshot and `grim -g` crop of the same areas, and a
  window capture checked to be exactly the frame at the output's scale.
  "Over" is then dropped back over most of "under": the window shot of
  "under" must be pixel-identical to the one taken while nothing covered
  it (what overlaps a window leaves no trace), while the whole-output
  capture changes inside the overlap and nowhere outside the moved
  window's old or new frame. Then the capture protocols: plain `grim` now
  travels ext-image-copy-capture (it prefers the ext managers when they are
  offered) and its whole-output screenshot is compared pixel-exact against
  the offscreen capture, and `grim -T <ext-foreign-toplevel identifier>`
  captures "under" by name, again pixel-exact against the offscreen window
  capture taken while "over" still covers most of it. Last, the portal:
  a private `dbus-run-session` with scratch XDG dirs and its own PipeWire
  (never the machine's bus, portal or config — a machine without the
  portal pieces skips the phase) runs a Python+Gio ScreenCast client that
  must receive a PipeWire stream for a monitor and for a window; until the
  compositor's own `capture.pick` picker lands, a stand-in script answers
  xdpw's chooser with the first offered monitor (or window).

- **`bar.cpp`** — the top bar's panels, on a private world: before the
  compositor starts, the scenario brings up its own `dbus-daemon` system and
  session buses on sockets under `tmp/scenarios/bar/bus/` (a bar process must
  never touch the machine's buses) and launches
  `tests/scenarios/bar-standins.py` on them, which provides deterministic
  stand-ins — NetworkManager with one Wi-Fi device and five networks
  (`HomeNet` connected and known, `OldCafe` known and forgotten later,
  `CafeGuest` secured-unknown, `CorpNet` enterprise (802.1X), `LibraryFree`
  open),
  BlueZ with a powered adapter plus one paired and one unpaired device,
  UPower with a discharging 55 % battery, power-profiles-daemon, and a
  status-notifier tray item with a five-item menu. The weather fetch stays
  in the private world too: a curl stand-in sits first on the PATH the bar
  inherits, answers wttr.in at once from a canned response captured once
  (`tests/scenarios/bar-standins/wttr-j1.json`, three forecast days) and
  passes anything else to the real curl. The network panel's CLI tools get
  the same treatment: `nmcli`, `ping`, `qrencode` and `speedtest` shims
  sit first on that PATH, answering the panel's reads from a fixture
  world (the connected `HomeNet` at `192.168.8.42`, gateway `192.168.8.1`,
  PSK `correcthorse`, the tool's JSON) and recording the panel's writes
  in a state file so its reads follow them; `wl-copy` passes through to
  the real one under the scenario's display. Every invocation of either
  stand-in is logged under the run's scratch (`curl-standin.log`,
  `tool-standin.log`) so a run can show it never
  reached the network or the machine's own NetworkManager. The scenario proves the
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
  the scenario follows that rule). The weather widget and panel are
  therefore always checked: the widget shows beside the clock, and the
  panel lists the fixture's forecast days.

  The bar's other half is the plugin host it now runs on, proven in the
  same private world on scratch XDG dirs: a broken manifest sits in the
  scratch data dir from the start (its skip shows as every bar run being
  accounted for — logo, clock and weather, one tray item, this machine's
  indicators, nothing more), a user plugin dropped in while the shell runs
  appears as the bar's second run without anything of the shell being
  touched, editing the loaded plugin's QML turns its widget into a
  labelled one and pushes the bar's left edge further right, and
  disabling it in the scratch `shell.json` removes it again — every one
  through the shell's own reload, with the skip reasons visible as
  `[plugins]` lines in the test log. Last, an idle watch: the shell is
  sampled about every 10 ms for 10 s and must have spawned no process at
  all, because the reloads ride on change notifications, not on a poll.
  Then the theme: a foot window is mapped for its title bar and the
  overview is opened for its tabs, and both are sampled against the
  palettes in `src/core/config.h`; the theme switch (the right-most bar
  widget) is clicked and the scratch `shell.json` must carry
  `theme.mode: light` while the compositor reports light, the bar and an
  open panel are sampled light against the UI kit's palette
  (`shell_bg_light`/`shell_panel_light`, kept in sync with `UiTheme.qml`),
  the title bar and the target tab's accent fill likewise; rewriting the
  config as an edit reloads the shell and the bar that comes back is still
  light (a new bar surface proves the reload happened), and clicking the
  switch again restores dark in the config, the compositor, the bar and a
  panel. Screenshots of both modes — bar, panel, window title bar,
  overview — land in `tmp/scenarios/bar/` as `light-*`/`dark-*`.

  The overview then takes that bar's world: Super+Tab opens it, and the
  shell surfaces keep their input over it — a click on the bar's own
  background stays in the overview, the network indicator opens its
  panel inside it, a click outside the card closes the panel without
  leaving, and the indicator's next click reopens it. The panel holds
  the keyboard: typing reaches the Wi-Fi password field (seen as its
  password dots), and the panel takes the first Escape while the
  overview hears the second and closes.

  The same private world then proves the overview toolbar. A scratch
  user plugin sits in the scratch data dir from the start with one
  action button, one panel button and one menu button (its menu holds a
  glyph row and a colour-swatch row), and its one action appends its args
  to its storage — the read-back is the proof any click ran what it
  should. The toolbar must be unmapped out of the overview; entering it
  (Super+Tab) maps the toolbar as a `wayland-canvas-toolbar` layer
  surface of the shell client, its buttons read off the screenshot (the
  strip's opaque column holds nothing but its own glyphs) and its
  centring checked. The action button runs its action with its args and
  the toolbar stays; the pointer's resting place shows the button's
  label beside the strip on hover. The panel button opens the scratch
  panel through the common trigger, left of the strip and level with the
  button, and Escape — the panel holds the keyboard — closes it. The
  menu button expands the flyout beside the toolbar; choosing an entry
  runs its action with its
  args and folds the flyout. Escape then leaves the overview — the
  toolbar never takes the keyboard — and the slide out unmaps it with
  no panel or flyout outliving it. Screenshots of the toolbar, the
  flyout and the panel beside it are inspected on the way.

  Then adoption and the applications' preference. The machine's real
  color-scheme is read first through a child with the run's isolation
  undone (nothing else may see that environment), then an outside client
  sends `theme.set` light on the control socket — the compositor's own
  event is the only thing that could tell the shell — and the shell must
  adopt it: the bar's palette flips light, the scratch `shell.json`
  carries the adopted mode, and an edit that reloads the shell keeps it.
  The switch still works on the adopted mode, restoring dark. The run
  never writes the preference itself: it is the real session's to set, so
  the headless compositor must leave even the scratch keyfile backend the
  run points at (no `glib-2.0/settings/keyfile` under the scratch
  config), and the machine's color-scheme must read exactly as it started
  (today `prefer-dark`). The write itself is proven by the unit test
  `theme-prefs` against that same private backend.

  Last, the `canvas.notes` plugin end to end, driven
  only through the public socket: `msg action.run canvas.notes/create`
  puts a `canvas.notes/...` item in the state, and the plugin's editor
  must map as a layer surface of the shell client (`namespace
  wayland-canvas-note-editor`) holding the keyboard on its own surface as
  it opens — its card checked, pixel by pixel on the screenshot, to sit
  exactly at the activated box in the note's colour with the darker strip
  on top. Typing then Escape keeps the text in the state and in the
  plugin's storage (scratch state); a fake-pointer drag's new place
  reaches the storage through `item.moved`; a config-rewrite reload
  leaves exactly the one note in the state (nothing doubled), and once
  `msg item.remove` took it off the canvas, another reload must put it
  back from storage — the restart proof. In the overview, the settled
  camera's own math gives the note's on-screen box; a double-click there
  opens the editor and typed keys reach it; back in the normal view, a
  colour dot on the strip recolours the note (state and storage), a click
  outside closes the editor, the strip's × deletes the note from canvas
  and storage, and a second note closed with no text is removed too.

  Last, the `canvas.agents` plugin end to end. The bar's own `HOME` is a
  scratch one (read back from `/proc/<pid>/environ`), holding the fixture
  AI world: Claude Code transcripts with the reference screenshot's own
  day figures (811.5K today, 324.6M yesterday, 246.1M the day before,
  deduplicated), Codex rollout files and a fake `codex` app-server on the
  scratch PATH (the real CLI never runs: the fake answers the RPC
  handshake and account reads, and refuses the rate-limit read so GPT is
  signed out), a Fireworks-style JSONL, and credentials with the
  `max_20x` tier. The usage probe is pointed at the run's own loopback
  stand-in (`tests/scenarios/bar-standins/agent-usage.py`), which answers
  37 %/68.4 % (later 95 %/72 %) and records the token and beta header it
  received; the curl stand-in proves no request ever named the real
  Anthropic endpoint. The run proves the collector's own JSON (four
  families in order, titles and chips, limits shares, day and model
  figures, the signed-out texts), the widget appearing left of Bluetooth
  and hiding again when the world empties, the panel's drawn Claude mark,
  meters at their probed shares (the 95 % window in the attention
  colour), the provider-switch chips (a signed-out GPT tab with its
  sign-in card and no meters, a tokens-only Fireworks tab with the star
  mark), the signed-out Claude tab after its credentials are removed
  (card and attention meta, figures kept), the reopen probing again past
  the probe-reuse window, and both palettes — all on screenshots under
  `tmp/scenarios/bar/agents*.png`.

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
