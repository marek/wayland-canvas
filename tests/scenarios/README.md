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
  overview desktop create/abandon, minimize/restore via the shelf, a click
  of the fake pointer on the open overview's empty canvas landing on that
  spot, and Super+Ctrl+Right on the fake keyboard jumping to the desktop
  to the right. A gesture forwarded to a client is read from what
  `tests/clients/gesture-client.c` reports, a corner square on the overlay
  layer that binds `zwp_pointer_gestures_v1` and prints each swipe and pinch
  it receives while the fake pointer is over it. Then a desktop's
  scale: the Super + 2-finger pinch following the fingers and settling on 5 %
  steps, clicks and typing through the camera at 80 %, settles that never
  re-place a boarded window or tell a client a scale, twelve settles
  between 50 % and 200 % that change no floating, maximized or tiled
  window, the frame back where it began at 100 %, Super+0 and `0` in
  the overview returning to 100 %, a board laid out at 50 % whose
  membership holds at 200 % although the frame's edges have passed its
  windows' centres, and a window opened at 200 % joining the board
  there by halving the right half, every other window keeping its box. Then,
  views at any zoom: a desktop down at 25 %, where a 2-finger scroll over
  empty canvas moves nothing without Super and with Super held looks away
  even over a window, and a plain 2-finger pinch and a 3-finger swipe
  toward no desktop leave the zoom alone and the view interactive, the
  overview opens further out and returns to the free view at its zoom,
  and Super+0 brings a free view to 100 %. Last, normal-view hops along a
  row of three desktops (the middle one at 80 %): two quick swipes walking
  two desktops, a swipe past the
  last one staying on it with a hint, a long swipe hopping only once, onto
  the 80 % desktop at its scale, a 3-finger pinch with travel hopping like
  a swipe, and from a free view below the row a swipe toward nothing
  staying and one toward the row landing on it. Then launches from the
  overview: an app opening in the overview's target, a free spot; one that
  takes a second to appear, with the overview closed by Escape before it
  does, still opening in the spot it was launched for, unfocused, with a
  hint saying where (traced through the launched shell to its child); a
  window asking to start maximized, far from the canvas origin, filling the
  screen being looked at; and a late launch starting maximized filling the
  free spot it was launched for. The late launches are scratch desktop
  entries in a scratch `$XDG_DATA_HOME`.
  Last, the `[gestures]` options set in-process through
  `server->gestures.set_options`: the four-finger-hops preset (a 4-finger
  swipe hopping in both modes, a 3-finger swipe up opening the overview and
  one down going in, a 3-finger sideways swipe abandoning a desktop, a
  4-finger pinch previewing tile and combine), `off` (3- and 4-finger swipes
  forwarded to the client in the normal view with the camera and the overview
  state untouched; nothing at all in the overview, a pinch included),
  pinch-scale off (a Super + 2-finger pinch scaling no desktop: forwarded in
  the normal view, zooming the overview camera in the overview), the help
  line naming each preset's gestures by its text (keys only under `off`),
  and the defaults restored at the end, a 3-finger hop working again;
  hop-swipe away turning that hop round.
  Then the look away from the 80 % desktop: Super+Alt+Up and a drag over
  the empty canvas above it — moving nothing without Super, with it
  panning the content along with the pointer at the desktop's zoom — undone
  together by double Super, which eases back onto the desktop at its
  scale; Super+Alt+Right looked back from the same way; a hop after a look
  away making its landing the view kept, so double Super stays there; and
  the overview doing the same, even when Escape brings back the very view
  the look away had reached. Last, Super+2..5 from desktop 1 alone making
  desktops above, right of, below and left of it, and a covered slot
  passed over; a desktop scaled while it is the overview's target
  drawing its ordinary tab's label from its own text afterwards, never
  the target's older one stretched; and each direction switch turning its
  own gesture alone: with hops away four fingers up still open the
  overview, with the overview swipe down four fingers up go back in and
  down open it, and the pan switch alone turning Super + a scroll round.
  Then frames allocating nothing: a window drag, the same drag with an IPC
  client subscribed (the window list, compared rather than serialised, and
  told when the focus changes it), and a 3-finger pinch over a desktop of the
  overview with a minimized window on the shelf each run a hundred frames
  after a warm-up while a malloc shim (`alloc-counter.c`, not built into
  sanitizer runs, which own malloc) counts what C++ and the text and JSON
  libraries allocate; wlroots', libwayland's and the GL driver's own frame
  bookkeeping is the renderer's and not counted. A probe first proves the
  counter counts an `operator new` and a `g_strdup`. Last, nine windows wider
  than a third of the screen opening one after another cascade to the right
  edge of the usable area, none passes it, and the next opens on the first
  spot again. Last, a second output at scale 2 beside the first, both in
  the overview with a hint: 100 unchanged frames on both rebuild no text
  texture (a counter in the text cache), and a changed hint builds one per
  scale.
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
  Last, the overview's right clicks: on the empty canvas a new desktop, inside
  a desktop clear of windows and on its tab removing it (its windows staying
  put, on no desktop), on a window nothing. Last, canvas.conf, in the run's
  own scratch `$XDG_CONFIG_HOME` (never the user's): written with every
  default on the first run, Super+Space opening the launcher where a lone
  Super tap runs nothing by default, a hand edit saved over the file
  applying to the running session without a restart (`launcher = Super+K`),
  `settings.set` rewriting one line of a hand-written file with every other
  byte kept and reporting the chord taken from the launcher, a `[commands]`
  chord running its command, a bad line listed with its reason and the
  first hinted on every screen, the file's swipe preset reaching the
  gesture engine, the `settings` event reaching a subscriber, and
  `shortcut.capture` over the socket through a forked raw client — the
  recorded chord reported and run by nothing (the terminal it would spawn
  never appears), the same chord unrecorded spawning it, a plain key
  reaching its client, `shortcut.cancel` from a second connection
  answering the pending capture `{"keys": null}` and itself `true` (also
  with no capture under way) after which a bound chord runs its action
  and a plain key reaches the focused client, and the Super tap given to
  the launcher through the socket opening and closing it once the
  double-tap interval has passed, two quick taps running the double tap
  (`pan-back`) and neither single tap, and `shortcut.capture` recording
  two taps as `Super Super` and one, the interval passed, as `Super`.
  Last, a button held on a window keeps the pointer on it while the
  pointer travels off its edge, and the release gives the pointer to what
  is under it.
  Last, the clipboard chords: an X11 client of the scenarios' own
  (`tests/clients/x11-client.c`, through Xwayland) started late and clicked
  to the keyboard, Super+C and Super+V each sending it the app's own chord —
  Ctrl+Insert for the copy, Shift+Insert for the paste — the client saying
  so by X's keycode and modifier state.
- **`launcher.cpp`** — the app launcher: Super+Space opens it — the first
  run having written canvas.conf with every default into the run's own
  scratch `$XDG_CONFIG_HOME` (never the user's), and a lone Super tap
  running nothing by default — the panel renders, search filters, Enter
  and result-click launch foot and close the launcher, the launcher's own
  chord closes the open launcher, logo-as-modifier does not open it,
  Escape and outside-click close it, opening it running no `gsettings`
  program (a stand-in first on PATH records any run; the icon theme is read
  through gio from a keyfile settings backend), no-match queries, and a custom
  desktop entry installed into
  `$XDG_DATA_HOME/applications` before `Server::init`: found by typing its
  name, launched with Enter, and verified by the marker file its spec-quoted
  `Exec` writes plus the foot window it opens. The marker holds the launched
  command's ignored signals and its `XDG_CURRENT_DESKTOP`: SIGPIPE must be as
  the scenario process itself was started (the compositor's own ignore never
  reaches what it launches), and the desktop name `wayland-canvas:wlroots`.

- **`lock.cpp`** — the lock screen, driven by the scenarios' own locker
  (`tests/clients/lock-client.c`, an ext-session-lock-v1 client built with
  the tests, so no swaylock or hyprlock is needed): the `lock` shortcut
  pushes the locker's action to the shell and locks nothing by itself; the
  locker then takes the session, and the screen is its own fill edge to
  edge while a window capture is refused; a canvas shortcut does nothing,
  the keys and a click reach the locker and the pointer focus is its
  surface, and a three-finger swipe moves no camera; a killed locker leaves
  the session locked behind an opaque fill, the next locker takes it over
  and unlocks it, and the windows come back. Last the idle lock: the
  countdown runs from `canvas.conf`'s default, the timeout asks the shell
  to lock, an idle inhibitor stands the countdown down while it lives and
  it runs again once the inhibitor is gone. Nothing in it touches the
  machine's logind session.

- **`xwayland.cpp`** — X11 applications, driven by an xcb client of the
  scenarios' own (`tests/clients/x11-client.c`, built with the tests
  because this machine has no xterm or xeyes): the session names an X
  display, no X11 window exists before a client runs, and the client's
  window then maps as a view with its X11 title and the same title bar
  every window gets, where the user is looking. A click and a key reach
  the client, the title bar's maximize button maximizes it (the client is
  told its new size), and the × closes it through `WM_DELETE_WINDOW` —
  the client says so itself and its view goes with it. Then the client's
  own menu — an override-redirect window — which must sit exactly where
  the client put it, wear no title bar and take no focus; the clipboard
  both ways, an X11 client's copy read with `wl-paste` and a `wl-copy`
  read back inside the client; a compositor of its own, headless on its
  own display, whose `WLR_XWAYLAND` names a binary that is not there: it
  starts, serves its display and stops. Then X's own picture following
  the canvas — a window opened over another takes the click, a minimized
  one takes none, windows past X's screen or larger than it at 25 % take
  clicks at their own points, a click straight after a jump lands where
  it was aimed and so does a press in the same instant as the jump, held
  until X has taken in the moves; on a tiled desktop a modal dialog whose
  hints go to X in the one flush that maps it, as toolkits send them,
  floating centred over its window and still floating after Super+T
  switches tiling off and on; a dialog that maps bare and joins the board,
  its parent and modal state arriving afterwards, leaving the board to
  float centred over its window;
  the keyboard following Super+1 and Super+2 to the window last used on
  each desktop; and last fullscreen: Super+F on a window beside another
  taking it to a new desktop of its own, overlapping none, the screen and
  the keyboard with it and the other window left in place, Super+F
  bringing it back and the desktop going, and a window alone on its
  desktop going fullscreen in place.

