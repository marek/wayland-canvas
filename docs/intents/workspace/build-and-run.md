# Build and run

> Owns the build, its dependencies, and the ways to run the
> compositor. The test venues are owned by `AGENTS.md` and the
> `verification` skill.

## Requirement

A clean checkout builds and runs with two commands and no manual
fetching: wlroots arrives by itself when the system does not have it.

## Behavior

### Dependencies

A C++26 compiler — LLVM's clang, selected by
`toolchain/llvm.ini` ([../platform/cpp-standards.md](../platform/cpp-standards.md))
— meson ≥ 1.3 and ninja; the wlroots 0.20 dependencies
   (wayland, wayland-protocols, libdrm, gbm/EGL/GLES2, libinput, libseat,
   xkbcommon, pixman, libdisplay-info, hwdata); cairo and pango
   (decorations, launcher); librsvg and glib (icons); json-glib
   (the control socket); sqlite3 (the usage store the bar reads). Optional: `qs` (Quickshell) for the autostarted
   top bar ([../shell/bar.md](../shell/bar.md)).

### Build

| Subject | Rule |
|---|---|
| Commands | `tools/build.sh` is the one command a clean checkout needs: it takes meson and ninja from PATH when they run there and from the repo's `.venv` otherwise, configures through the native file — setting the build dir up again when it was configured with another compiler, by a checkout at another path, or is no longer a whole meson build dir, and reconfiguring one another meson version set up — and compiles. A directory meson did not make is left alone. By hand it is `meson setup build --native-file toolchain/llvm.ini` and `ninja -C build`; `meson.build` stops with that command when another compiler is configured |
| Compiler | `toolchain/llvm.ini` selects clang for the compositor and the wlroots subproject: `-std=c++26 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic`; libstdc++ here, libc++ where installed; the system linker (`lld` once installed, set in the native file) |
| venv fallback | `tools/venv.sh` pip-installs `requirements.txt` (meson and ninja, unpinned: the latest) into a `.venv` at the repo root (git-ignored) and prints its bin dir. A venv made at another path (its scripts name that path's Python), from another `requirements.txt`, or whose meson or ninja does not run is made again. `tools/build.sh`, `tools/check.sh` and `tools/package-arch.sh` take meson and ninja through it whenever none run on PATH |
| wlroots | the system `wlroots-0.20` when installed; otherwise meson fetches wlroots 0.20.2 per `subprojects/wlroots.wrap` into `subprojects/wlroots/` (ignored) and links it statically with only what this compositor needs — the GLES2 renderer, the DRM and libinput backends, session on, Xwayland and libliftoff off ([../platform/wlroots.md](../platform/wlroots.md)) |
| Protocol header | the layer-shell protocol header is generated at build time from `protocols/wlr-layer-shell-unstable-v1.xml` |
| Unit tests | `meson test -C build`; the scenario suite and everything at once run through `tools/check.sh`, which configures through the native file and re-creates a build dir that was configured with GCC |
| Packages | `tools/package.sh` builds both formats; `ninja -C build packages` / `package-arch` / `package-deb` are the same thing through meson. Artifacts land in `dist/` at the repo root (ignored) ([packaging.md](packaging.md) owns the formats and the guarantees) |

### Running

| Mode | How | Notes |
|---|---|---|
| From a TTY | `./build/wayland-canvas` | DRM/KMS via libseat/logind. Use a spare TTY: a hung compositor holds the keyboard, so VT switching cannot save you — wrapping a first run in `timeout` (for example `timeout 15m`) is the habit |
| Nested | the same command from a terminal inside another compositor | renders at 1x ([../render/hidpi.md](../render/hidpi.md)); the host may swallow 3- and 4-finger gestures it binds itself, and a lone Super tap passes through ([../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)) |
| Screen sharing | apps share through the screen-sharing portal ([../platform/protocols.md](../platform/protocols.md#screen-sharing)): a login shares wayland-canvas's own screens; a nested run shares through the host's portal, where the canvas is the host's window to pick | `dbus-run-session -- ./build/wayland-canvas` gives a nested run a portal of its own, as a test only — the apps in it lose the session's keyring (saved passwords and logins) and its other services |
| Startup command | `-s <cmd>` | run at startup, for example `-s foot` |
| Scale | `-S <scale>`, or `$WAYLAND_CANVAS_SCALE` | default by the dpi rule: 2 on dense panels, else 1 ([../render/hidpi.md](../render/hidpi.md)) |
| Debug | `-d` | debug logging |
| Without the bar | `-B` | start without the autostarted top bar; the scenario suites always do |
| Control socket | `wayland-canvas msg <method> [<params as JSON>]` | the methods, events and errors [../platform/ipc.md](../platform/ipc.md) lists, over `$WAYLAND_CANVAS_SOCKET`; `msg subscribe` prints each event as one line |
| Install | `meson install` | the binary and `shell/` (under `share/wayland-canvas/`) |

The terminal binding (`Super+Return`) and `Terminal=true` desktop
entries run `$TERMINAL`, falling back to `foot`.

Everything the compositor starts — the startup command, the terminal,
launcher entries, the shell — runs through `/bin/sh -c` in a session of
its own: `WAYLAND_DISPLAY` names the compositor's socket,
`XDG_CURRENT_DESKTOP` is `wayland-canvas:wlroots`, the variables
that would steer a client to another compositor (`DISPLAY`,
`HYPRLAND_INSTANCE_SIGNATURE`, `SWAYSOCK`, `I3SOCK`, `NIRI_SOCKET`) are
unset, the signal mask is empty, and SIGPIPE is as the compositor found
it — the compositor ignores SIGPIPE for itself (a control-socket client
may leave mid-write), and that ignore never reaches what it starts.

A single-instance app that is already running in another session — a
browser or an Electron app open in the host compositor of a nested run,
or in a session on another VT — hands the launch to that instance, and
the window opens there. Such an app starts here only once that instance
has quit, or with a profile of its own (Chrome's `--user-data-dir`).

## Cross-links

- [repos.md](repos.md) — what is committed and pushed.
- [../platform/cpp-standards.md](../platform/cpp-standards.md) — the
  toolchain and the C++ rules.
- [../platform/wlroots.md](../platform/wlroots.md) — the wlroots
  version and subproject rule.
- [../render/hidpi.md](../render/hidpi.md) — scale and crispness.
- [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)
  — the terminal binding and nested-mode input notes.
