# Views per shell, the review's bugs, one owner per fact

Date: 2026-10-19 18:54 EDT

## Context

Every finding of a review of the whole tree is fixed:

- The X11 crashes cannot be written again. An X11 window and a wayland
  window are two subclasses of one abstract `View`, and no code outside
  the two shell files reaches a shell's wlroots object.
- Every other crash goes: the known ones, and whatever a sanitizer run of
  every suite finds.
- Every other bug goes:
  - shots, window shots, window casts and recordings;
  - the night light;
  - the shell and its plugins;
  - packaging and tools;
  - tests that pass without checking what they claim.
- Each class and file holds one concern. The hubs that grew several are
  split by concern:
  - in the compositor: `Server`, `Input`, `View`, `Ipc`, `Painter`,
    `CaptureUi`, `settings.cpp`, the usage collector and `output.cpp`;
  - in the shell: `Host.qml` and the network, settings and notes plugins;
  - in the tests: the bar scenario.
- Every fact that two places keep, and that can drift apart, has one
  owner the others read.
- State machines and modes take the shapes cpp-standards.md gives them,
  and dead code goes.
- Comments say only what the code cannot. No banners, and no false,
  stale or restating comments. A comment goes wherever behaviour is not
  obvious.

Requirements:

- `View` is abstract, and `XdgView` and `X11View` are its `final`
  subclasses. The `is_x11() ? … : …` dispatch, `ViewShell` and the public
  `wlr` and `x11` pointers are gone.
- X11 windows resize like any window: by an edge, by Super+right-drag,
  and by their own resize request.
- Each bug fixed here gets a scenario or unit test that fails without the
  fix, wherever it can be driven headless.
- A split moves code and changes no behaviour; the suites prove it.
  Length alone is no reason to split a function or a file: a split
  follows a concern.
- No compatibility paths. A name, field or file goes when its
  replacement lands.

Intents settled with this plan:

- [platform/cpp-standards.md](../intents/platform/cpp-standards.md): a
  subclass per kind.
- [windows/x11.md](../intents/windows/x11.md): one View, two shells; X11
  resizing.
- [apps/capture.md](../intents/apps/capture.md): the recorder never stalls
  the compositor.
- [shell/plugins.md](../intents/shell/plugins.md): a config that does not
  parse is never written over.

Each split adds its new subsystems to cpp-standards.md's
"One per compositor" row.

## Working through it

- Work the phases in this order:
  1. phase 0, before 2026-11-01;
  2. phases 1-5: intent, crashes, bugs;
  3. phases 6-8: one owner per fact;
  4. phases 9-11: structure;
  5. phase 12: comments;
  6. Verify.

  The splits come after the de-duplication, so they move code that
  already exists once. Within a phase the boxes are independent unless a
  box says otherwise.
- Line numbers are those of commit `daa70d1`. Where code has moved, find
  it by the names given.
- Commits:
  - On a work branch each box is its own commit, made once the unit tests
    and scenarios that cover what it changed pass, plus
    `tools/check.sh --sanitize` when it changes ownership, lifetimes or
    buffers. The box's tick goes in the same commit (the
    `incremental-commits` and `verification` skills).
  - Promoting a branch to main squashes its box commits into one commit
    per phase, or per coherent part of a phase.
  - `tools/check.sh` runs once per batch: on main, after a batch of
    branches has merged and before a pause or a push.
  - A split is its own commit on the branch.
  - A box whose on-screen check is still owed stays open, marked
    "(on screen owed)".
- Where a box says a check fails first: write the steps, run them against
  the current code, see them fail, and say so in the commit body.
- A box that cannot be done as written stays unticked. That covers code
  that is not as described, or a check that cannot be driven headless.
  The box's text then says what was found, and the work goes on.
- Read AGENTS.md before a change, plus the skills it names for that
  change: `code-style`, `robust-architecture`, `fresh-deploy` and
  `verification`, and `canvas-plugins` for QML.
- A throwaway reproduction of the three X11 crashes may still be in
  `tmp/review/x11-resize/` (`repro.cpp`, `build.sh`). Nothing below depends
  on it.

## Tasks

### 0. Before 2026-11-01

- [x] The agent-usage unit test holds in every season
  - The problem: `tests/test_agent_usage.cpp:703` sets
    `TZ=America/New_York`, and `test_midnight_buckets` (624-655) assumes
    UTC-4. Under EST that check fails `(600 0 0)`, and `tools/check.sh`
    stops at the unit suite.
  - The fix: set a fixed-offset POSIX zone, `setenv("TZ", "<-04>4", 1)`,
    which is UTC-4 all year and needs no zoneinfo. That also steadies
    `date_offset`'s 86400 s steps (65). Correct the comments at 625-628
    and 701-702.
  - Acceptance: `meson test -C build --suite unit` passes, and so does
    `TZDIR=/nonexistent build/test-agent-usage`.

### 1. Intent

- [x] cpp-standards.md: a subclass per kind. x11.md: one View, two shells,
  and the Resizing row. capture.md: the recorder never waits for its
  encoder. plugins.md: an unparsable config is never written over.

### 2. A view per shell: the X11 crashes

- [x] The xwayland scenario drives the three crashes and fails on them
  - Add these steps to `tests/scenarios/xwayland.cpp`, against the X11
    window of `tests/clients/x11-client.c`:
    - (a) Hold Super, press BTN_RIGHT over the window, move in four steps,
      then release.
    - (b) Focus the window with a click, then lock the session with the
      lock client. Add `CANVAS_LOCK_CLIENT` to this scenario's env, as
      meson.build:403 does for the lock scenario. Then unlock.
    - (c) Press Super+Shift+4, then click on the window.
  - After each step, an IPC `state` round trip shows the compositor alive;
    the boxes below add their own checks.
  - Hold the button across steps: press, wait, move, release (the
    `canvas-plugins` skill, rule 7).
  - Before the split, each of (a), (b) and (c) ends the run with SIGSEGV
    (exit 139). Commit these steps with the split, not before it.
- [x] `View` is abstract, and `XdgView` and `X11View` are its two shells
  - `src/windows/view.h` gets a protected `View(Server &)` and
    `virtual ~View()`.
  - Every shell query becomes a pure virtual, named as today minus the
    dispatch: `surface`, `shell_geometry`, `initialized`, `send_close`,
    `configure_size`, `tell_maximized`, `tell_fullscreen`, `tell_tiled`,
    `tell_activated`, `shell_title`, `shell_app_id`, `client_pid`,
    `wants_maximized`, `wants_fullscreen`, `shell_parent`,
    `resize_settled`, `for_each_surface`, `surface_at`.
  - New queries:
    - `modal()`;
    - `client_placed()`, the override-redirect test;
    - `min_size()` and `max_size()`, which grab.cpp:255-266 reads off the
      xdg toplevel today;
    - the resize hooks of the next box.
  - `kind()` stays a plain `View` member, built from `client_placed()`,
    `modal()` and `shell_parent()`.
  - Delete: `ViewShell` and `shell()` (they have no callers), `is_x11()`,
    the 19 ternaries, the `xdg_*`/`x11_*` private declarations, and the
    public `wlr`, `x11` and `override_redirect` members.
  - New `src/windows/view_xdg.h`: `class XdgView final : public View`.
    - It holds `wlr_xdg_toplevel *const wlr` (cpp-standards Names: the
      wrapped object is `wlr`) and the listeners `listen_xdg()` connects
      today.
    - It has `static XdgView *from_surface(wlr_surface *)`.
    - Today's `xdg_*` bodies become its overrides in `view_xdg.cpp`.
  - New `src/windows/view_x11.h`: `class X11View final : public View`.
    - It holds `wlr_xwayland_surface *const wlr`, `override_redirect` and
      the listeners `listen_x11()` connects today.
    - It has `static X11View *from_surface(wlr_surface *)`.
    - Today's `x11_*` bodies become its overrides.
    - The X-only members the X window manager calls become public
      members: `x11_x`, `x11_y`, `x11_follow_canvas`, `x11_configure`,
      `x11_place_override_redirect`.
  - `View` gets `virtual X11View *as_x11()` and a `const` twin; they
    return nullptr, and X11View's return `this`.
    - Only the X window manager calls them, where `is_x11()` stands
      today: view_x11.cpp:34, 320, 389, 411; `Xwm` after phase 9.
    - RTTI stays off.
  - decoration.cpp:298 (`is_x11() && !override_redirect`, decorated
    without negotiating) becomes a View query each shell answers.
    X11View: not override-redirect. XdgView: false.
  - The shared life-cycle handlers become protected View members:
    `handle_map`, `handle_unmap`, `handle_first_commit`, `handle_commit`,
    `handle_destroy`, `handle_request_move`, `handle_request_resize`,
    `handle_set_title`, `handle_set_app_id`.
    - Each shell's listeners connect them as
      `connect<&XdgView::handle_map>(…, this)`. A protected base member
      has to be named through the derived class, and `Listener::connect`
      accepts a base handler for a derived owner (util.h:43).
    - `handle_request_resize` takes `uint32_t edges` instead of the xdg
      event, so both shells can call it.
  - No virtual call from `View`'s constructor or destructor. The subclass
    constructor connects its own listeners.
  - `handle_destroy()` still ends with
    `destroy_owned(server.toplevels, this)`, which deletes through the
    virtual destructor.
  - Every `void *data` that points back at a window stores
    `static_cast<View *>(this)` and is read back as `View *`. That covers
    `toplevel->base->data`, `xwayland_surface->data` and `surface->data`
    (`x11_parent()` reads one at view_x11.cpp:135).
  - Creating views:
    - `Server::handle_new_xdg_toplevel` (view.cpp:903-905) moves to
      `view_xdg.cpp` and makes an `XdgView`.
    - `Server::handle_new_xwayland_surface` makes an `X11View`.
    - `Server::toplevels` stays `std::vector<std::unique_ptr<View>>`.
    - `View::from_surface` tries `XdgView::from_surface`, then
      `X11View::from_surface`; `from_xdg_surface` and `from_x11_surface`
      go.
  - Code outside the shell files that needs a shell's own object gets a
    View query instead. Only files in `src/windows/` include
    `view_xdg.h` or `view_x11.h`.
  - Rewrite the comments at view.h:16-18, view.h:73-75 and
    view_xdg.cpp:1-3 to describe the classes.
  - Acceptance, every check printing nothing:
    - `git grep -nE 'is_x11|ViewShell|override_redirect' -- src ':!src/windows/view_x11.*'`
    - `git grep -nE 'wlr_xdg_toplevel|wlr_xwayland_surface' -- src/windows/view.h`
    - `git grep -lE 'windows/view_(xdg|x11)\.h' -- src ':!src/windows'`
- [x] An interactive resize belongs to the view, and each shell answers
  its own part
  - Today grab.cpp does all of it, and skips X11 windows (233-235) even
    though their borders show resize cursors. It:
    - writes `view.resize_edges`, `resize_end_serial` and `resize_start`
      (117-119);
    - calls `wlr_xdg_toplevel_set_resizing` (121, 524) and
      `wlr_xdg_toplevel_set_size` (269).
  - The new shape:
    - View gets `begin_interactive_resize(uint32_t edges)`,
      `resize_to(wlr_box)` and `end_interactive_resize()`.
    - The grab only computes the box, clamped by `min_size()` and
      `max_size()`.
    - The 100×60 floor becomes `config::` constants; grab.cpp:256 has
      them as bare numbers.
  - XdgView:
    - `resize_to` sets the size.
    - The left or top edge is anchored on commit, as now
      (view.cpp:800-814), until the client acks the configure that
      `end_interactive_resize` sends with resizing off
      ([bugs/resize-anchor-lost-on-release.md](../intents/bugs/resize-anchor-lost-on-release.md)).
  - X11View:
    - `resize_to` sends the place and the size in one `x11_configure`,
      so nothing needs anchoring.
    - `end_interactive_resize` ends the resize at once.
  - The three anchor fields become one private
    `std::optional<ResizeAnchor>`, on XdgView if only it needs them.
    view.cpp's five `resize_edges = 0` (435, 490, 550, 772, 812) become
    one reset call.
  - X11View connects `request_resize` to `handle_request_resize`
    (view_x11.cpp:55 connects only `request_move`). An X11 client's own
    resize then starts the same grab.
  - Acceptance:
    - Step (a): after the release the window is larger by the drag, and
      the compositor is still running.
    - A left-edge drag keeps the right edge within 1 px, both after the
      release and after the client's next commit.
    - The input scenario's xdg resize checks stay green.
- [x] The unlock and the window shot go through View queries
  - lock.cpp:139 calls `focused_view->surface()`.
  - capture.cpp:196-201 uses `view.shell_geometry()` and `view.surface()`,
    with the same empty-geometry fallback as draw_view
    (render.cpp:349-396). Phase 4 merges the two paths.
  - Acceptance:
    - Step (b): after the unlock the compositor is running, and
      `seat->keyboard_state.focused_surface` is the X11 window's surface.
    - Step (c): the shot's PNG has the window frame's size and shows the
      client's colour.
- [ ] xdg popups and the xdg-shell globals live beside XdgView (on screen owed)
  - Move `Popup`, `popup_owner_view`, `popup_owner_layer` and the
    unconstrain code (view.cpp:262-345, 1025-1046) to
    `src/windows/popup.{h,cpp}`.
  - Move `wlr_xdg_shell_create` and `handle_new_xdg_popup`
    (view.cpp:887-909) to `view_xdg.cpp`.
  - Acceptance: `git grep -nE 'wlr_xdg|wlr_xwayland' -- src/windows/view.cpp`
    prints nothing; the scenarios are green; in a nested run, a GTK app's
    menu opens where it should.
- [ ] Phase 2 verified (on screen owed)
  - `tools/check.sh` is green.
  - Nested run (`build/wayland-canvas -s foot` inside the session) with
    an X11 app such as `xterm`, `xeyes` or `xclock`:
    - resize it by an edge and by Super+right-drag;
    - shoot it with Super+Shift+4;
    - lock and unlock while it has focus.

### 3. Every other crash

- [x] A sanitizer run of every suite
  - Configure a second build:
    `meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false`.
    It goes beside `build/` and is git-ignored like it; add it to
    .gitignore if it is not covered.
  - Run it:
    `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 meson test -C build-asan --suite unit --suite scenarios --timeout-multiplier 4`.
  - Each report naming our code gets a box of its own in this phase: the
    file and line, the scenario step that hit it, and the fix.
  - Leaks and reports wholly inside third-party frames (mesa, pixman,
    glib) go in one suppressions file under `tests/`, each entry with a
    one-line reason. Our own frames are never suppressed.
  - Afterwards, `tools/check.sh` gains a `--sanitize` switch that runs
    this build, so the run can be repeated.
  - Acceptance: both suites run clean under the sanitizers.
