---
name: verification
description: How to actually verify a change in wayland-canvas before calling it done - tools/check.sh, the unit suite, the headless scenarios (real compositor, real foot clients, grim, fake devices), nested and TTY runs for on-screen feel - plus the hygiene every test run must keep (kill only your own PIDs, self-quitting runs, nothing left in the tree) and the traps of scripted runs. Use before reporting any fix or feature as complete, before ticking a plan box, and before and after starting any compositor process.
---

# Verification

Compiling means the change did not break the build. `tools/check.sh`
passing means it did not break what is already covered. Neither means
the change does what was asked.

## Trace a fix to its real endpoint

A fix that writes the right state can still leave what the user sees
unchanged. Trace forward from the code you changed:

- Who reads this state, and when? Every frame in the frame loop, on the
  next commit, only on the next configure?
- Does it cross a protocol? Then the client sees it only in what the
  compositor sends — a configure, a frame callback, an enter/leave. A
  value correct internally can be invisible to the client.
- Is there a cache in between (the title-bar texture cached in the
  view, the camera quantized to the pixel grid, the output mask)?

Ask "then what happens?" one step past the code you touched. Don't
claim completeness from "it runs": compare against the reference
(`tinywl` and `wlr_scene` for protocol behaviour, Windows 11 for the
title bars and launcher).

## Venues

All tools live in `tools/`; scenario sources in `tests/scenarios/`.

| Venue | Command | Proves |
|---|---|---|
| Everything | `tools/check.sh` | Builds and runs all three suites: unit, packaging, scenarios |
| Unit | `meson test -C build --suite unit` | The pure canvas math (direction choice, tiling, zoom paths) and desktop-entry parsing |
| Packaging | `meson test -C build --suite packaging` | The default-session tool against fake roots, one per supported display manager: set-default then restore byte-identical, no-op restore, idempotent set-default, missing previous session restored; the package checks; the plan tools, the autopilot hook and `agent-setup.sh` against scratch copies |
| Scenarios | `meson test -C build --suite scenarios` | The real compositor on the headless backend: real foot clients, gestures injected through the same entry points libinput events reach, keyboard and pointer driven through fake devices announced to the backend, screenshots with grim via screencopy, wtype for the virtual keyboard |
| On screen, nested | `build/wayland-canvas -s foot` from a terminal inside another compositor | On-screen feel without leaving the session; the host may swallow gestures it binds, a lone Super tap passes through |
| On screen, TTY | `timeout 15m build/wayland-canvas -s foot` from a spare TTY | The real backend: DRM/KMS, real touchpad gestures, the 2x panel |
| Packages | `source .venv/bin/activate && tools/package.sh` | The release build of every target in both package roots: makepkg's flags (with `_FORTIFY_SOURCE`, `system()` and friends warn when their result is dropped), a Debian root holding only the declared build dependencies, and in each package the lock screen's shipped PAM settings naming a service file the package installs |

What each kind of change needs:

| Change | Minimum |
|---|---|
| Any code | The build, plus the unit tests and the scenarios that cover the files changed (`meson test -C build <name>`); `tools/check.sh` once per batch of work, before a push, a pause or a merge; `tools/package.sh` before the work is called warning-free |
| Input, gestures, canvas, overview, window management, launcher | + the scenarios that cover it |
| Protocols, anything a client depends on | + a scenario with a real client (foot, grim) |
| Rendering, title bars, the overview UI | + an on-screen run and a look |
| A restructuring | The suites that cover the moved code; behaviour must not change |
| Ownership, lifetimes, buffers | + `tools/check.sh --sanitize` |
| Dependencies, build options, `meson.build`, `packaging/` | + `tools/package.sh` |
| Comments only | No test run: the diff with comments stripped is empty |

A run is repeated only when something it covers changed since it last
passed: a full suite run after a commit that touched one area proves
nothing the area's own tests did not.

Every run is read whole: its log stays in `tmp/` until it has been read,
and an exit code or a `grep -c warning:` is not a reading — a configure
`ERROR:` or a compiler warning passes straight through such a filter. A
failure reported against a command is reproduced with that command first
(the `debugging-discipline` skill).

## On-screen feel

- A scenario screenshot shows layout; it does not show feel. **A nested
  or TTY run is the only proof of on-screen feel.**
- What cannot be shown that way (real touchpad hardware, the 2x panel)
  is owed: it is a plan box that says so, and only the user can close it.
- After a change to what is drawn, look at the screen (or the grim
  capture) and show the user the picture.

## Hygiene for every run

- **Kill only your own PIDs.** Never `pkill -f wayland-canvas` or kill
  by pattern: other sessions run test compositor instances from the
  same build folder, and the host compositor must never be touched.
  Kill by the PID you started; `pgrep -fl wayland-canvas` afterwards to
  confirm nothing of yours is left.
- **Runs quit themselves.** TTY runs start under
  `timeout 15m ...` because a hung compositor holds the keyboard and VT
  switching would not save you; nested runs are closed from their own
  session. A hard kill by PID is the fallback, not the plan.
- **Nothing temporary stays in the tree.** Debug prints and test
  commands are removed before the turn ends; another session may
  commit them.
- **Scratch.** Logs, captures and screenshots go under the repo's
  `tmp/` (the `scratch-space` skill); the tools write there by
  themselves, `CANVAS_TMP` overrides for one run.
- **Displays don't collide.** A nested or scenario run gets its own
  Wayland display; never point `WAYLAND_DISPLAY` at the host's or
  another session's.

## Traps in scripted runs

- A client's surface appears asynchronously: wait for the map before
  driving keys or gestures at it.
- An interrupted tool call never runs its kill step: `pgrep` before and
  after.
- grim hangs forever if the compositor renders only on its own damage
  and ignores `needs_frame` — the case study is
  `docs/intents/bugs/screencopy-never-returned.md`.
- A nested host binds some 3/4-finger gestures itself; a gesture that
  "does not arrive" in a nested run is the host's, not the
  compositor's, until shown otherwise.
- Device-level injection (fake keyboard/pointer devices through the
  backend) takes the real path and finds what calling handlers
  directly does not — prefer it for input changes
  (`docs/intents/bugs/grabs-that-never-ended.md`).

## Reporting

Say what was run and what it showed, with the log line or picture that
shows it. Say what was not run and why ("not seen on screen"). A box is
ticked for what was verified, not for what was edited.
