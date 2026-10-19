# Install, build and run

Getting the compositor onto a machine and onto a screen: release packages,
your own packages, the default session, building from source, the ways to
run it, and the socket that controls it while it runs.

## Install a release

Each [release](https://github.com/marek/wayland-canvas/releases/latest) has
a package for Arch and Arch-based distributions (`.pkg.tar.zst`) and one for
Debian and Ubuntu (`.deb`). Download yours from the latest release and
install it:

```sh
sudo pacman -U wayland-canvas-*.pkg.tar.zst   # Arch and Arch-based
sudo apt install ./wayland-canvas_*.deb       # Debian and Ubuntu
```

Then pick wayland-canvas at the login screen, or make it the default
session (below).

## Build your own package

The same two packages, built from the checkout (`.pkg.tar.zst` with
makepkg, `.deb` inside a rootless Debian root filesystem):

```sh
tools/package.sh              # both formats, artifacts in dist/ (--arch, --deb
                              # build just one; ninja -C build packages too)
tools/install.sh              # builds the package for your distro, installs it
                              # with the system package manager, and asks
                              # whether to make wayland-canvas the default
tools/install.sh --uninstall  # restores the previous default, then removes it
```

## The default session

`wayland-canvas-session status | set-default | restore` shows or changes the
default session later; `restore` puts every display-manager file back byte
for byte. SDDM (autologin and the remembered last session), GDM
(AccountsService), LightDM and greetd are covered; on anything else the tool
changes nothing and says how to start the compositor. Removing the package
directly (`pacman -R`, `apt remove`) restores the default as well.

## Build from source

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

The terminal is `$TERMINAL` (falls back to `foot`).

## The control socket

Control a running compositor: `wayland-canvas msg <method> [<params as JSON>]`
over `$WAYLAND_CANVAS_SOCKET` (set for every client it spawns): `state`,
`subscribe`, `launcher.toggle`, `overview.toggle`, `theme.set`,
`surface.place`, `surface.unplace`, `surface.move`, `surface.resize`,
`window.focus`, `action.run`, `capture.pick`, `capture.stop`,
`settings.get`, `settings.set`, `shortcut.capture`, `shortcut.cancel`,
`quit`. `wayland-canvas -h` lists them;
[../intents/platform/ipc.md](../intents/platform/ipc.md) is the protocol.

## See also

[Concepts](concepts.md) for what you will see on first run, and
[screenshots and screen sharing](capture.md) for the capture side of a running
compositor.