- [x] The server releases its SIGINT and SIGTERM sources
  - server.cpp:491-492 (`Server::init`), hit by every scenario at exit:
    `wl_event_loop_add_signal` sources were never removed.
  - The fix: `sigint_source_` and `sigterm_source_` are `EventSourcePtr`
    members, dropped first in `~Server`.
- [x] The `GList` that `json_array_get_elements` returns is freed or not made
  - agent_usage.cpp `cache_load_file` and `read_limits_cache`, and
    tests/scenarios/bar.cpp:2807 (`collector_providers`, hit by the bar
    scenario's usage steps) leaked it.
  - The fix: walk the array with `json_array_get_length` and
    `json_array_get_element`.
- [x] The scenario clients free what they allocate
  - tests/scenarios/capture.cpp:920 (the thumbnail-click step) dropped the
    surface `load_newest_shot` returns; the x11-client never freed its
    two atom replies (xwayland scenario).
  - The fix: the surface is destroyed; the replies are freed.
- [x] The usage collector survives a NULL `session_id`
  - The problem: agent_usage.cpp:648-649 builds a `std::string` from
    `sqlite3_column_text(statement, 0)` before any null check. A NULL
    column crashes the collector instead of skipping the row, as the
    code beside it intends.
  - The fix: check both columns for null and skip the row.
  - Acceptance: `tests/test_agent_usage.cpp` gains an opencode fixture
    database with a NULL `session_id` row; the row is skipped and the
    rest count.
- [x] An IPC client is destroyed the way every owned object is
  - The problem: `Ipc::close_client` (ipc.cpp:1590-1597) and `reap`
    (1599-1600) erase the `unique_ptr` in place. The Client's destructor
    therefore runs while `clients_` is mid-erase, and `~Client` answers
    pending replies (568-582).
  - The fix: use `destroy_owned`'s order, taking the object out first and
    destroying it after the vector is whole. `reap` collects the doomed
    clients first, then destroys them.
  - Acceptance: the ipc scenario disconnects a client with each deferred
    call pending (capture.pick, shortcut.capture, surface.move), then
    finishes the pick, capture or drag. The run is clean under the
    sanitizer build.
- [x] A window that goes is forgotten by everything that keeps a pointer
  to it
  - The problem: cpp-standards says the one subsystem holding a view
    pointer is told when the view goes. Today `Input::forget` passes a
    dying View only to the capture UI (input.cpp:797-803).
  - Audit every `View *` kept past a call:
    - grab states;
    - gesture states;
    - capture picks and casts (image_capture);
    - pending surface drags;
    - fullscreen desktops;
    - overview targets;
    - launches;
    - the decoration under the pointer.
  - Each holder is told through one `view_gone(View &)` fan-out from
    `handle_destroy`.
  - Acceptance: a scenario closes a window during each hold (a move grab,
    a capture pick hovering it, a window cast, a fullscreen desktop) and
    runs clean under the sanitizer build.
  - Done: the `View *` holders are the grab states (move, resize, title
    button, an overview press's window or shelf target), the capture UI
    (hover, a shot waiting on a window), window casts and
    `Input::last_title_view`. Gestures, surface drags, launches and
    overview targets hold none; fullscreen desktops hold a session id and
    reconcile each frame. `Server::view_gone` tells the four from
    `handle_unmap` and `handle_destroy`; casts end there too, no longer
    on the surface's own signals. The `viewgone` scenario closes a window
    during a move grab, a capture pick, a window cast and a fullscreen
    desktop, clean under `tools/check.sh --sanitize`.

### 4. Screenshots and recordings

- [x] A window cast of an X11 window works
  - Today the cast takes the same `render_view_offscreen` path as the
    shot (image_capture.cpp:81), which crashed until phase 2.
  - Acceptance: the sharing scenario casts the X11 window of
    x11-client through the ext-image-copy-capture toplevel source. It
    gets frames the window's size showing the client's colour, and the
    compositor is still running.
- [x] A window is drawn by one path on screen and in a capture
  - The problem:
    - `render_view_offscreen` (render/capture.cpp:151-210) is a hand copy
      of `draw_view` (render.cpp:338-397).
    - It sits on its own copies of logical_box and draw_surface_logical
      (capture.cpp:13-80); this copy is the one that crashed on X11.
    - The surface-origin fallback exists three times (also
      render.cpp:625-633).
  - The fix:
    - Render the window shot and the window cast through `draw_view`,
      with a zoom-1 `RenderCtx` at the frame's origin, the way
      `render_area` (render.cpp:1146-1180) builds offscreen contexts.
    - Add one `View::surface_origin()`.
    - Delete the copies and their "like the renderer's …" comments.
  - Acceptance: the capture and sharing scenarios are green (window-shot
    pixels and window-cast frames, xdg and X11).
- [x] The recorder never stalls the compositor
  - The problem:
    - ffmpeg's stdin pipe is blocking (capture_ui.cpp:932), and
      `push_frame` (1007-1040) writes whole frames from the event loop.
    - An encoder that falls behind therefore freezes input and drawing.
    - Each frame allocates: `capture_output` makes a GPU buffer and a
      cairo surface (render/capture.cpp:229-243), and `render_area`
      makes a vector (render.cpp:1175).
  - The fix:
    - Make the write end `O_NONBLOCK`, and write from a
      `wl_event_loop_add_fd` source with `WL_EVENT_WRITABLE`.
    - `EAGAIN` keeps the rest of the frame for the next writable event.
    - A frame that is due while the last one is still being written is
      dropped, counted, and logged once per recording.
    - EPIPE or 0 ends the recording, as now.
    - Render into one buffer kept for the recording, through a
      `capture_output` variant that takes the buffer; the window source
      already reuses one (image_capture.cpp:77-81).
  - Correct the comment at capture_ui.cpp:1005-1006, which says a blocked
    pipe ends the run.
  - Acceptance:
    - The capture scenario's recording still yields a playable file.
    - A new step records with a stand-in `ffmpeg` first on PATH that
      never reads stdin. While it runs, IPC `state` answers within
      500 ms, and the stop ends the recording.
- [x] The capture toolbar's layout is one pure function the test calls
  too
  - Today `toolbar_layout` (capture_ui.cpp:663-712) is private, and
    bar.cpp:2843-2865 re-derives it.
  - Button kinds are decoded by arithmetic: `i >= 3`, `index % 3` and
    `index >= 6` (690, 730, 754, 1442-1449). `paint_glyph(int target)`
    takes 0, 1 or 2.
  - The fix:
    - Move the layout to a header as a pure function of the output size.
    - The buttons become a `{target, record}` struct, and the target an
      enum class.
    - bar.cpp clicks through the function.
  - Acceptance: the capture and bar scenarios' toolbar clicks are green.
  - Done: `toolbar_layout(width, height)` and `toolbar_hit` are in
    `capture_toolbar_layout.{h,cpp}`, unit-tested by `test-capture-toolbar`.
    The buttons are a `ToolbarMode{target, record}` or a `ToolbarOption`;
    the capture and bar scenarios click through `toolbar_layout`. The
    capture scenario's copy lacked the narrow-output clamp; the one
    function has it.
- [x] The capture UI is five components, not one class
  - Today `CaptureUi` (capture_ui.h:205-300, capture_ui.cpp) holds the
    picker, the toolbar, the clipboard, the ffmpeg recorder, the
    countdown and the thumbnail. Its own banners name them.
  - Split it into:
    - `Recorder` (pipe, process, timers, frames);
    - `CapturePicker`;
    - `CaptureToolbar` (layout, hit-testing, drawing);
    - `ShotThumbnail`;
    - `ClipboardImage`.
  - `CaptureUi` keeps the input routing and the countdown, and stays the
    one `UiLayer` of phase 6.
  - Files: `src/apps/capture/*.{h,cpp}`, or beside capture_ui.* in
    `src/apps/`.
  - Acceptance: the capture, sharing and bar scenarios are green, and
    `capture_ui.cpp` holds only the routing and the countdown.
  - Done: beside capture_ui.*, in `src/apps/`: `capture_picker`,
    `capture_toolbar` (over `capture_toolbar_layout`), `shot_thumbnail`,
    `clipboard_image`, and `shot_taker` (`take_shot`, the cursor
    composite); `capture_geometry` holds `frame_on_output`;
    `capture_options.h` and `capture_overlay.h` are the shared structs.
    The picker and the toolbar report a finished pick or a clicked mode
    to `CaptureUi` through a `std::function_ref` argument.
    `capture_ui.cpp` keeps the routing, what becomes of a pick
    (`pick_done`, `answer_ipc`) and the countdown.

### 5. The other bugs

The compositor:

- [x] The night light reaches a real display and owns its transforms
  (`src/platform/night_light.cpp`)
  - The built-in filter is a matrix (`wlr_color_transform_init_matrix`,
    89), but the DRM backend takes only 3×1D LUTs (wlroots
    `backend/drm/drm.c:882-886`).
    - So the test in `apply_to_output` fails every frame and nothing is
      warmed.
    - Build a 3×1D LUT per output from `night_light_multipliers`, with
      `wlr_output_get_gamma_size(output.wlr)` entries.
  - `wanted()` (93-102) returns two kinds of transform:
    - a new reference from `wlr_gamma_control_v1_get_color_transform`,
      which nobody unrefs, leaking one per frame while wlsunset runs;
    - a borrowed one otherwise.
  - Keep one owned `Handle<wlr_color_transform, wlr_color_transform_unref>`
    per output:
    - rebuilt only on a set_gamma, a settings change or a mode change;
    - committed only when it differs from what the output has.
  - A gamma client that goes away restores the compositor's own state,
    the built-in filter or none. Today a null transform returns early
    (106-108), and the client's ramp stays.
  - A table the output cannot hold is refused with
    `wlr_gamma_control_v1_send_failed_and_destroy`
    (night-light.md: the client is told).
  - Correct render.cpp:1124-1126 and night_light.h:28-29. Both promise an
    in-frame fallback that night-light.md rules out.
  - Move night_light.* to `src/platform/`; it only changes output state.
  - Acceptance:
    - A unit test of the LUT built from a temperature: the endpoints are
      right, the channels are monotonic, and blue is lowest.
    - The nightlight scenario is green, and clean under the sanitizer
      build.
    - On the TTY the panel warms when the switch is on; see Verify.
  - Done: `NightLight` keeps one `Slot` per output: the client's table, the
    built-in LUT and the sizes they were made for. Only `src/platform/` code
    changes output state, so `night_light.*` moved there and the ramp
    arithmetic is `night_light_ramp.*`, unit-tested by `night-light-ramp`.
    The nightlight scenario gives the headless output a fake ramp and
    records the transforms its commits carry. Under the sanitizer build the
    old code leaked 12 LUT transforms there (9504 bytes); the new code
    leaks none. The multipliers were not 1 at 6500 K (green 0.9965, blue
    0.9806), against the intent; each channel is now taken relative to its
    value at 6500 K. A control destroyed with its output no longer reaches
    `output->data`, which was never cleared.
- [x] `settings.set` range-checks before narrowing
  - The problem: ipc.cpp:1476 casts the int64 to `int` first, so
    `lockIdleMinutes: 4294967301` writes 5.
  - The fix: compare the int64 with the bounds first, then narrow.
  - Acceptance: the ipc scenario sends 4294967301; it gets an error and
    canvas.conf is unchanged.
  - Phase 6's settings table replaces this code. Whichever lands first
    carries the test.
  - Done: `lockIdleMinutes` had no upper bound at all, and the idle timer's
    `minutes * 60 * 1000` overflows an int past 35791 minutes. Both the
    `settings.set` check and the `lock-idle` parser now use
    `settings::max_lock_idle_minutes`. The ipc scenario sends 4294967301
    (2^32 + 5) and a temperature of 2^32 + 4000, each wrapping to a valid
    value, and finds an error and canvas.conf unchanged; the old code fails
    those three checks.
- [x] A cascaded window stays inside the usable area (confirm first)
  - The suspected bug: `place` (view.cpp:160-246) clamps only the
    vertical position after cascading (235-244), with bare 32 and 64
    (215-234). A window cascaded near the right edge would then pass it.
  - Acceptance: a gestures or input step opens windows until the cascade
    reaches the right edge; each frame stays inside the usable box.
  - Done: the bug reproduced. A gestures step opened nine windows wider
    than a third of the screen, one after another: three of the nine
    reached 4555 px on an area ending at 4488. `place` now cascades by
    `config::cascade_step` only as far as the whole frame stays inside the
    area, and a window opening when that is used up takes the first spot
    again. The step checks that none passes the edge, that the cascade
    reached it, and that the next window opens at the first spot.
- [x] Per-frame paths allocate nothing, as cpp-standards.md says
  - `emit_windows` (ipc.cpp:1641-1653) JSON-serialises every window on
    every frame of a drag (`View::move` marks the list dirty,
    view.cpp:388-389; output.cpp:420-423 flushes it), even with no
    subscriber.
    - Skip it when no client is subscribed.
    - Compare a reused vector of rows, not serialised text.
  - `LayerSurface::name()` (layer.cpp:225-227) copies the namespace on
    every pointer motion: `belongs_on_canvas`, `layer_surface_at` (353)
    and `pointer_hit_test` (input.cpp:893) all reach it. Compute the
    on-canvas flag once, when the surface is made.
  - `pinch_update` (gesture.cpp:510-537) formats the hint text and calls
    `Hint::set` on every event, and `Hint::set` resets the texture and
    damages every output (render.cpp:947-971).
    - Keep the previewed outcome on the pinch, and set the hint only
      when it changes.
    - `Hint::set` returns early on unchanged text.
  - `draw_shelf` lays out each minimized title through Pango every frame
    and frees the texture at the loop's end (render.cpp:652-661). Cache
    the titles; phase 6's text cache.
  - `overview_tab_layout` runs `snprintf` per desktop per frame to guess
    a width (874-879). Measure once per label.
  - Acceptance: an allocation counter in a debug-only test hook, or an
    `LD_PRELOAD` malloc counter in a scenario, shows zero allocations
    across 100 frames of a window drag and of a pinch. If neither can be
    built cleanly, review each path by reading and note that in the box.
  - Done: three counted paths, each a hundred frames after a warm-up, in
    the gestures scenario: a window drag, the same drag with an IPC client
    subscribed, and a 3-finger pinch over a desktop of the overview with a
    minimized window on the shelf. The counter is a malloc shim,
    `tests/scenarios/alloc-counter.c`, linked into that scenario only
    when the build is not sanitized (a sanitizer owns malloc; the step then
    says SKIP). It counts what C++ (`operator new`) and glib, pango, cairo,
    pixman, json-glib, harfbuzz, fontconfig and freetype allocate; what
    wlroots, libwayland and the GL driver allocate for their own frame
    bookkeeping is the renderer's and not counted. A probe first proves it
    counts an `operator new` and a `g_strdup`. Before the fixes the drag
    allocated 604 times, with a subscriber 965, and the pinch 603.
    - `emit_windows` reads the list into reused `WindowRow`s and compares
      them with the last pushed rows, and does nothing with no subscriber.
    - `LayerSurface` decides `on_canvas_` once, in its constructor.
    - A pinch keeps its previewed `PinchOutcome` and sets the hint only
      when it changes; `Hint::set` of the same text only restarts the timer.
    - Shelf titles come from `Painter::shelf_title`, a cache of their own
      keyed by window and scale and redrawn when the title or the palette
      changes, until phase 6's text cache; `overview_tab_layout` formats
      the number with `to_chars`, not `snprintf`.
    - A bug found on the way: `Decoration::update` compared the title's
      presence the wrong way round (`title == nullptr` against
      `has_value()`), so a window with a title redrew its title bar on
      every frame.
- [x] The icon theme is read without blocking the compositor (confirm
  first)
  - The suspected bug: `detect_theme` (icons.cpp:96-107) runs
    `gsettings` through `popen` and waits on it. Check whether it runs
    on the compositor's loop: at start, or when the launcher opens.
  - If it does, read the key through gio, which theme_prefs already
    links, or once at start before the loop runs.
  - Done: it did. The first icon lookup after start (the first launcher
    open; the chain then lasts until teardown) ran `gsettings` through
    `popen` on the loop: the launcher scenario with a `gsettings` stand-in
    first on PATH failed "opening the launcher ran no gsettings program"
    before the fix.
    `theme_prefs::icon_theme()` now reads the key through gio in the
    compositor's process, from the schema `theme_prefs::apply` already
    owns, and `detect_theme` falls back to GTK's `settings.ini` as before.
    The scenario runs the keyfile settings backend and the unit test
    writes `icon-theme` into it and reads it back. What the icon chain then
    holds is not read off the screen: no scenario step reads icon pixels.
- [x] Teardown runs in reverse order of creation
  - The problem: logind finishes after ipc and night_light
    (server.cpp:435-437), though it starts last (632). cpp-standards'
    teardown rule says reverse order.
  - Make `~Server` exactly the reverse of `init` and `start`.
  - Done: `~Server` runs `start()` backwards (services, logind, the session
    bus hand-back, the canvas.conf watch, ipc, Xwayland), then `init()`
    backwards (night light, lock, image capture, input, decorations,
    layers, the shell, outputs). The SIGINT and SIGTERM sources are released
    first on purpose: nothing dispatches the loop during teardown, so a
    signal then must take the process down, not wait in a source nobody
    reads. Every suite and the sanitizer run pass.
- [x] logind's sd-bus objects have owners and its calls are checked
  - logind.cpp frees sd-bus messages and errors by hand (48-57) and
    writes its own `Bus` destructor (21-30). Both become `Handle`s
    (cpp-standards: a C object with a destroy function).
  - The `sd_bus_match_signal` results (78-83) are checked and logged.
  - Done: the bus, messages and errors are `Handle`s (`BusPtr`,
    `MessagePtr`, `ErrorPtr`); `GetSessionByPID`, `Inhibit`, the message
    reads, `dup`, the three match signals, the bus fd and its event
    source are all checked and logged. Two behaviours came with the checks:
    a sleep delay is only held when the `PrepareForSleep` match took, and a
    bus that fails in `handle_bus` is finished, since its fd stays readable
    and would spin the loop. Built and run through every suite and the
    sanitizer; the sd-bus paths themselves need a login session (the TTY
    run under Verify), so no headless run reaches them.
- [x] A removed tiled desktop releases its windows' board seats
  - The problem: `Desktops::remove` (desktop.cpp:234-248) un-tiles
    windows through a callback into `server_.canvas`. It leaves their
    `board` and `slice` naming the removed desktop: a second, partial
    `leave_board` (canvas.cpp:341-345).
  - Phase 9's Canvas box moves this into `Canvas::remove_desktop`. Until
    then, call `leave_board` for each window.
  - Acceptance: a gestures step abandons a tiled desktop with two
    windows; afterwards neither window has a board.
  - Done: `Desktops::remove` calls `Canvas::leave_board` (now public) for
    every window whose board is the removed desktop, as `toggle_tiling`
    does, instead of un-tiling the windows whose centre lies in it. The
    gestures scenario's last step tiles a desktop with two windows and
    abandons it; the old code fails that check and the new passes.
- [x] An X11 modal dialog is laid out as a dialog every time
  - The problem:
    - The xwayland scenario's modal-dialog checks fail now and then:
      "its modal dialog floats, off the board", "centred over the window
      it blocks" and "tiling leaves the modal dialog floating".
    - The dialog is laid out as a plain tiled window, as if its
      `WM_TRANSIENT_FOR` and `_NET_WM_STATE_MODAL` were not read yet when
      it maps.
    - Seen once in 27 runs during the touch-and-pen work, and again in a
      `tools/check.sh` run on main with four suites loading the machine;
      the rerun passed. This box moved here from
      2026-10-19-1203-touch-and-pen.md.
  - The fix:
    - Find the order in which wlroots' Xwayland reports the parent, the
      modal state and the map: `associate`, the `set_parent` and
      `set_window_type` events, and the `_NET_WM_STATE` updates.
    - Then lay the window out again when one of those arrives after the
      map, the way the first commit does for a wayland dialog. Do not
      wait or poll for them.
  - Acceptance:
    - A scenario step reproduces the late properties deterministically,
      for example an x11-client mode that sets the transient and modal
      hints after mapping.
    - That step fails before the fix and passes after.
    - The modal-dialog checks pass 20 runs in a row under a loaded
      machine.
  - Done: the parent and the window type were never late: wlroots reads
    every property at the association, before the surface maps. The modal
    state was lost. At the map notify wlroots rewrites `_NET_WM_STATE`
    from fields it has not read yet, and the association comes after it,
    so a client that sets the hints and maps in one flush loses
    `_NET_WM_STATE_MODAL`; a client that waits on round trips between its
    hints gives wlroots time to select property changes first, which is why
    only a loaded machine failed. `X11View::handle_x11_map_request` reads
    the property at the map request, before the rewrite
    (`Server::x11_modal_requested`). A parent that arrives after the map
    lays the window out again (`set_parent` into `View::handle_kind_change`):
    a window that has become a modal dialog leaves the board and floats
    centred over its parent. wlroots emits nothing for the modal state alone,
    and the window type decides no kind, so neither has a listener; a modal
    state sent alone after the map waits for the next hint (the case study
    is `docs/intents/bugs/x11-modal-state-lost-at-map.md`).
  - Done: `x11-client --dialog` now sends the hints in the one flush that
    maps the dialog, and `--dialog-late` maps it bare and hints it on the
    second key; the xwayland scenario checks both. Without the fix the
    first failed 3 checks in each of the 4 runs made and the second 2
    checks; with it the scenario passed 20 runs in a row, other suites
    running on the machine.
- [ ] The modal state read at map does not wait on X (found, not started)
  - The problem: the modal-dialog fix reads `_NET_WM_STATE` at each X11
    map request with one blocking round trip on the window manager's
    connection (`Server::x11_modal_requested`). While Xwayland is slow or
    stalled, the compositor's loop stops: no input, no frames.
  - The fix: send the request when the map request arrives and take the
    reply when it comes, laying the window out again then (the same
    late-kind path that a late parent uses), or bound the wait by
    `config::x11_input_wait_ms`, as input to X already is.
  - Acceptance: a scenario that stalls the X server (SIGSTOP on
    Xwayland) while a dialog maps shows the compositor still answering
    IPC `state` within 100 ms. The modal-dialog checks stay green.

The shell and its plugins:

- [x] A shell.json that does not parse is never written over
  - The problem:
    - `Host.qml:86-95` falls back to `{}` when the file does not parse.
    - The next host write then replaces the user's file with one key.
      Writers include ThemeApi.qml:34-36, PluginApi.qml:69-73 and
      Host.qml:220-258, and even a theme change the compositor reports
      triggers one.
  - The fix:
    - `loadConfig` sets a `configValid` flag.
    - While it is false, `writeConfig` applies the change in memory,
      skips the file, and logs once.
    - A reload that parses clears the flag.
  - Acceptance: the bar scenario writes an invalid shell.json and flips
    the theme over IPC; the file stays byte-identical. Once the file is
    fixed, the next flip writes it.
  - Done: `loadConfig` sets `configValid` (false for text that is not a
    JSON object; a blank or missing file stays valid), and `writeConfig`
    applies the change in memory, skips the file and logs once while it
    is false. The bar scenario cuts shell.json in half, reloads the
    shell on it, flips the theme over IPC, and finds the file byte for
    byte as left, the bar light all the same. Once the file is fixed the
    next flips write `theme.mode` into it.
- [x] The Wi-Fi share QR keeps the password off the command line
  - The problem: `NetworkPanel.qml:602-603` passes `WIFI:…;P:<secret>;;`
    to qrencode as an argument. Arguments are world-readable in /proc, as
    the file's own comment at 222-223 says.
  - The fix:
    - Write the payload to qrencode's stdin, as the EAP process does at
      235-243 (`qrencode -t ASCII -m 4` reads stdin when given no
      string).
    - Escape `\ ; , : "` in the SSID and the password, as the WIFI:
      format requires.
  - Acceptance: the bar scenario's qrencode stand-in (add one to
    `tests/scenarios/bar-standins*` if missing) logs its argv and stdin.
    The secret appears only on stdin, and an SSID with `;` arrives
    escaped.
  - Done: the payload is built by `wifiField`, which escapes `\ ; , : "`
    in the SSID and the password, and goes to `qrencode -t ASCII -m 4` on
    its stdin (the process's stdin closes behind the write). The bar
    scenario's qrencode stand-in logs its argv as one line and its stdin
    as another: the stdin line carries the payload, and no argv line
    names the password; `Lounge;Guest` with the password `pa;ss:w"rd\x`
    arrives as `S:Lounge\;Guest;P:pa\;ss\:w\"rd\\x`.
- [x] Network names containing `:` are read right
  - The problem: nmcli's terse mode escapes `:` as `\:`, and
    NetworkPanel.qml:443-446 splits on every `:` without unescaping. An
    SSID containing a colon never matches.
  - The fix: split on unescaped colons only, and unescape `\:` and `\\`
    in every terse-mode parser of the plugin.
  - Acceptance: the bar scenario's nmcli stand-in lists an SSID
    `cafe:5G`; the panel shows it, with its band, as connected.
  - Done: `terseFields` cuts a line at unescaped colons and unescapes each
    field, `terseLines` and `terseUnescape` do the same for the
    single-value reads (profile, addresses, band, DNS, secret). The nmcli
    stand-in writes terse output with `:` and `\` escaped, and the
    NetworkManager stand-in roams the machine to `cafe:5G`: the panel
    shows it connected on 5 GHz, pins it by its own profile name and
    shares it as `S:cafe\:5G`.
- [x] Forget removes the network's own connection
  - The problem: "Forget" runs `nmcli connection delete <ssid>`
    (NetworkPanel.qml:568-575). A connection name can differ from the
    SSID, and the Network objects in use at 181-218 already have a
    `forget()`.
  - The fix: call `forget()`.
  - Acceptance: a bar step forgets a network whose connection name is not
    its SSID.
  - Done: `forget` calls the Network object's `forget()`. The stand-in's
    `OldCafe` profile is saved as "Old Cafe profile"; the bar step
    forgets it and finds the profile gone from the settings, with no
    `nmcli connection delete` in the tool log.
- [x] The agents widget reads its refresh setting
  - The problem: `Agents.qml:15-16` reads `api.settings`, which the API
    does not have.
  - The fix: read `api.plugin.settings.refreshIntervalSec`.
  - Acceptance: in the bar scenario, `refreshIntervalSec` = 2 makes the
    collector stand-in run again within 4 s.
  - Done: `refreshMs` reads `api.plugin.settings.refreshIntervalSec`, 900
    s before the api arrives. The bar scenario writes `refreshIntervalSec`
    = 2 into shell.json, watches the codex stand-in's initialize calls (one
    per collector run) every 100 ms and requires the second run within 4
    s of the first it sees.
- [x] The agents panel keeps the provider it shows when providers
  reorder
  - The problem: AgentsPanel.qml:18-23 tracks the chosen tab by position,
    while the collector sorts the providers after Claude and GPT by
    weekly tokens (agent_usage.cpp:1276-1296). After a refresh, the same
    position can show another provider.
  - The fix:
    - Keep a `currentId` and derive the position from it.
    - Look the provider logos up in a map instead of hard-coded ids
      (61-65).
  - Acceptance: a bar step whose stand-in swaps two providers between two
    refreshes keeps the chosen provider.
  - Done: the panel keeps `currentId` and derives the position from it,
    and a `marks` map by provider id gives the hero's logo. The bar
    scenario adds a day of gateway tokens between two refreshes, checks
    the collector's own order swapped Fireworks and the gateway, and
    finds on the screenshot the selection on the fourth chip and the star
    mark in the hero.
- [x] Notification settings apply at once
  - The problem: `NotificationsState.qml:36-41` copies `popupSeconds`,
    `criticalNeverExpires`, `maxPopups` and `historyLimit` once.
    settings.md says a change from the settings window applies at once.
  - The fix: bind them to `api.plugin.settings`, as Lock.qml:19-22 does.
  - Acceptance: a bar step changes `maxPopups` from the settings window;
    the next burst shows the new number.
  - Done: the four settings bind to `api.plugin.settings`, with the
    declared defaults before the api arrives. The bar scenario drags
    "Popups at once" to 2 in the settings window; the next burst of four
    notifications shows two cards.
- [x] Every panel and action gets `api` when it is created
  - The problem: Host.qml:555-560 assigns `api` after `createObject`,
    and so does ActionsRunner.qml:32. `BrightnessPanel`'s
    `Component.onCompleted: readSession()` (79) then runs without it,
    and the night-light row shows "Off" until the next settings event.
    Plugins guard `api != null` everywhere because of this.
  - The fix: pass `api` in `createObject`'s initial properties, and drop
    the null guards that only covered this.
  - Acceptance: a bar step opens the brightness panel; its night-light
    row shows the configured state at once.
  - Done: `createObject` takes `{api: …}` for panels (Host.qml) and
    actions (ActionsRunner.qml). The null guards of the panel and action
    components are gone. The bar scenario switches the night light on,
    reloads the shell, opens the brightness panel and finds the switch
    on.
- [x] One NotificationServer, not one per output (confirm first)
  - The suspected bug: `Popups.qml:43-56` makes a server in a perOutput
    surface, and `NotificationsState.qml:80-110` never de-duplicates.
    With two outputs, each notification would land twice.
  - Confirm with a two-output bar step. If it reproduces, move the server
    into the `NotificationsState` singleton so Popups only shows. If it
    does not, note that here and close the box.
  - Done, and it reproduced: with a second output up, one notification
    made two history entries. The server now lives in the
    `NotificationsState` singleton; Popups only shows. The bar scenario's
    two-output step finds one entry for one notification.
- [x] The logo's terminal outlives a bar rebuild (confirm first)
  - The suspected bug: the terminal is a child of the widget's `Process`
    (Logo.qml:13-21), so a widget rebuild would kill it.
  - The fix: `startDetached()`.
  - Done, and it reproduced: a shell reload closed the terminal. The
    logo starts it with `startDetached()`; the bar scenario opens one
    with a right click, reloads the shell twice, and finds it still
    there.
- [x] A plugin's number setting writes once per drag (confirm first)
  - The suspected bug: `PluginSettingRow.qml:52-66` writes shell.json and
    rebuilds the page on every slider move.
  - The fix: give PanelSlider a `committed` signal and write on it.
  - Done, and it reproduced: each move wrote shell.json and rebuilt the
    page, which destroyed the slider under the pointer and cut the drag
    short after two moves. `PanelSlider` has a `committed` signal, once
    per press (or per wheel step), and the row writes on it. The bar
    scenario drags the slider and counts one write of shell.json.
- [x] The wheel steps every bar control alike
  - The problem: `BarButton` hands on the raw `angleDelta`. Audio.qml:25
    rounds it to notches, while Brightness.qml:42 steps once per event,
    so a smooth-scrolling wheel races brightness.
  - The fix: BarButton emits whole notches, and both widgets step by
    them.
  - Acceptance: a bar step sends a 120-unit and a 15-unit wheel event;
    each control moves one step for the first and none for the second.
  - Done: `BarButton` adds the turn up and emits `wheeled(notches)`, one
    per 120 units, a half-done turn kept until it completes and dropped
    when the wheel turns back; Audio steps its volume by 0.05 and
    Brightness by 5 % per notch. The bar scenario starts a PipeWire
    daemon of its own with one null sink as the default and a
    `brightnessctl` stand-in holding one backlight (so the brightness
    widget is always there), sends 120-unit and 15-unit wheel events
    through the fake pointer, and finds one step for the first, none
    for the second, and one step for eight 15-unit events. Quickshell's
    volume is the cube root of PipeWire's channel volume, so the
    scenario reads the sink in that scale.
- [x] A click above a toolbar-anchored card closes it (confirm first)
  - The suspected bug: PanelCard's input mask (PanelCard.qml:60-64)
    starts at the card's top edge, so such a click would pass through
    to the window below.
  - Done, and it reproduced: a click above the card closed it, and the
    overview and its toolbar were gone too, the click having reached
    the canvas. The mask starts at the card's top edge, so the output
    above it was not the card's; a toolbar button's card now catches
    the whole output (the toolbar strip is below the panel layer). The
    bar scenario clicks above the open card and finds the card closed,
    the toolbar mapped and the overview active.
- [x] Touchscreen swipes respect the capture UI (confirm first)
  - The suspected bug: 3- and 4-finger touchscreen swipes start gestures
    while the capture picker is open. touch.cpp:120 checks only the lock,
    while the touchpad path drops them (input.cpp:1485).
  - Phase 6's UI-layer box removes the cause; until then, add the same
    check.
  - Done, and it reproduced: with the picker open, three fingers
    swiping right hopped to the next desktop and four swiping up opened
    the overview. `Touch::join_gesture` starts no swipe while the
    capture UI owns the pointer. The touch scenario opens the picker
    with Super+Shift+4, finds both swipes do nothing, closes it with
    Escape and finds the hop works again.

Packaging and tools:

- [x] greetd `set-default` changes only the session, and never the login
  screen itself
  - The rule, settled; write it into packaging.md's greetd row (88) and
    the tool's header (lines 22-23) first:
    - In `[default_session]`, when the command names the session with a
      flag (`--cmd X`, `--cmd=X`, `-c X`, `--command X`, `--command=X`,
      as agreety, tuigreet and gtkgreet take it): only X becomes
      `wayland-canvas`. X may be a quoted string. Every other byte of
      the command stays as it was.
    - When the command has no such flag and `[default_session] user`
      names a login account, it is a session started with no login
      screen: the whole command becomes `wayland-canvas`. A login account
      has a uid of at least `UID_MIN` (from `$root/etc/login.defs`,
      default 1000) in `$root/etc/passwd`, and is not 65534.
    - Any other command is a login screen that picks sessions from a
      list (regreet or gtkgreet in cage). It stays untouched; the tool
      says the screen lists wayland-canvas and picks it there.
    - `[initial_session]`, greetd's automatic login, gets
      `command = "wayland-canvas"` whenever it is present, as autologin
      does for SDDM and LightDM.
    - When nothing was changed, the tool exits 1 with that message, as it
      already does for a missing command.
    - A missing `user` means greetd's own greeter account, not a login
      account.
  - The bugs this fixes: `packaging/wayland-canvas-session:470-475` cuts
    everything from `--cmd` on (`${old%%--cmd*}`), so
    `tuigreet --cmd sway --remember --time` becomes
    `tuigreet --cmd wayland-canvas`; and a list-based greeter such as
    `cage -s -- regreet` is replaced by wayland-canvas run as the greeter
    account, so the machine boots with no login screen.
  - `greetd_set_command` (214-236) learns the section to write, and
    escapes `"` and `\` in the value for TOML. `restore` stays
    byte-exact.
  - The existing fake root `greetd-bare` (tests/test_packaging.sh:206-213,
    `command = "sway"` with no user) now expects no change and exit 1.
  - Acceptance: `tests/test_packaging.sh` gains fake roots, each restored
    byte for byte:
    - `tuigreet --cmd sway --remember --time` →
      `tuigreet --cmd wayland-canvas --remember --time`;
    - `agreety --cmd=sway` and `agreety -c sway`;
    - `cage -s -- gtkgreet --command 'sway --unsupported-gpu'` → the
      quoted value replaced;
    - `cage -s -- regreet` with `user = "greeter"`: unchanged, exit 1;
    - `sway` with `user = "marek"` and a passwd giving marek uid 1000:
      replaced;
    - an `[initial_session]` with `command = "sway"` beside a tuigreet
      `[default_session]`: both set.
  - The guide page that describes `set-default`, if one does, says the
    same.
  - Done: the tool reads `command` as shell words (quotes group, nothing
    is evaluated) and a TOML string with `\"` and `\\`; a value with any
    other TOML escape, or over several lines, is reported and never
    rewritten. The packaging suite has a fake root for each case above,
    and more: `--command=X`, a quoted value holding the flag's own name,
    TOML escapes written back, a literal string, no `user`, an unknown
    user, a system account, 65534, a `UID_MIN` from login.defs, and an
    `[initial_session]` beside a list-based screen. `status` also shows
    the initial session's command.
- [x] The flake's build finds what the compositor runs
  - The problem: flake.nix:45-62 has no ffmpeg and no `qs`, yet the
    compositor runs both by name (config.h:199, 231).
  - The fix: `wrapProgram` the installed binary with both on PATH.
  - Acceptance: `nix build` passes, and the wrapper's PATH names both.
  - Done: `makeWrapper` and a `postFixup` `wrapProgram` put `ffmpeg` and
    `quickshell` (which holds `qs`) on the installed binary's PATH.
    `nix build .#wayland-canvas` passes with its unit suite, and the
    wrapper's PATH names `quickshell-0.3.2/bin` (`qs`) and
    `ffmpeg-9.0.1-bin/bin` (`ffmpeg`). The first build also stopped at
    the link, `DSO missing from command line` for `xcb_get_input_focus`:
    `core_lib` calls xcb and never named it, so meson.build now lists
    `xcb` in its dependencies.
- [x] The autopilot hook tells "needs the user" by a marker, not a
  substring
  - The problem: autopilot-stop.sh:9 drops any line containing `user`,
    `deferred` or `waiting`, file names included. It hides open items
    that need nobody, such as touch-and-pen.md:65.
  - The fix: one end-of-line marker, `(the user)` or
    `(the user to decide)`, which check-plans.sh validates and the hook
    matches exactly. Fix the existing plans that use other wording.
  - Acceptance: the hook lists touch-and-pen.md:65 as workable, and
    check-plans.sh passes.
  - Done: `tools/plan-items.sh` lists every open box with its kind: `user`
    (the title, joined over its wrapped lines, ends in the marker),
    `stray` (any other wording of a wait) or `work`. The hook lists
    `work` and check-plans.sh fails `stray`, so the two read one rule.
    Ten plans were reworded to end in the marker. The hook now lists
    touch-and-pen.md:65 as work, and check-plans.sh passes. The autopilot
    and plan-checklists skills and the development guide say the same.
    `tests/test_agent_tools.sh` (meson suite `packaging`) drives the tool,
    the check and the hook on scratch plans.
- [x] Hook changes reach existing setups
  - The problem: agent-setup.sh:21 writes `settings.local.json` only
    when it is missing, and hooks are wired in two places
    (`.claude/settings.json` and the local file).
  - The fix: wire hooks in one place, and have agent-setup.sh update an
    existing file's hooks section.
  - Done: the hooks are wired in `.claude/settings.json` only, the
    tracked file. agent-setup.sh no longer writes `settings.local.json`
    or links `.claude/hooks`: it removes those links, and takes the hook
    groups it wrote out of an existing `settings.local.json`, leaving
    other hooks, other keys and the file's mode alone, and deleting the
    file when nothing else is in it (it says so when `jq` is missing).
    Run again it changes nothing; the agent-tools suite proves the old
    setup, a local file with other content, and the second run.
- [x] check-lock-pam.sh reads the lock plugin's manifest
  - The problem: lines 8-14 hard-code the manifest's path and its
    setting keys.
  - The fix: take both from `plugins/canvas.lock/plugin.json`.
  - Done: the script finds `plugins/canvas.lock/plugin.json` under the
    package root by that path with no prefix, and fails on none or more
    than one. `pamService` and `pamConfigDirectory` stay named in the
    script: they are the lock plugin's own setting names, the ones
    Lock.qml reads, so a rename stops the package build. A missing key
    or a manifest that does not parse is reported with the key or the
    reason and the manifest's path, not a traceback. Nine checks in
    `tests/test_packaging.sh` cover it.

Tests:

- [x] The bar scenario's early exits end the run, and every wait has a
  budget
  - Early exits:
    - Today `step = 99; return …` at bar.cpp:4140, 4220, 4430, 4457,
      4489, 4514, 4629, 4793, 6089, 6312, 6400, 6433, 6463, 6505 and 7443
      runs case 100, the config-watcher phase at 5255.
    - Give the driver a named end, a flag it checks rather than a step
      number.
  - Second waits:
    - The second wait inside a step resets `retries` on every re-entry,
      so it never times out: 2227-2251, 4450-4466, 4979-4993, 5131-5145,
      6058-6069 and 6531-6547.
    - Give each wait its own budget, as capture.cpp's `acted` flag does
      (50, 1353-1361, 1523).
  - Phase 8's harness generalises both fixes.
  - Acceptance: a check made to fail on purpose in a scratch build stops
    the run with a FAIL line; it never runs on, and it never hangs to the
    timeout.
  - Done: a step that cannot go on returns `end_run(why)`, which prints a
    FAIL line naming the reason and the line, sets `run_aborted` and
    leaves the next tick to print the summary: fifteen sites, the theme
    phase's skip among them. The seven second waits (the six listed and
    the agents panel's keyboard wait) count in `second_retries`, which
    the timer resets when the step moves. Scratch builds of the run with
    a check forced to fail: the exit at the bar-runs check ended the run
    in ten seconds with one FAIL line, where the old code ran on through
    the later phases; a keyboard check forced never to hold failed after
    its twenty polls and the walk went on, where the old code repeated
    the step until the timeout.
- [x] The scenarios check what their checks say
  - "Forwarded to the client" (gestures.cpp:879, 1577, 1582) tests only
    `holds_alternative<Forwarded>`, and no test client binds pointer
    gestures.
    - A test client (touch-client, or a small new one) binds
      `zwp_pointer_gestures_v1` and reports swipe and pinch events.
    - The checks read those reports.
  - "Clicking empty overview canvas" (635) and "the keyboard jump" (646)
    call `overview.exit()` and `goto_direction()` directly (630, 640).
    Drive them with the fake pointer and the fake keyboard.
  - Acceptance: each check fails when the path it names is broken, for
    example with the forwarding call removed in a scratch build.
  - Done: `tests/clients/gesture-client.c` (meson `gesture-client`, found
    through `CANVAS_GESTURE_CLIENT`) is a corner square on the overlay
    layer that binds `zwp_pointer_gestures_v1` and prints each swipe and
    pinch it receives. gestures.cpp starts it with the windows and, for
    the four forwarded checks (the 2-finger pinch at 100 % and at 25 %,
    the 3- and 4-finger swipes under `off`, the Super pinch with
    pinch-scale off), parks the fake pointer on it, drives the gesture
    through the device path and polls, with a budget, until the client
    has said exactly the begin, update and end lines. The swipes now come
    through the device path too. The overview click is a press and release
    of the fake pointer on empty canvas in the open overview, and the
    jump is Super+Ctrl+Right on the fake keyboard; both fake devices are
    made in step 0. The old click check passed without any click:
    restoring the window had already closed the overview, so the step
    now opens it again before it clicks. Scratch builds, each with one
    path broken: with every forwarding call a no-op the four client
    checks failed (the `holds_alternative<Forwarded>` checks still
    passed); with the hop binding a no-op the jump check failed; with
    the overview's empty-canvas press a no-op the click check failed.
- [ ] Exact checks wait for the state they check, not a fixed time
  - gestures.cpp:630-646 waits a fixed 700 or 600 ms and then requires
    the camera within 1 px, for a 280 ms animation (config.h:146).
  - Poll until the camera is no longer panning, with a budget; the same
    file already does this at 1562.
  - Apply the same to every fixed wait followed by an exact camera or
    pixel check: bar has 416 fixed returns against 298 polled, and
    gestures 149 against 38. List what was changed in the commit body.
  - Done in gestures.cpp: `camera_easing()` (panning or zooming), and
    the 23 steps whose first check reads where the camera ended, or takes
    the camera as a baseline, now poll on it with a budget before they
    act. Checks that nothing moved, and checks of a scale or a frame, stay
    after their fixed wait.
  - Left, in bar.cpp: its camera checks already poll (`camera_settled`,
    `overview_settled`), but 39 pixel checks still read a shot taken after
    a fixed wait, without a re-shot until the picture holds as the
    network panel's dots do (cases 3, 11, 44, 51, 118, 122, 126, 130, 141,
    145, 186, 187, 230, 241, 424 and 615, and the 23 steps that take the
    shot before a check).
  - Seen flaking on a loaded machine: `edge_checks`' "the strip is
    centred along its edge" fails right after `toolbar_on_edge` holds,
    and passes on a rerun. It checks the strip's place once instead of
    polling until the strip has settled.
  - Phase 8's harness (its `settled()` poll) and phase 10's bar split are
    where the bar part gets done.

### 6. One owner per fact: the compositor

- [x] The compositor's own UIs take input and draw in one order, from one
  list
  - Today the order lock → shortcut capture → capture UI → launcher →
    overview → bindings is written by hand in many places:
    - `process_key` (input.cpp:447-589);
    - the pointer paths (849-862, 1013-1020, 1145-1150);
    - the axis path (1430-1437);
    - touchpad gestures (1485, 1513);
    - touch (touch.cpp:120);
    - the tablet (1169-1172, 1201-1205);
    - `Painter::draw_scene` (render.cpp:1104-1114);
    - `Server::set_theme` (server.cpp:760-766);
    - teardown (server.cpp:438-443);
    - output destroy (output.cpp:462-463).

    The copies already disagree (the touchscreen box in phase 5).
  - Add `src/input/ui_layer.h`, an abstract `UiLayer` holding what
    Launcher and CaptureUi already offer informally:
    - `owns_keyboard`, `key`;
    - `takes_pointer_at(Output &, ox, oy)`, `pointer_motion`,
      `pointer_button`, `pointer_axis`;
    - `blocks_gestures`, `cursor_image`;
    - `theme_changed`, `output_gone(Output &)`;
    - one drawing contract that leaves the painter knowing neither class.
      CaptureUi's display list and the launcher's panel texture both fit
      `draw(const RenderCtx &)`.
  - Launcher and CaptureUi implement it, and Input holds them in priority
    order, the capture UI above the launcher.
  - Every input path asks one of `Input::layer_for_keys()`,
    `Input::layer_at(output, ox, oy)` or `Input::gestures_blocked()`.
  - `draw_scene` draws the same list bottom-up; set_theme, teardown and
    output destroy walk it.
  - What stays outside the list:
    - the lock stays first and separate, because it is the session and
      not a layer;
    - the overview keeps its per-output mode.
  - In `process_key`, the "symbols, then base symbols" scan written four
    times (513-526, 532-542, 557-573, 576-588) becomes one `KeySyms` with
    one helper.
  - Acceptance:
    - `git grep -nE 'launcher\.(is_open|key|pointer_)|capture_ui\.(owns_keyboard|key|pointer_|takes_pointer_at)' -- src/input src/render`
      prints nothing.
    - The launcher, capture and touch scenarios are green.
    - A touch step: a 3-finger touchscreen swipe with the picker open
      changes nothing.
  - Done: `UiLayer` (src/input/ui_layer.h) and `KeySyms`
    (src/input/key_syms.h) are in; `Server` hands Input `{capture_ui,
    launcher}`, and `process_key`, the motion, button, axis, touch and
    tablet paths, the touchpad gestures and Phase 5's touch swipe check
    ask `layer_for_keys`, `layer_at` or `gestures_blocked`. The painter,
    `set_theme`, teardown and output destroy walk the list. The touch step
    (a three-finger swipe with the picker open) is Phase 5's case 18-22 in
    touch.cpp, which fails with `gestures_blocked` forced to false. A
    scroll is a layer's where it takes the pointer, not wherever the
    capture UI holds the keyboard.
- [x] The theme switch happens wholly in `Server::set_theme`
  - Move the applications' light/dark preference (`theme_prefs::apply`,
    called from ipc.cpp:1035) into it, so every caller gets the whole
    switch.
  - Acceptance: the ipc scenario's theme.set check still sees the
    preference written; `theme_prefs::apply` has one caller.
  - Done: `Server::set_theme` calls `theme_prefs::apply` first, so it also
    runs when the mode is unchanged (the shell's start push); it is the
    one caller. A headless run is not a real session and never writes the
    preference, so the ipc scenario's theme.set checks see the palette
    switch and the reply, and test_theme_prefs holds the write itself.
- [x] One text cache and one cached-texture helper
  - Text is cached today by many owners, in different ways:
    - the Hint's cache is public and written by Painter (render.h:112-115,
      render.cpp:709-718);
    - `LabelImage` (render.h:168-176), Decoration (decoration.h:63-68)
      and Launcher (launcher.h:69-72) each keep a single scale slot;
    - only `HelpSlot` keeps several slots.
  - Because of the single slots, two outputs at different scales
    re-render the hint, the tab labels and the title bars on every frame
    they both repaint.
  - Add:
    - `render/text_cache.{h,cpp}` with
      `TextCache::get({text, font, fg, bg, scale})`, keeping a few slots
      per key;
    - `CachedTexture{get(scale, box, draw); invalidate()}` for the
      launcher panel and the capture UI. It replaces their 16
      hand-written `stale_ = true` + damage pairs (9 in launcher.cpp, 7 in
      capture_ui.cpp).
  - Every cache owner uses them, and `set_theme` drops all of them
    through one hook.
  - Acceptance: with two outputs at scales 1 and 2, no text texture is
    rebuilt across 100 unchanged frames (a counter in the cache, read by
    a scenario); the pixel checks stay green.
  - Done: `render/text_cache.{h,cpp}` holds `TextCache` (up to
    `config::text_cache_entries` texts, least recently asked for trimmed
    after each frame) and `Droppable`, and `render/cached_texture.h` holds
    `CachedTexture<Key>`, four slots per scale and key. The hint, the
    help line, the tab labels, the shelf titles and the picker's size
    label ask the cache; the launcher panel, the toolbar and the title
    bar are `CachedTexture`s. `Server` owns the cache,
    `Server::set_theme` calls `text_cache.clear()`, which drops every
    holder too, and the `theme_changed` hooks are gone. The gestures
    scenario adds a second output at scale 2 and counts 0 rebuilds over
    100 frames on both.