- **`touch.cpp`** — the touchscreen and the pen, through a fake
  touchscreen and a fake pen tablet announced on the headless backend and
  a touch, pointer and tablet-tool client of the scenarios' own
  (`tests/clients/touch-client.c`, which asks for the compositor's title
  bar and can leave touch or tablet support out). A tap and a drag reach a
  window as touch, the drag still its window's past the window's edge;
  one finger drags a window by its title bar; a finger held still on the
  overview's empty canvas makes a desktop there, as a right click does;
  two fingers on the canvas look away and spread to scale the shown
  desktop; three fingers hop and four open the overview, but with the
  capture picker open (Super+Shift+4) neither does anything, the picker
  staying until Escape closes it, after which the hop works again. A finger on a
  window as the session locks is cancelled for it, and locked, a tap
  reaches only the locker, as a click since it takes no touch. A window
  without touch support gets a tap as a click; the pen reaches a tablet
  client with pressure and tilt, a client without tablet support as the
  pointer with the barrel button as the right one, and drags a title bar.
  Where this machine has Xwayland, a tap and the pen's tip reach an X11
  window as its press, within a pixel (Xwayland passes a touch through its
  device's 0..65535 range and back). Last, a touchscreen naming a second
  output acts on that output.

- **`capture.cpp`** — the shots-and-recordings UI, end to end, with the
  XDG pictures and videos dirs pointing into `tmp/scenarios/capture/`
  and an `xdg-open` stub on PATH (nothing touches the real user dirs or
  opens a real viewer). Every saved shot is compared pixel for pixel
  against grim — the whole screen (Super+Shift+3, Shift+Print, and the
  toolbar's screen mode), a dragged region (Super+Shift+4, the
  toolbar's region mode), a clicked window and a Space-mode window
  (exactly its frame, alone, at the output's scale). Ctrl on a shot
  chord and the toolbar's destination option are read back with
  wl-paste and matched against the same grim reference. The thumbnail:
  pixels appear bottom-right, a swipe dismisses them, a click reaches
  the xdg-open stub with the saved file's path. The picker keys (Space,
  Esc, Print) behave; the timer's shot lands only after its countdown.
  `capture.pick` answers a dragged region by its numbers, a window by
  its session id and ext-foreign-toplevel identifier, an output and the
  portal's `any` both ways, cancels as `cancelled`, and refuses a
  second concurrent pick; the `msg capture.pick` chooser bridge prints
  the chosen stdin line byte-exactly and stays silent on a cancel.
  Recordings are the compositor's own offscreen frames piped into a real
  ffmpeg: whole screen (stopped by Alt+Print), a region and a window, the
  window's rectangle frozen at pick time (both stopped by `capture.stop`),
  each left as a file ffprobe reads at the right size — odd edges cropped
  to the encoder's whole macroblocks — with `capture.recording` events and
  the state's `recording` following start and stop, and `capture.stop` on
  nothing an error. Last, a recording whose encoder never reads: a
  stand-in `ffmpeg` (`recorder-standins/ffmpeg`, put first on PATH for
  that one start) waits for the pipe to close and reports what it was
  given and how much the pipe still held. While it runs IPC `state`
  answers within 500 ms, frames that came due behind it are dropped and
  counted, grim still gets the screen, and `capture.stop` ends the
  recording with the written frames unread.

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
  must receive a PipeWire stream for a monitor and for a window; xdpw's
  chooser is the compositor's own `wayland-canvas msg capture.pick`
  bridge, and the scenario answers its picker by clicking the source the
  client asked for.

- **`session.cpp`** — the shell plugins a login needs, in a private world:
  its own `dbus-daemon` system and session buses under
  `tmp/scenarios/session/bus/`, scratch XDG dirs, and a scratch PAM service
  that accepts one known password through `pam_exec`, so the machine's
  notification server, polkit agent, logind session and login stack are
  never reached. The notification server keeps its bus name against a
  second daemon; the lock shortcut locks the session through the locker
  plugin, whose clock moves on to the next minute while locked (a shot
  just past the minute's turn, so the run waits up to a minute), a wrong
  password keeps it locked and the right one unlocks it; the password
  dialog, on the polkit plugin's stand-in flow, stays open on a wrong
  password and tells polkit so.

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
  UPower with a discharging 55 % battery, power-profiles-daemon, a PipeWire
  daemon of the run's own holding one null sink, and a
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
  bar's own process runs with exactly those bus addresses and with the
  compositor's own executable directory first on its `PATH` (both read
  from `/proc/<pid>/environ`); looked up by name along that `PATH`,
  `wayland-canvas-agent-usage` — the agents widget's collector — is the
  build's copy, which the scenario's own `PATH` does not hold. Then, with
  the real bar client (qs) started by the
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
  positions are read off the bar screenshot itself. The brightness widget
  is always there, its `brightnessctl` a shim that keeps one backlight in
  a state file. The weather widget and panel are
  therefore always checked: the widget shows beside the clock, and the
  panel lists the fixture's forecast days.

  The wheel is proven on the audio and brightness indicators. The
  PipeWire daemon's socket sits under `tmp/scenarios/bar/pipewire/`, named
  to the bar by `PIPEWIRE_RUNTIME_DIR`; its sink is the default, at volume
  0.50 in the widget's scale (Quickshell's volume is the cube root of
  PipeWire's channel volume), and the backlight starts at 50 %. A 120-unit
  wheel event over each indicator moves its control one step (0.55; 55 %,
  one write), a 15-unit event moves neither, and eight 15-unit events add
  up to one notch for each (0.60; 60 %).

  The network panel's names and secrets are proven the same way. The
  `qrencode` stand-in logs its argv as one line and the text it read from
  its stdin as another, so the share-as-QR check finds the `WIFI:`
  payload on the stdin line only and the password on no argv line. The
  `nmcli` stand-in writes its terse output the way the real tool does,
  with `:` and `\` behind a backslash, and the NetworkManager stand-in
  has methods that move the machine to `cafe:5G` and to `Lounge;Guest`
  (a password holding `;`, `:`, `"` and `\`): the panel must show the
  colon network as connected on its band, pin it by its own profile
  name, and share both networks as escaped payloads. The known network
  `OldCafe` is saved under a profile named `Old Cafe profile`, so
  Forget is proven by the profile's object leaving the stand-in and
  `nmcli connection delete` never being called.

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
  button; a click above the card closes it and nothing more, the toolbar
  and the overview staying. The panel is opened again, and Escape — the
  panel holds the keyboard — closes it. The
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

  Then the shell's watcher on its config file, every way `shell.json` can
  change. The bar's left section ends at the logo while the scratch
  `canvas.hello` plugin is off; switching it on in the file puts its
  label past the logo, so the section's ink edge on a fresh screenshot
  and a new bar surface — counted as wlroots announces each one, since an
  address or a protocol id can come back — prove the shell reloaded and
  applied the edit. The file is saved over twice by rename (a sibling
  written and renamed over it, the inode replaced each time, the way an
  editor saves), deleted (the shell reloads on the defaults and writes
  nothing back, so the file stays missing) and created again by hand;
  then the theme switch makes the host write the file itself, which must
  reload nothing, and a rename-over after that write must still reload,
  twice around. The shell watches only the file, never its directory:
  the compositor rewrites `canvas.conf` beside it on every settings
  change, and that must not reload the shell (the settings phase proves
  it).

  Last, the `canvas.notes` plugin end to end, every change checked where
  the user sees it — the card on the screenshot — as well as in the note's
  own plain-HTML file in the scratch state: `msg action.run
  canvas.notes/create` maps one surface of the shell client named
  `wayland-canvas-canvas-canvas.notes/<id>`, placed on the plane centred
  on the parked pointer — kept on its screen and clear of the bars — at
  the rectangle its file holds, and holding the keyboard. Typing goes into the
  note where it sits; a pan leaves one card where it was put; a colour dot
  turns the file and the card green; a reload brings the note back once;
  writing past the bottom grows the card, drawn to its new height. The
  strip and the grip are pressed and held across steps, as a hand holds
  them: each press hands the drag to the compositor (`surface.move`,
  `surface.resize`), the note follows the pointer while it is held — at
  100 %, on a desktop pinched to 75 % and in the overview at its zoom —
  and stays, in its file and on screen, where it is let go. On a second
  note a word selected and made bold by the strip's B is `<b>` in the file
  and heavier on screen while the first word keeps its ink; the strip's +,
  twice on the word selected backwards, sizes that word alone two pixels
  up, the rest at the note's size, and the strip's B shows on for it; a
  selection dragged from the first word out past the note's edge keeps the
  pointer on the note while the button is held, and a size typed into the
  strip sizes all of it, each part keeping its weight, the line drawn
  taller; Ctrl+I and the strip's U set their span; the font menu,
  filtered by typing, sets an installed family for the whole note; Escape
  gives the keyboard back and a click takes it
  again; a third note left empty when the keyboard goes is deleted, file
  and all. A second output aimed at the first note hit-tests and draws it,
  and its going leaves both notes; the strips' ×s take them away, files
  included.

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
  the probe-reuse window, the panel keeping Fireworks when a day of
  gateway tokens added between two refreshes moves the gateway above it
  (the selected chip and the hero's star mark follow the provider, not
  the position), the manifest's `refreshIntervalSec` reaching the widget
  (written into the scratch `shell.json` as 2 s, the collector runs
  again within 4 s on its own, counted by the fake `codex` app-server's log), and
  both palettes — all on screenshots under `tmp/scenarios/bar/agents*.png`.

  Last, the `canvas.recording` plugin: Alt+Print opens the capture toolbar
  on its recordings and a click of its record-screen mode starts a real
  recording into the bar world's scratch Videos (never the machine's —
  the bar's own `HOME` is the scratch one). The plugin's dot is read off
  the bar screenshot itself: attention-red pixels in the right section (a
  widget the dot displaces can slide under where it sits, so the strip is
  read by colour, not by what moved); a fake-pointer click on it stops
  the recording (`state.recording` null again), the dot leaves the bar
  with it, and ffprobe reads the file as a playing screen-sized video.

  Then the toolbar's edges. The themed foot window is focused and
  Super+M minimizes it onto the shelf, and the overview that follows
  shows the toolbar on its default right edge. Then each edge the
  config can name is written into the scratch `shell.json` as an edit
  (`toolbar.position: left`, `top`, `bottom`, then `right` again) —
  each write reloads the shell, and the strip's placed box is read from
  the compositor's layer surfaces, never assumed: a column or row of
  44 px squares, 8 px from the edge (the top row below the bar),
  centred along it. A side toolbar clears nothing — the shelf keeps its
  place along the bottom — but the bottom edge must carry the shelf
  above the strip (the layout's own numbers, the same ones its
  hit-testing reads). On every edge the note button's colour flyout
  must open toward the screen's centre — its yellow swatch row read on
  the screenshot in the region the toolbar's own geometry puts the card
  — and the button's second click must fold it, the pointer parked
  clear throughout. The run leaves the overview with Escape — the
  toolbar slides out, nothing outlives it — and the config back at
  right. Screenshots land in `tmp/scenarios/bar/edges-*.png`.

  Last, the `canvas.settings` plugin end to end. The usage fixtures are
  dropped once more (with a reload) so the agents widget is back on the
  bar, and the overview's toolbar now holds a fifth button, the gear.
  Clicking it maps the settings panel as a `wayland-canvas-panel-settings`
  layer surface of the shell client; its tab row and every row the phase
  clicks are aimed from the panel's fixed metrics and cross-checked
  against the screenshot first: each tab is read as chosen from its
  accent-tint fill (exactly one), each row from a label band where the
  card lays it, each switch from the track's colour at the end its knob
  has left, each exclusive choice from its dot. The bar the panel opened
  under is remembered (address and resource id) and no write of the
  phase may replace it: the shell never reloads on a settings write. The
  Bar tab's switch on the agents row writes exactly that widget into
  `bar.hidden` in the scratch `shell.json`; the bar loses the widget's
  run with no reload, the row's switch shows off, every other stays on
  and the hidden label turns attention-red; the switch's second click
  empties the list and undoes all of it. The Toolbar tab shows Right
  chosen by default, then chooses Left, Top and Right in turn: each
  choice writes `toolbar.position` into the scratch `shell.json`, lights
  only its own dot, and moves the strip to that edge live (its placed
  box read from the compositor, the bar surface unchanged: the host
  wrote the file itself, so nothing reloads) while the open settings
  card follows it to the strip's inner side — beside a column, level
  with the gear; below the top row, centred on the gear — its tab row
  checked against the screenshot at the place the placed strip implies.
  The Shortcuts tab records a chord by pressing it (the fake keyboard's
  Super+T on the terminal row, whose key chip shrinks from Super+Return
  to Super+T): `canvas.conf` gains `terminal = Super+T`, no other line
  holds it, the running session's resolved keys give it to Terminal and
  take it from Tiling (which held it by default, so has no line to
  rewrite), and the panel stays open with the shell unreloaded while the
  compositor rewrites the file. The Gestures tab shows Three-finger hops
  chosen and the pinch switch on, then chooses Four-finger hops:
  `swipes = four-finger-hops` in the file and only that dot lit. With the
  panel closed on Escape the new chord opens a foot window (`TERMINAL` is
  pinned to `foot` for the run) while the one it replaced opens nothing.
  The panel is opened again on the Shortcuts tab and the App launcher row
  (which carries no note) is clicked: its third line, "Press the new keys
  — Esc cancels", is read off the screenshot. Escape is the capture's
  own cancel: the line goes and the panel stays open. Then the Terminal
  row, now off its default: Reset writes `terminal = Super+Return` back
  into `canvas.conf`, the running session holds the chord again and it
  opens a foot window; Clear writes `terminal = none`, the session
  holds no chord and Super+Return opens nothing; Reset, now alone on the
  row, restores the default once more. The list is wheeled to its end
  (two shots alike mean it stopped) and the Add a command row clicked: a
  draft row with a chord chip stands above it. The chip is clicked and
  Super+Y recorded: the draft shows the chord it holds (a wider chip than
  None) but `canvas.conf` gains no line, the compositor's parse errors
  stay empty and no file-error hint appears, even after a pause long
  enough for a write. The row's field is clicked and `markcmd` typed (a
  script on the run's tool PATH touching a marker file), and only Enter
  writes `Super+Y = markcmd`, with no error; the marker is absent until
  Super+Y is pressed, then present. Emptying the saved row's field (Ctrl+A,
  Backspace, Enter) saves nothing: the line still holds `markcmd`. Remove
  takes the line out of the file and the command out of the running
  session. A second draft goes the other way round: `markcmd` typed into
  its field without Enter, then the chip clicked and Super+U recorded.
  The typed text rides along, so the one write is `Super+U = markcmd`,
  once, with no file error, and the chord runs the command; Remove takes
  it out again. The list is wheeled back to its top and the App launcher
  row clicked to record again; the gear's second click closes the panel
  while it records.
  Closing while recording cancels the recording, so the keys are free at
  once — Super+Return opens a foot window (`TERMINAL` pointed at a script
  that writes what it is typed into a marker file), Escape leaves the
  overview, and a plain key typed into that window reaches its client
  while `terminal` is still `Super+Return` in `canvas.conf`.
  One earlier step, the adopt phase's hand edit of `shell.json`, is saved
  the way an editor does, renamed over the file: the shell must still
  reload on it, though it no longer watches the config directory (the
  compositor rewrites `canvas.conf` there on every settings change).
  Screenshots of the panel, its four tabs and both file-writing steps land
  in `tmp/scenarios/bar/` as `settings-*.png`.

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