- [x] Box helpers live in one header
  - The copies:
    - `box_centre` (output.cpp:96, overview.cpp:17, canvas.cpp:30) and
      `centred_box` (three copies);
    - the box↔Rect conversions (canvas.cpp:21, view.cpp:13,
      render.cpp:49, desktop.cpp:93, 211);
    - the Rect→box roundings (overview.cpp:35, view.cpp:17,
      canvas.cpp:39, view.cpp:321-326).
  - They go into `src/core/box.h`, which may use wlroots types;
    geometry.h stays wlroots-free.
  - Keep each call site's rounding. Where two roundings really differ,
    make two named functions (say `box_rounded` and `box_covering`), and
    each site uses the one it uses today.
  - `id_of` (overview.cpp, output.cpp) moves to `canvas/desktop.h`.
  - Hand-written versions of wlroots helpers go:
    - `box_contains` (capture_ui.cpp:135, input.cpp:31) becomes
      `wlr_box_contains_point`;
    - launcher.cpp's `box_eq` becomes `wlr_box_equal`.
  - The six "which output shows this point" loops (view.cpp:45, 83, 143,
    307; canvas.cpp:131; fullscreen_desktops.cpp:23) become one Server
    query. If two loops really differ, the difference becomes a
    parameter, or that site stays as it is with the reason in the commit
    body.
  - The tests' copies of canvas.cpp:39-46 (gestures.cpp:284-290,
    input.cpp:129-135) use the header too.
  - Acceptance: each helper is defined once (grep), and the unit and
    scenario suites are green.
  - Done: `box.h` holds `rect_of_box`, `box_centre`, `centred_box` and the
    four roundings that really differ, `box_rounded`, `box_snapped`,
    `box_of_viewport` and `box_covering`; `id_of` is in desktop.h. The
    loops became `Server::output_showing`, with a `Showing::normal_view`
    parameter for maximize's skip of outputs in the overview.
- [x] One cairo and pango kit
  - `render/draw.h` exports:
    - `rounded_path`, identical today in launcher.cpp:71-84 and
      capture_ui.cpp:154-167;
    - `PangoLayoutPtr` and `FontDescPtr`, defined today in draw.cpp:10,
      launcher.cpp:23-24 and capture_ui.cpp:49-50;
    - one text painter with an anchor, replacing launcher.cpp:86-116,
      capture_ui.cpp:170-181 and draw.cpp's own;
    - a `hit(boxes, x, y)` helper, for the launcher's and the toolbar's
      hit-tests (`row_at` launcher.cpp:314, `toolbar_hit`
      capture_ui.cpp:714-727).
  - decoration.cpp:167-171 uses the kit too.
  - `launcher_pi` and `capture_pi` become `std::numbers::pi`.
  - Acceptance: one definition of each, and the pixel checks of the
    launcher, capture and gestures scenarios are green.
  - Done: draw.h holds `rounded_path`, `PangoLayoutPtr`, `FontDescPtr`,
    `make_layout`, `text_width` and `paint_layout`/`paint_text` with a
    `TextAnchor` (top_left, middle_left, centre); the launcher, the
    toolbar, the title bar and draw.cpp's labels and cards use them. The
    hit helper is `box_hit` in box.h, beside the other box helpers, and
    `toolbar_hit` uses it; the launcher's `row_at` indexes one uniform
    column by division and has no boxes to loop over, so it stays.
- [x] Each layout that drawing and hit-testing share is computed once
  - Launcher:
    - `PanelLayout` (launcher.cpp:40-63) gains the search, rows and
      footer boxes.
    - `panel_rows`, `draw_panel` and `row_at` (315, 389) read them,
      instead of each repeating `12 + search + 8 + … + 28`, `- 64` and
      `- 32`.
    - The paddings become `config::` constants.
  - Title bar:
    - decoration.h gets one `constexpr` list of the buttons and a
      `button_box(index, width)`.
    - `Decoration::update` (163-165, 174), `hit` (231-241) and the button
      positions (57-59) use them. Today the positions come from the order
      of the `DecoPart` enum.
    - The tests click through it too: gestures.cpp:414-419 uses
      `f.width - 10/50/100`, and input.cpp:482-526 uses `f.y + 16`.
  - Overview bottom:
    - One function returns the help-line, hint and shelf boxes.
    - The shelf stops assuming a 28 px help line (render.cpp:929-932).
      Today the hint stacked on the measured height can cover the shelf's
      title strip.
    - The tab sizes (891-905) are `config::` values, and the tab glyph
      offsets (514-524, 545) derive from the tab's box.
  - Acceptance: no copied literal sum remains (grep), and the launcher
    row, title-bar button and shelf clicks in the scenarios are green.
  - Done: `PanelLayout` moved to apps/launcher_layout.{h,cpp} so a
    scenario can click through it; it holds the card, search, rows and
    footer boxes, and the paddings are `config::launcher_pad`, `_gap`,
    `_footer_height` and `_margin_x/_y`. A row takes a click only inside
    the rows box, as it is drawn. decoration.h holds `deco_buttons`,
    `button_box` and `button_centre`; the title bar draws, hit-tests and
    sizes its title through them, and the gestures, input and xwayland
    scenarios click `button_centre`. `overview_bottom_layout` returns the
    help line, the hint's row and the shelf's bottom, so the shelf rests
    on the help line as measured. The tab and shelf sizes are `config::`
    values and the tab glyphs are centred in their buttons.
    `overview_press_target` already hit-tested through
    `overview_shelf_layout`. The gestures scenario restores its
    minimized window with a click on the shelf thumbnail, and the bar
    scenario's edge steps read `overview_bottom_layout`.
- [x] Each constant that two places keep is defined once
  - The overview's maximum zoom of 0.75 (gesture.cpp:284 and
    overview.cpp:231) becomes `config::overview_max_zoom`, or the
    gesture calls the overview's own zoom.
  - `config::cursor_size` replaces the bare 24 at server.cpp:553.
  - ipc.cpp's 1 MiB and 64 KiB (1547, 1750-1751) use config.h:209-210.
  - The scale clients are told comes from one `Output` function. Today
    output.cpp:55-60 and render.cpp:763-771 compute it with floors of 0
    and 1; pick the floor render/hidpi.md supports.
  - The plugin layer names the compositor recognises come from one
    function that both places read. Today ipc.cpp:209-262 knows the
    toolbar and the app bar, and render.cpp:573-603 only the toolbar.
  - Acceptance: each value is defined once (grep), and the scenarios are
    green.
  - Done: `config::overview_max_zoom` already names another bound (the
    fitted overview's, 0.30), so the 0.75 is `config::overview_zoom_limit`,
    read by the wheel and the pinch. The scale is `Output::sharper_scale`
    and `Output::tell_scale`, with no floor: render/hidpi.md says clients
    are told the scale of the outputs they are on, so a surface on a 0.5x
    output is told 0.5, not 1. The strip names are in layer.cpp behind
    `LayerSurface::is_overview_strip`; render's overview bottom now clears
    a bottom app bar as it did a bottom toolbar (overview.md and
    app-bar.md say so). No scenario puts the app bar on the bottom edge,
    so that clearance is not exercised. ipc.cpp's messages build "1 MiB"
    and "64 KiB" from the two config constants.
- [x] One settings table for every `[gestures]` and `[session]` key
  - Today each key is written out separately in each of these places:
    - settings.cpp: parse 550-611, resolve 674-703, setters 1015-1054,
      and the template 747-763, which restates settings.h:101-113;
    - ipc.cpp: `settings.get` 471-488, and `settings.set` 1294-1529 with
      its own validation at 1463-1492. Its usage text (1296-1297) leaves
      out three keys that it accepts.
    - main.cpp:44-51, whose help (82-87, 93-95) leaves out `[session]`;
    - the QML night-light sliders, which copy 1000/6500/4000
      (BrightnessPanel.qml:50-57; SettingsPanel.qml:351, 844-849).
  - Generalise `settings::Direction` and `directions`
    (settings.h:122-135) into one table of every non-action key. Each row
    gives:
    - the file section and key, and the get/set param name;
    - the kind: a choice with its names, a switch, or a bounded integer
      (with `off` for 0 where the key has that);
    - the default and the bounds;
    - the `Resolved` member.
  - These all read the table:
    - parse, resolve and `template_text`;
    - one `File::set(field, value)`, replacing `set_session` and
      `set_gesture` (1019-1050);
    - `settings.get`, and `settings.set`, which shrinks to a loop;
    - the usage text and `--help`.
  - `settings.get` reports each key's bounds and default. Both QML
    sliders read them from there, and become one kit `NightLightControl`.
  - `SwipePreset` is defined once, in settings.h:26; gesture.h:17 uses
    it, and the mapping at server.cpp:162-181 goes.
  - ipc.cpp's `swipe_name` and `swipe_preset` (388-414) and its `trim`
    (33) use settings' own (152-172, 25), exported.
  - Inside settings.cpp, each of these becomes one function: the chord
    lists joined three times (735-740, 897-902, 956-961), the duplicate
    checks done twice (946-953, 983-990), and the edit loop
    (903-906, 962-970, 996-998).
  - Acceptance:
    - A unit test walks the table. Every default round-trips through
      `template_text` → `parse` → `resolve`; every param name is unique;
      an out-of-bounds value is refused by `File::set` and by
      `settings.set`.
    - The ipc scenario and the bar scenario's settings-window steps are
      green.
    - `git grep -nE '5500|6500' -- plugins/canvas.settings plugins/canvas.brightness`
      finds no night-light bound.
- [x] An action and its runner cannot drift apart
  - The problem:
    - A registry row (settings.cpp:180-293) is joined to its runner in
      bindings.cpp (417-484, 582-595) by name, on every key press. A row
      with no runner silently passes its key to the client.
    - Which table a chord lands in depends on the display label
      `"Capture"` (bindings.cpp:551) and on the names `launcher` and
      `terminal` (545-549, 650-656, 689-697).
    - The rule that capture chords win is written three times:
      bindings.cpp:599 and 647, and capture_ui.cpp:510.
  - The fix:
    - Resolve each runner once in `Bindings::apply`, and log a row
      without one.
    - Give `ActionDef` an explicit field for where its chords apply,
      instead of the label, and make all three copies of the
      capture-chords-win rule read it.
  - Acceptance: a unit test checks that every registry action has a
    runner, and that renaming the group label changes only the label. The
    input and bar scenarios are green.
- [x] Small helpers written twice are written once
  - Each of these gets one definition:
    - `overloaded` (gesture.cpp:15, grab.cpp:20) moves to core/util.h;
    - `super_held` (gesture.cpp:22-27) uses `seat_modifiers`
      (input.cpp:36-42);
    - "the desktop under the cursor, else the target" exists twice and
      the copies differ. gesture.cpp:77-88 lacks the same-output check
      that 378-392 has; keep one, with the check.
    - the grab's anchor delta (grab.cpp:225, 236, 276, 341, 367);
    - the on-canvas surface scale (input.cpp:1025, 1057, 1188);
    - `1u << (button & 31)`;
    - the VT-switch loop (bindings.cpp:604, 623).
  - The title-bar cache key (decoration.h:65-70; decoration.cpp:139-143,
    189-194) becomes one struct with a defaulted `==`.
  - Hover and pressed change through one Decoration method that also
    damages. Today grab.cpp:139, 295, 440 and input.cpp:51 write the
    fields and damage by hand.
  - `visibility_dirty = true` plus a damage, set by hand 18 times across
    7 files, becomes one call.
  - The overview's target reset (overview.cpp:114-121, 183-190) is
    written once.
  - Acceptance: each is defined once (grep), and the unit and scenario
    suites are green.
  - Done: the title-bar key was already `BarState` with a defaulted `==`
    (the text cache phase); the grab's anchor is one `GrabAnchor` that
    every grab state holds; the desktop under the cursor is
    `Input::desktop_under_cursor`, which the render hover, the
    overview-finger abandon swipe and the overview pinch now share.
    Covered by the unit `bindings` test and the gestures, input, lock,
    touch, viewgone, xwayland and bar scenarios.
- [x] File, text and ownership helpers live in one wlroots-free place
  - `Handle` and `destroy_owned` move to a wlroots-free `core/handle.h`;
    util.h keeps `Listener` and includes it. agent_usage's hand-written
    `ParserDelete` (agent_usage.cpp:29-37, agent_usage_main.cpp:43-51)
    goes.
  - A wlroots-free `core/fs` and `core/text`, linked by the compositor
    and by agent_usage, hold:
    - `mkdir_p`, today in server.cpp:148 and capture_ui.cpp:68 with
      different error handling;
    - one atomic text write, which checks `fwrite` and `fclose` before
      the rename. It replaces server.cpp's version and agent_usage's
      `write_cache` (862-885), which is identical to `write_limits_cache`
      (1518-1541); neither checks fwrite or fclose today.
    - one JSON file reader, replacing `read_cache` (813-823) and its
      near-copy at 1474-1480;
    - `shell_quote` (server.cpp:184, capture_ui.cpp:89);
    - `ascii_lower` (desktop_entry.cpp, icons.cpp:14);
    - JSON string quoting (ipc.cpp:115; the copy at
      capture_ui.cpp:98-118 drops `\b` and `\f`);
    - one INI reader. Today there are three: desktop_entry's own parser,
      GKeyFile, and an fgets loop at icons.cpp:67-93. Keep the one that
      parses desktop entries to the spec, and use it everywhere.
  - agent_usage's raw `JsonBuilder *` and `char *` (record_json 1353, the
    cache writers) get handles. Text from `json_to_string` is freed with
    `g_free`, not `std::free`.
  - Acceptance: each is defined once, and the unit suite plus the ipc and
    capture scenarios are green.
- [x] The control socket answers late calls one way
  - The capture UI stops writing wire JSON (capture_ui.cpp:416-431, and
    the `PickUse::ipc` mode at 353-356) and hands Ipc a typed pick
    result.
  - The three deferred replies become one `Reply` type, which a method
    may answer now or later. Today each has its own pending slot
    (ipc.h:150-159), `answer_*` function (829-838, 869-887, 1226-1250) and
    cleanup branch in `~Client` (568-582).
    - `handle_line`'s if-chain for the four deferred methods (757-769)
      goes, so every method is a row of one table.
  - The other copies:
    - the `{x, y, width, height}` object is built in one place
      (281-291, 300-310, 337-344, 1240-1249);
    - the scope text "global"/"overview" (448, 853-856, 1332-1338, 1511);
    - the hand-written JSON type checks (788, 966, 1019, 1098, 1261,
      1360, 1404, 1473, 1957) use the `opt_`/`req_` helpers (164-201);
    - the msg client's two round trips (1842-1864, 1925-1948) become one
      function with an owned fd (`close(fd)` is written 12 times).
  - Acceptance: the ipc, capture and sharing scenarios are green.

### 7. One owner per fact: the shell and its plugins

- [x] Plugins see only the documented API
  - The problem:
    - `api.shell` is the Host item (Host.qml:418), and `api.plugin.host`
      exposes it too.
    - So a plugin can call `writeConfig()`, or take another plugin's api
      through `api.shell.plugins[i].api`.
    - Toolbar.qml:46-57 and 473, and AppBar.qml:30-41, already use
      host internals.
  - The fix:
    - Add `shell/ShellApi.qml` holding exactly the `api.shell` members of
      plugins.md, plus the app-bar edge and switch that canvas.settings
      uses (add those to plugins.md:137).
    - Drop `host` from PluginApi, and move the two plugins onto
      documented members.
    - Delete Toolbar.qml's fallback for a host without `api.shell`.
  - Acceptance: `git grep -nE 'api\.plugin\.host|api\.shell\.plugins' -- plugins`
    prints nothing, and the bar scenario is green.
- [x] The two overview strips are one kit component
  - Toolbar.qml and AppBar.qml each write their own copy of:
    - the strip window (AppBar 211-282, Toolbar 216-298);
    - the slide (192-202, 109-133);
    - the chrome and label chip (306-400, 302-417);
    - the overview flag (157-168, 96-107);
    - the placement beside the strip (AppBar 415-434, Flyout.qml:31-60);
    - the 44 px square (Toolbar.qml:41, AppBar.qml:26).

    The copies already differ: only the app bar clamps its chip, and
    their corners differ.
  - Move them into the UI kit as an `EdgeStrip`, with its chrome and a
    `placeBeside()` helper that PanelCard's seven clamps (110-150) use
    too.
  - Each strip's edge and default come from `api.shell`; Host.qml:119-133
    stays the one parser. Toolbar.qml:59-67 and 148-174, and
    AppBar.qml:43-69, go.
  - The host drops invalid toolbar buttons at load, so Toolbar.qml:76-94
    and its copy at AppBar.qml:174-190 go.
  - StripLayout takes the length each strip publishes, instead of each
    strip recomputing the other's (StripLayout.qml:13-34).
  - Acceptance: the bar scenario's toolbar and app-bar steps are green on
    every edge. In a nested run, check both strips on each edge
    (on screen).
  - Done: `EdgeStrip` holds the strip window, the slide, the overview
    flag, the chrome and label chip (clamped for both strips), the 44 px
    square and `placeBeside()`, which PanelCard's clamps and the flyout
    use. Toolbar.qml and AppBar.qml keep only their squares. The edge
    comes from `api.shell`, the host drops invalid toolbar buttons, and
    StripLayout reads each strip's published `length`. The host-writes
    walk waits for the reload that putting the app bar back on its edge
    makes before it starts.
    A nested run with three windows open showed both strips on nine edge
    pairs (each edge shared, and right/left, left/right, top/bottom,
    bottom/top, right/top) and none after Escape.
- [ ] Each manifest fact is stated once
  - The host passes each panel its name when creating it. Today 11
    panels restate it as `panelName` (e.g. AgentsPanel.qml:15-16), while
    Host.qml:561 already has it.
  - A bar widget entry gets an optional `description`, passed on by
    `Host.widgetEntry`. SettingsPanel.qml:976-977 then stops naming
    canvas.agents.
  - Tray.qml:46-51 toggles its panel by its local id, and its unused
    `panel` property (15) goes; TrayState stops duplicating the model the
    panel manager passes.
  - Each note colour is declared once, and the toolbar menu is built from
    that list. Today plugin.json:8-15 and 26-73 and NotesState.qml:376
    hold copies, which NotesState.qml:65-92 reconciles.
  - Acceptance: `git grep -nE 'canvas\.[a-z]+/' -- plugins shell` finds
    only a plugin's own names inside its own folder, and the bar and
    notes scenarios are green.
  - Left open: TrayState stays. The panel box below stops the panel
    manager passing the anchor, which leaves TrayState the one owner of
    the menu's item. The tray widget keeps its `panel` property, which
    now carries the manifest's panel name. The rest is done and green.
- [ ] Every panel reads its state from its plugin, wherever it was opened
  from
  - The problem: the panel manager hands a panel the item that opened it
    (`model = anchor`, PanelsManager.qml:48-49). Several panels read
    `model.*`: NetworkPanel.qml:26-32, BrightnessPanel.qml:12 and 23-24,
    AgentsPanel.qml:21 and 35-36.
  - So:
    - a panel opened by an action or from another plugin is empty;
    - per-output widgets hold their own copies: two bars hold two
      brightness values, and the weather is fetched once per output
      (Brightness.qml:39, 47-64; Weather.qml:18-33).
  - The fix: give each of these plugins a singleton state that its
    widgets and its panel read, as NotificationsState, LockState and
    TrayState already are. The panel manager stops passing the anchor as
    the model.
  - Acceptance, in a bar scenario with a second headless output:
    - a wheel on one bar's brightness shows on the other;
    - the weather is fetched once;
    - each of these panels opened through `api.panels.open` from an
      action shows its data.
- [x] The host rebuilds only what a write changes
  - The problem:
    - `rebuildRegistry()` runs on every host write (Host.qml:193, 223,
      237, 251, 259, 613), though only bar.layout, bar.hidden and
      toolbar.order feed the registry.
    - Bar.qml:37 and 66-82 then recreate every widget on every output.
  - The fix: rebuild on those three keys only, and have the bar keep
    widgets by name across a rebuild.
  - Acceptance: in the bar scenario, moving the toolbar to another edge
    leaves a clock panel's toggle state and the weather fetch count
    unchanged.
- [x] Compositor.qml keeps its state from one event table
  - The problem:
    - Five events update the state it keeps (110-128), but
      `capture.recording` (ipc.md:67, 141) is not among them.
    - Pending callbacks are never failed on a disconnect (66-77).
    - `message.error` reaches callers in three shapes (103; also
      AppBar.qml:494).
  - The fix: one table from event to state update, and every in-flight
    call failed with one error shape on disconnect.
  - Acceptance, in the bar scenario:
    - a recording started over IPC lights the recording widget;
    - when the shell's socket drops, its callbacks get an error.
- [ ] Facts copied inside a plugin are stated once
  - NetworkPanel's DNS providers come from one list. Today they are
    written out in setDns (517-550), classifyDns (742-755), dnsClicked
    (552-558) and four pills (1006-1033).
  - SettingsPanel:
    - its tabs come from one `{id, label, component}` table. Today the
      tab list (436-454) and the Loader chain (458-463) must agree.
    - `setGesture` and `setSession` (295-307, 356-368), and the six
      three-part error checks, become one function.
  - Token counts are formatted in one place. agent_usage.cpp:912-922
    gives "1.0M" where AgentsPanel.qml:452-464 gives "1M".
  - Acceptance: the bar scenario's network, settings and agents steps
    are green.
- [ ] The UI kit covers what the plugins rebuild by hand
  - Add `PanelButton`, a trailing slot on `PanelRow`, `PanelSwitchRow`,
    `PanelMeter`, `PanelPill` and `UiTheme.radius` (PanelRow.qml:32, 39;
    ChoiceRow.qml:23, 30).
  - Fix PanelCard: the fallbacks of 1280, 720 and 26 (84-88) read
    `UiTheme`, and the clamp written seven times (117-160) becomes one
    function.
  - What goes:
    - about 20 controls anchored by hand with `rightMargin: 8`;
    - 13 PanelSwitches placed on a PanelRow with three anchor lines each;
    - the hand-made buttons (Dialog.qml:167-211, Popups.qml:160-187,
      ClockPanel.qml:46-47, ShortcutRow.qml:87-88);
    - FlyoutRow's own hover row (20-21);
    - PowerPanel's own hero (78-110), where `PanelHero` exists;
    - NetworkPanel's private Pill and HeroButton (1366-1430), and its
      literal block offsets (937, 966, 993, 1002, 1039, 1324);
    - AgentsPanel's hand-drawn meters (193-207, 261-277) and its ten
      hand-styled `Text` blocks (e.g. 131-145);
    - dim `PanelText` used where `PanelSectionHeader` exists;
    - the dialog scrim colour (Dialog.qml:80), which comes from the
      theme.
  - Acceptance:
    - `git grep -c 'rightMargin: 8' -- plugins` finds none.
    - The bar scenario's screenshots are green.
    - A nested run looks unchanged (on screen).

### 8. One owner per fact: tests, build and tools

- [ ] One scenario harness, proven on the small scenarios
  - The copies:
    - All 12 scenario files and `tools/media/tour.cpp` define their own
      `check`, `mkdir_p`, `handle_timer`, `run_step` driver and `main`;
      10 define `shot` and `key`, and 9 define `click`.
    - Larger shared blocks are copied too: the fake devices
      (capture.cpp:492-522 and input.cpp:169-198), the raw IPC client
      (input.cpp:259-295 and ipc.cpp:167-203), the child clients
      (touch.cpp:106-133 and xwayland.cpp:106-133), and the private D-Bus
      world (bar.cpp:363-376 and 611-624, session.cpp:118-131 and
      150-163, tour.cpp:181-330).
  - The copies differ: only capture's waits have a budget, only the
    tour's reap sends SIGTERM first, and there are three different
    bus-readiness waits.
  - Create `tests/harness/` as a static library in meson, holding:
    - the driver, with named steps and named results (after N ms, again
      after N ms, go to a named step, finish), documented once;
    - a per-wait budget;
    - a `settled()` poll for the camera and other animations;
    - `check` and `check_near`, printing the failure's location;
    - scratch dirs under `CANVAS_TMP`;
    - `shot` and PNG reading;
    - the fake keyboard, pointer, touchscreen and tablet;
    - the IPC client, using the compositor's own `write_request`
      (ipc.cpp:521-534) from the core library;
    - child processes and the private bus;
    - layout helpers such as `deco_point(view, part)`, built on phase 6's
      title-bar list, and `bar_mid_y()`.
  - Port lock and nightlight to it.
- [ ] The rest of the scenarios on the harness
  - Port launcher, session, sharing, touch, xwayland, capture, ipc, input
    and gestures, a few files per commit, each run green.
  - The 14 call-site explanations of the return protocol ("without the
    advance") go.
  - Literal points become layout helpers: bar.cpp's 21 clicks at y = 13
    beside its `bar_h()` (1346), and `click(60.0, 700.0)` written five
    times.
- [ ] The media tour on the harness
  - Port `tools/media/tour.cpp` (its copies of the startup and bus code
    match session.cpp:171-191 and bar.cpp:363-376).
  - Acceptance: `tools/media.py` still renders the stills.
- [x] The unit tests share one `tests/check.h`
  - Today seven `check` functions come in five shapes.
    test_settings.cpp:19-31 is nearly test_theme_prefs.cpp:22-29.
    test_geometry prints only the location, test_session_bus the
    function, and test_session_services neither.
  - `tests/check.h` provides `check`, `check_near` and the summary line.
- [x] The test clients share their Wayland boilerplate
  - `say()` (gamma-client.c:40, lock-client.c:48, touch-client.c:78,
    x11-client.c:34) and the memfd shm buffer (lock-client.c:60,
    touch-client.c:95) move into `tests/clients/client-common.h`.
- [ ] meson.build states each list once, and says what it skipped
  - The seven unit-test blocks (297-378) differ only in name, library and
    deps; they become a `foreach` over a dict.
  - The core dependency list (214, 224, 478, 504; the copies already
    differ) becomes one `declare_dependency`.
  - `CANVAS_TMP` is set once, not at 320, 390 and 510.
  - Bar and session need qs, dbus-daemon, notify-send and the stand-ins
    (446-458).
    - A scenario that cannot run is registered anyway, as a test that
      exits 77 and names what is missing.
    - `summary()` lists them.
    - So `tools/check.sh` never reports scenarios as passed when they
      did not run.
  - Acceptance: `meson test -C build --list` shows every scenario, and
    without qs the bar scenario shows SKIP with the reason.
- [x] The packages match their binary
  - PKGBUILD:79 and build-deb.sh:56 say wlroots is linked statically.
    That holds only when meson falls back to the subproject. Pass
    `--force-fallback-for=wlroots` in both builds (package-arch.sh,
    build-deb.sh).
  - Decide Xwayland support from wlroots' own `have_xwayland`;
    meson.build:75-79 uses `xcb.found()`.
  - `make-source-tarball.sh` runs `fetch-wlroots.sh` itself; today only
    CI does (packages.yml:49, 79).
  - build-deb.sh's `/tmp/cxx26-probe` uses `mktemp` with a trap.
  - package-arch.sh:22 sets `PACKAGER` only when it is unset.
  - Acceptance: `tools/package.sh` builds both packages here, and `ldd`
    on each packaged binary shows no libwlroots.
- [x] Both package builds run to the end on this machine
  - Found by running `tools/package.sh` for the box above: makepkg's
    `-fexceptions` made `core/wlr.h` compile pthread.h (via xwayland.h's
    xcb.h) under `#define class class_`, so the Arch build failed; the
    Debian root filesystem has no `/dev/fd`, which `check-lock-pam.sh`'s
    process substitution needs. `wlr.h` includes xcb.h first and the script
    reads `find` into a variable.
- [x] The tools share one preamble
  - Add `tools/lib.sh` with `repo_root`, `use_venv`, `die` and
    `usage_from_header`.
  - Today's copies:
    - the repo root is found three ways (package.sh:16-17, build.sh:10,
      the hooks);
    - the venv block is written three times (build.sh:33, check.sh:33,
      package-arch.sh:17);
    - the `sed '2,/^$/p'` usage prints `set -euo pipefail` as help
      (install.sh:23).
  - install.sh:
    - its per-format facts move into one `select_format`. Today they
      are spread over eight `case` blocks (49-58, 79-83, 89-98, 106-109,
      116-121, 154-157, 169-172).
    - it takes the artifact package.sh reports, instead of guessing by
      mtime (107-110).
  - The session tool ships standalone and cannot source lib.sh; fix its
    `usage()` (wayland-canvas-session:40) in place.
  - `check-plans.sh` and the hooks run in strict mode like every other
    tool.
  - Acceptance: `tests/test_packaging.sh` is green, and each tool's
    `--help` prints its header and nothing else.
  - Run here: `tools/lib.sh` is sourced by every shell tool; `package.sh`
    ends with `package.sh: built <path>` lines and install.sh reads them;
    the packaging suite copies lib.sh beside the tools it copies and checks
    every tool's `--help` against its header.
- [x] media.py and the tour share one manifest
  - Today `STILLS` (media.py:30) repeats tour.cpp:1170-1189, along with
    the 1600×1000 size (media.py:156-157, tour.cpp:49, 53) and the
    output names. media.py:119-130 also re-implements the stand-in
    handshake of meson.build:448-462.
  - The fix: the tour writes `out/manifest.json`, and media.py reads it.
  - Acceptance: `tools/media.py` runs end to end, with the stills
    unchanged.
  - Run here: the stills came out at the same 1920×1200 and look the same
    (SSIM 0.978-0.999 against the committed ones); the WebP bytes differ.

### 9. Single responsibility: the compositor

- [ ] `Server` holds the wlroots globals, the object lists and the
  subsystems, nothing else
  - `Xwm` (`src/windows/xwm.{h,cpp}`) takes the X window manager, and is
    the one caller of `View::as_x11()`:
    - `x11_offset_*`, `x11_stack_`, the settle ask;
    - `sync_x11`, `x11_follow_pointer`, `x11_changed`, `x11_settled`,
      `x11_give_up`, `x11_put_pointer`, `x11_ask_settled`;
    - the Xwayland setup and its handlers (view_x11.cpp:272-478).
  - `CanvasSurfaces` (`src/platform/canvas_surfaces.{h,cpp}`) takes the
    plugin surfaces on the plane: `canvas_places`, `canvas_stack`,
    `raise_canvas_surface` and `named_surfaces` (ipc.cpp:1067-1079).
    - It offers place and unplace by name, add, remove, raise and
      `bottom_up()`.
    - Today ipc.cpp:1146 and layer.cpp:237 both write the public map and
      only Ipc erases (1160). So an unplace from anywhere else leaves a
      place behind that comes back at the next map.
  - `ConfStore` (`src/core/conf_store.{h,cpp}`) takes canvas.conf:
    - `conf_path`, `conf_text`, `conf`, `conf_errors`;
    - `load_conf`, `write_conf`, the inotify watch and
      `handle_conf_change`;
    - the create-and-watch block of `Server::start` (594-622), which
      calls `config_home()` four times.
  - `Focus` (`src/windows/focus.{h,cpp}`) takes focus and stacking:
    - `focused_view`, `focused_layer`;
    - `focus_view`, `focus_in`, `focus_keyboard`, `return_keyboard`,
      `refocus_layers`;
    - `top_visible_view`, `last_minimized_view`, the minimize counter;
    - Server's focus and hit-testing code in view.cpp (911-1023).
  - `SessionEnvironment` (`src/platform/session_environment.{h,cpp}`)
    takes the bus hand-over, the services the session starts and stops
    (server.cpp:212-300, 624-640) and `manages_services_`. Its destructor
    hands the bus back.
  - `Options`, `now_ms` and `spawn` move out of server.h, to core/options.h
    and core/process.h. Files that need only those stop including
    server.h; launches.cpp:7 includes it for `now_ms` alone.
  - Acceptance:
    - server.h declares no member defined outside server.cpp and its
      subsystems' files, and its `// output.cpp` location comments are
      gone.
    - cpp-standards.md's "One per compositor" row names the new
      subsystems.
    - The scenarios are green.
- [ ] `Input` is the seat and the dispatch, nothing else
  - Like `Touch` and `Tablets`, these become classes of their own in
    `src/input/`:
    - `SuperTap` (input.cpp:710-773);
    - `ShortcutCapture` (615-708);
    - `XInputGate`, the X wait queue (1291-1368), which Touch and Tablets
      use too;
    - `PointerRouter`, the pointer routing and the implicit grab
      (945-986);
    - `DragAndDrop` (1598-1628).
  - `cursor_image` becomes private (input.h:121).
  - `lock_session` cancels five pieces of Input state
    (lock.cpp:116-120); that becomes one `Input::abandon_interaction()`.
  - Acceptance: input.cpp holds device setup and dispatch only, and the
    scenarios are green.
- [ ] `View` holds the window, nothing else
  - Placement (view.cpp:28-246) moves to `windows/placement.cpp` as free
    functions over `View &`: `place`, `place_over_parent`, `centre_in`,
    `cascade`. The cascade's 32 and 64 become `config::` values.
  - Foreign-toplevel mirroring moves to a `ForeignToplevel` member with
    one `publish(const View &)`, called once per change. Today it is
    619-717, plus 18 `set_*` calls spread through the setters; the
    wlroots setters are idempotent.
  - `view_update_outputs` (output.cpp:47-94) and `Server::reveal_view`
    (output.cpp:349-374) move beside the view.
  - Acceptance: view.cpp holds the view's state and its setters, and the
    scenarios are green.
- [ ] `Ipc` is a transport, with methods registered by their owners
  - Split ipc.cpp's 1986 lines into:
    - `platform/ipc_server.{h,cpp}`: the socket, framing, clients,
      events, and `register_method(name, fn)` with phase 6's `Reply`;
    - `platform/json.h`: the json-glib wrappers (72-201);
    - `platform/ipc_state.cpp`: the serializers (203-489);
    - one method file per area, each registering its own methods
      (state, windows and desktops, settings, capture, surfaces,
      actions);
    - `platform/ipc_client.cpp`: the `msg` CLI (1772-1986);
    - the xdg-desktop-portal chooser bridge (`capture_pick_bridge`) in a
      file of its own.
  - Ipc stops including the capture UI, the launcher and the theme; those
    subsystems include `ipc_server.h`.
  - Ipc reads per-frame change flags, as it already does with
    `windows_dirty`, so domain code does not call `ipc.emit_*` directly
    (desktop.cpp:230).
  - `emit_desktops` stops setting `windows_dirty` (1633-1636), and
    `run_action`, an alias of `emit_action` (1681-1683), goes.
  - Acceptance: a new method is one `register_method` call in its owner,
    and the ipc scenario is green.
- [ ] `Painter` draws frames; the overview's chrome belongs to the
  overview
  - The drawing primitives (render.cpp:19-227) move to
    `render/pass.{h,cpp}`.
  - The tabs, the shelf, the help line, their layout and their labels
    (render.cpp:511-730, 865-941) move to
    `canvas/overview_chrome.{h,cpp}`. Input hit-tests through it instead
    of `server.painter.overview_tab_layout` (input.cpp:217-245).
  - `Hint` moves to `render/hint.{h,cpp}`, with its cache private.
  - Acceptance: `src/input` includes nothing from `src/render` except the
    drawing types, and the scenarios are green.
- [ ] `settings.cpp` is one file per job
  - `core/chord.{h,cpp}`: chords (63-373).
  - `core/settings.{h,cpp}`: the registry, phase 6's table, parse,
    resolve and the template.
  - `core/settings_file.{h,cpp}`: `File` and its edits.
  - Acceptance: the unit suite is green.
- [ ] The usage collector: one table of sources and one of families
  - Sources:
    - A `SourceKind{name, files(Paths), scan(path)}` table replaces the
      hard-coded roots (agent_usage.cpp:1215-1225).
    - It also replaces the three ways a scanner is chosen: by
      `bool codex` (1204, 1235), by path equality (1233), and by a path
      prefix (1241-1242).
    - agent_usage_main.cpp's per-source `Paths` lines (417-424) read the
      table.
  - Families: `family_of` (1005-1039), `family_chip` (1041-1053),
    `family_title` (1055-1076) and the `{"claude","gpt"}` order (1276,
    1283) become one `families[]` table.
  - `collect` (1191-1350) is split into `scan_all`, `aggregate`,
    `provider_order` and `make_provider`.
  - Correct the false comment at 630-631 ("immutable flag"), and remove
    the unused `uri` (629).
  - Acceptance: adding a source is one table row, and
    `test_agent_usage` is green.
- [ ] `Output` is the output, nothing else
  - `handle_frame` (output.cpp:411-438) runs the server-wide flushes on
    every output's frame: fullscreen reconcile, visibility, the window
    list, cursor rebase and X11 sync. They become one
    `Server::flush_pending()`, run once per frame.
  - `handle_destroy` tells six subsystems through four verbs (451-493);
    that becomes one `output_gone` fan-out.
  - The output bit allocation, split today between 132-139 and 499-506,
    lives in one place.
  - Acceptance: the scenarios are green, including a second output
    unplugged mid-gesture.
- [ ] `Canvas` owns the board rules
  - The problem:
    - `desktops` is a public member that other code changes directly
      (canvas.h:52-53).
    - `Desktops::remove` un-tiles windows through a callback
      (desktop.cpp:234-248), and `Desktops::move` moves tiled windows
      (261-285).
    - `Overview::abandon` and `combine` (overview.cpp:246-282) touch no
      overview state, and bindings.cpp:204-213 calls `abandon` outside
      the overview.
  - The fix:
    - Desktops becomes a container of frames.
    - `Canvas::remove_desktop`, `move_desktop`, `abandon` and
      `combine_nearest` own every effect on windows.
  - Acceptance: the gestures scenario is green.
- [ ] Hand-mirrored code becomes one function called twice
  - `layer_configure` (layer.cpp:24-99) mirrors its x and y blocks
    (36-63) and also tracks exclusive zones (71-98). Split it into
    `place_on_axis()`, called twice, and `reserve_zone()`.
  - The border half of `Decoration::hit` (244-285) writes four edge
    strips and four corners by hand, mixing `<=` (248, 261, 265) with
    `<`. Make one per-axis `edge_band()` with one comparison rule.
  - `Launcher::key` (launcher.cpp:559-641) is a 41-branch keysym chain,
    each branch re-testing Ctrl and both letter cases. Make it a
    `{sym, ctrl, action}` table.
  - Acceptance: the layer, title-bar and launcher checks in the scenarios
    are green.
- [ ] No include cycles between subsystems
  - Input⇄Lock (input.cpp:28, 447; lock.cpp:116-120) and Input⇄Ipc
    (grab.cpp:411; ipc.cpp:863, 1222) call each other.
  - One-way facts are passed by constructor, or read through phase 9's
    Ipc change flags.
  - Acceptance: an include-graph check (a short script under tools/,
    run by check.sh) finds no cycle among the subsystem headers.

### 10. Single responsibility: the shell, plugins and scenarios

- [ ] Host.qml composes six parts
  - The parts:
    - `ShellConfig`: paths, read, the only writer `set(path, value)` with
      phase 5's guard, and the watch.
    - `PluginDiscovery` with `Manifest.js`: discover, listed,
      manifestProblem and skip.
    - `Contributions`, a kinds table `{key, validate, stage}`.
      - A plugin is committed all or nothing. Today panels register
        mid-loop (555-561): a plugin whose second panel fails is skipped,
        but its first panel stays.
      - A `globalName()` helper replaces eight `plugin.id + "/"` copies.
      - One `orderByConfig()` serves both widgets and buttons.
      - The dead `toolbarButtons` branch (499-504) goes.
    - `ShellApi`, from phase 7.
    - `SurfaceHost`: surfaces made per screen through `Variants`, so a
      hotplug no longer rebuilds every screen's surfaces (718-732).
    - `ReloadWatcher` (734-824).
  - Commands run as argument lists, not through `/bin/sh` with the
    host's own quoting (55-62, 331-340, 429-438, 803-811).
  - Acceptance:
    - The bar and notes scenarios are green.
    - Adding a headless output keeps the first output's bar widgets
      (their state survives).
- [ ] The network plugin: a service and four views
  - `NetworkService.qml` takes the twelve processes (NetworkPanel.qml:
    235-695) and their parsing (359-490, 643-667). It sits in the
    plugin's state singleton from phase 7, so the bar widget can show the
    band and the DNS.
  - The views become `BandSection`, `DnsSection`, `ShareView` and
    `SpeedView`, with a qmldir. The formatters (697-800) go in a JS file.
  - Acceptance: the bar scenario's network steps are green.
- [ ] The settings window: a model and one file per tab
  - `SettingsModel.qml` holds the IPC client, the model and
    `set(params, faultKey)`.
  - Each tab is a file, listed in phase 7's tab table.
  - Acceptance: the bar scenario's settings steps are green.
- [ ] Notes: editing, the file format and placement each in their own
  file
  - The text-formatting code (NoteSurface.qml:146-300, 605-627) moves to
    `NoteEditor.qml`.
  - The HTML encoder and parser (NotesState.qml:278-388) move to
    `noteFile.js`.
  - Acceptance:
    - The notes scenario's pixel checks are green.
    - A qmltestrunner test round-trips `noteFile.js` (mixed spans, empty
      paragraphs, no font family or colour kept), if qmltestrunner is
      installed; otherwise a node or qml6 script under `tests/`.
- [ ] The bar scenario: one file per phase on the harness
  - Today `bar.cpp` is 9488 lines, with one `run_step` of 5124 lines and
    about 77 file-scope mutable variables.
  - It becomes one file per phase (the walk, the theme, the watcher,
    notes, network, agents, settings, and so on) with named steps.
  - A phase can run alone, through an env var naming it, so debugging a
    late step no longer means running them all.
  - The unreachable `return 400;` (8085) goes.
  - Acceptance: the bar scenario is green as a whole and phase by phase.

### 11. State shapes and dead code

- [x] Touch's recognizer is a variant
  - Today `press_`, `two_` (five doubles) and `swiping_` (four fields)
    are mutually exclusive only by convention (touch.h:64-67, 102-108),
    and `begin_swipe` clears the others by hand (touch.cpp:351-357).
  - Make them a `std::variant<Idle, Press, TwoFinger, Swipe>`.
  - Acceptance: the touch scenario is green.
- [x] CameraMotion is a variant
  - Today it is a kind enum plus shared fields (camera.h:33-43), and its
    end-of-path rule is written twice (camera.cpp:75-81, 122-129).
  - Make it a `std::variant<Still, Pan, Zoom>`, with the end rule written
    once.
  - Acceptance: the unit and gestures suites are green.
- [ ] A window's mode is one enum
  - Today `maximized` and `fullscreen` are two mutually exclusive bools.
    `set_maximized`, `set_fullscreen` and `set_frame` (view.cpp:431-592)
    repeat the restore-from-saved block (463-469, 525-531) and clear the
    other mode by hand (440-449, 497-505, 564-585).
  - The fix:
    - an `enum class WindowMode`, with one `set_mode(mode)` and
      `area_for(mode)`;
    - one restore;
    - the public setters stay as one-line calls.
  - Acceptance: the input and gestures scenarios are green.
- [ ] A window's board seat and its moves have one shape
  - `board` and `slice` (view.h:156-158) must agree, and four places
    clear them as a pair. Make them a `std::optional<BoardSeat>`.
  - `saved` is shifted by hand (canvas.cpp:418-419,
    fullscreen_desktops.cpp:36-37). Add `View::translate(dx, dy)`.
  - The `if (view.tiled)` guards at canvas.cpp:358, 365 and 399 go:
    `set_tiled` already returns early.
  - Acceptance: the gestures scenario is green.
- [x] The gesture machine has no unchecked corners
  - `state_output`'s catch-all (gesture.cpp:48-61) silently drops a
    forgotten state. Use a generic lambda the compiler checks, like
    `grab_output` (grab.cpp:39-44).
  - `begun_` and `Pending::options` are two copies of one snapshot
    (184, 193); keep one.
  - The `ZoomOut` and `ZoomIn` states mirror each other (257-286,
    312-327); make one `Zoom{bool entering}`.
  - These become `config::` constants, each with a one-line reason:
    - the 0.8/0.2 smoothing, written three times;
    - the ±0.5 velocity threshold (314, 323);
    - `/400`, and `/200` at input.cpp:1441.
  - Acceptance: the gestures scenario is green.
- [x] Window kinds are read through their predicates
  - fullscreen_desktops.cpp:17 and view.cpp:727 compare
    `ViewKind::client_placed` directly, though view_kind.h:3-8 says
    every rule goes through its predicates.
- [x] Dead code goes: the compositor
  - `draw_item_card`, `scale_rgba` and `wrap_layout` (draw.cpp:22-41,
    106-133; draw.h:17-21), and `config::item_*` (config.h:110-117).
  - `MethodFn` (ipc.h:78-81), and `Request::ok` (ipc.cpp:691, 744) if it
    is still there.
  - `have_scale` in main, which only repeats `scale > 0`.
  - Acceptance: grep finds none of these names, `-Wunused` is clean, and
    the unit suite and the scenarios that cover the files changed pass.
- [ ] Dead code goes: the shell and plugins
  - `CanvasApi.surfaceName` (CanvasApi.qml:17-19).
  - `registry.barWidgets` (Host.qml:615).
  - Compositor.qml's `available` (14).
  - Notes.qml's unused `registry` and `modelData` (12-14).
  - Network.qml's unread `networks` (20-21).
  - Acceptance: grep finds none of these names, and the bar and notes
    scenarios pass.
- [x] Every build has Xwayland; configure fails without it
  - The compile-time variant without Xwayland, its second build tree and the build script's option for it are gone: X11 apps are always supported, the packages bundle wlroots with Xwayland, and a missing Xwayland program is the runtime case.

### 12. Comments

- [ ] No banner separators
  - These go: about 140 `// ----` lines in 22 files of `src/`, plus 11 in
    NetworkPanel, 13 in SettingsPanel, 6 in Toolbar, 29 in bar.cpp, 7 in
    tour.cpp and 5 in wayland-canvas-session. code-style forbids them.
  - Most of them mark concerns that phases 4 and 9-10 moved into files of
    their own; delete the rest.
  - Acceptance:
    `git grep -nE '^\s*(//|#) ?(-{3,}|={3,})' -- src shell plugins tests tools packaging`
    prints nothing.
- [ ] No block comments
  - desktop_entry.cpp:87-91, Toolbar.qml's 23-line header (7-29) and
    Host.qml:783-794 become one-liners stating behaviour. Host.qml's
    block also narrates how a fix was proven.
- [ ] No false or stale comments
  - Correct or delete each of these, and any the splits leave behind:
  - In the compositor:
    - config.h:6-7 (keybindings in input.cpp);
    - settings.h:47 and settings.cpp:176 (a "settings contract §3" that
      no doc has);
    - ipc.cpp:75 (the reason for `GErrorHolder`), 164 (it belongs to
      `opt_string`) and 701 ("NUL-terminated copy", though the length is
      passed);
    - render.h:68 (`output->camera()`) and 152-155 (a member that is
      gone);
    - overview.h:35 ("zoom-1 camera", but it uses `resting_zoom()`);
    - desktop.h:18-19 (against 70 and desktop.cpp:268-270);
    - canvas.h:19 (its "64" repeats `box_views_max`), 21 (it checks the
      board first) and 30 (it does not say what `axis` and `dir` mean);
    - gesture.cpp:601 (a pan state that does not exist);
    - icons.cpp:10-11 (it describes `DirType`, which is in icons.h).
  - In the shell and plugins:
    - Compositor.qml:28-30 ("exactly one response, in request order");
    - Host.qml:486-489 and 495-496 (the same fact twice), 607-610 (false
      for `registry`) and 620-623 (misplaced and wrong);
    - Toolbar.qml:229-237 (anchored to one edge, but it anchors two);
    - Popups.qml:43-44 ("one per shell");
    - SettingsPanel.qml:424-425;
    - NoteSurface.qml:545 (cryptic);
    - Lock.qml:147-148 (told as history).
  - In the tests and tools:
    - meson.build:485 ("200-odd steps", "step 90");
    - tests/scenarios/README.md:263 and 276, and session.cpp:5 (old bus
      paths, against session.cpp:785 and bar.cpp:9204);
    - test_packaging.sh:42 (`run` for `run_check`);
    - bar.cpp:2209-2211 (the walk's phase list).
  - Acceptance: a reviewer pass (`docs/roles/reviewer.md`) over the
    diff, and `grep -niE 'used to|previously|no longer|was changed'`
    over `src shell plugins tests tools` finds no history.
- [ ] No comment restates its line
  - These go:
    - render.cpp:1049 (`// clear`), 1053, 1071 and 1076, and 549-553,
      which repeats 531-532;
    - server.cpp:133, 147 (`// mkdir -p.`) and 394;
    - launcher.h:69-71;
    - gesture.cpp:347-348, and gesture.cpp:22, which excuses a copy that
      phase 6 removed;
    - input.h:118;
    - ToolbarButton.qml:53;
    - NetworkPanel.qml:1396;
    - tour.cpp:209;
    - AppBar.qml:223-230 (the same fact twice).
- [ ] The non-obvious places get their one line
  - Add a line explaining each of these:
    - why `button & 31` is safe (input.cpp:953);
    - why the 4 px top strip is not divided by zoom while the margin and
      the corner are (decoration.cpp:221);
    - why `fullscreen_area` is called twice (view.cpp:508-509);
    - the tab sizes, and the per-character width guess if a measured
      width does not replace it (render.cpp:879, 891-905);
    - why the brightness glyph depends on the screen count
      (Brightness.qml:17);
    - each `config::` constant phase 11 added.
  - The models for tone: geometry.cpp:125, camera.cpp:19-22,
    render.cpp:25-27 and input.h:205-206.

### Verify

- [ ] `tools/check.sh` green after the last phase
- [ ] The sanitizer suites of phase 3 run clean again after phases 9-11
- [ ] A nested run (on screen owed) covers:
  - each strip edge;
  - the launcher;
  - a window shot and a recording of a wayland window and of an X11
    window;
  - a lock and unlock;
  - a resize by an edge and by Super+right-drag, of both kinds of window;
  - a second output.
- [ ] On the TTY, the night light warms the panel, and a recording keeps
  the desktop responsive (the user)
