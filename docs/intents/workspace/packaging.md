# Packaging

> Owns the distribution packages, the session file, the default-session rules
> per display manager, and the install/uninstall guarantees. Building in
> general is owned by [build-and-run.md](build-and-run.md).

## Requirement

Two package formats come out of the build system — a `.pkg.tar.zst` for Arch
and Arch-based distributions and a `.deb` for Debian and Ubuntu — and one
installer flow asks whether wayland-canvas should be the default session.
Removing the package, through the installer or with a plain `pacman -R` or
`apt remove`, always leaves a working login: any default this package set is
restored before the session file disappears.

## Packages

| Subject | Rule |
|---|---|
| Contents | the `wayland-canvas` binary, `wayland-canvas-agent-usage` and `wayland-canvas-session` in `bin`, `wayland-canvas.desktop` in `share/wayland-sessions`, `wayland-canvas-portals.conf` in `share/xdg-desktop-portal`, the top bar's Quickshell config in `share/wayland-canvas/shell` and its plugins in `share/wayland-canvas/plugins`, the session target `wayland-canvas-session.target` in the systemd user unit directory ([session-services.md](session-services.md)), the lock screen's PAM service as `sysconfdir/pam.d/wayland-canvas` ([../shell/lock.md](../shell/lock.md)), the licence (`/usr/share/licenses` on Arch, `/usr/share/doc/wayland-canvas/copyright` on Debian) |
| Build commands | `tools/package.sh` builds both formats, `--arch` / `--deb` one of them; `ninja -C build packages`, `package-arch`, `package-deb` call the same scripts. Artifacts land in `dist/` at the repo root, whoever built them |
| Arch build | `makepkg` over a tarball of the local source tree (the wlroots subproject included, `.git` stripped), built by the same meson native file as a normal build, installed with `--skip-subprojects=wlroots` because wlroots is linked in statically |
| Runtime dependencies | every installed binary decides them, not just the compositor: the Arch `depends` names sqlite because the agent-usage helper links it, and `dpkg-shlibdeps` is run over each ELF under `usr/bin` |
| Debian build | inside a rootless Debian root filesystem downloaded from the official image registry, cached under `tmp/dl/`, entered with user namespaces (`unshare --map-root-user --map-auto`); build dependencies installed there by apt; `Depends` computed by `dpkg-shlibdeps` from the built binary |
| Debian release | the one whose toolchain builds the C++26 library features the compositor needs (`std::function_ref`, `std::inplace_vector`, `std::print`); the in-root build probes exactly those three features before compiling, so an unsuitable `--dist` override fails early |
| Prompts | never from the packages themselves: pacman forbids prompts in install scripts, and the Debian maintainer scripts stay non-interactive. `tools/install.sh` is the one flow that asks, for both formats |
| Removal hooks | Arch `pre_remove` and Debian `prerm` run `wayland-canvas-session restore`; Debian `postrm purge` removes `/var/lib/wayland-canvas` |
| Tag builds | a `v<version>` tag — or a manual run — builds both formats on GitHub Actions and drafts a release from them (below) |
| X11 runtime | optional: xorg-xwayland is `optdepends` on Arch and `Recommends` on Debian — X11 applications run through it ([../windows/x11.md](../windows/x11.md)), and the package is complete without it |
| Top bar runtime | optional: quickshell is `optdepends` on Arch and `Recommends` on Debian (in sid) — the compositor starts the bar when `qs` is on PATH (`-B` turns it off), and the package is complete without it ([../shell/bar.md](../shell/bar.md)) |
| Screen sharing runtime | optional: xdg-desktop-portal-wlr is `optdepends` on Arch and `Recommends` on Debian — apps share screens through it ([../platform/protocols.md](../platform/protocols.md#screen-sharing)), and the package is complete without it |

## Release builds

A pushed tag named `v<version>` builds the release: one job per format,
each in its distribution's own container (`archlinux:base-devel`,
`debian:sid`), calling the same `packaging/` scripts the local flow calls.
The container *is* the environment `tools/package-arch.sh` and
`tools/package-deb.sh` reproduce on a contributor's machine, so neither
makepkg's source tarball nor the rootless root filesystem is rebuilt there:
`tools/package.sh --here` skips both: makepkg runs in place as the image's
`nobody` (it refuses to run as root, and no account is ever created), and
`packaging/debian/build-deb.sh` runs in the container itself.
`--here` needs root for apt and says so when it does not have it.

| Subject | Rule |
|---|---|
| Trigger | a pushed `v*` tag, or a manual run from the Actions tab; nothing on a push to a branch |
| Tag and version agree | `tools/ci-check-tag.sh` fails the run before anything is built when the tag is not `v` plus the `meson.build` version |
| wlroots | `tools/fetch-wlroots.sh` checks the wrap's revision out before the source tarball is made, so the package build itself needs no network for it |
| Artifacts | the two packages only — `.pkg.tar.zst` and `.deb`, collected from `dist/`. No source archive: the packages are the release |
| Release | a tag drafts a GitHub release carrying both packages; publishing it stays a human step |

There is no CI beyond this: correctness is proven by `tools/check.sh` run
locally ([build-and-run.md](build-and-run.md)), and the workflow exists to
build and publish artifacts, not to gate commits.

## The session file

`wayland-canvas.desktop` (`Exec=wayland-canvas`, `DesktopNames=wayland-canvas;wlroots`)
is installed to `share/wayland-sessions`, which SDDM, GDM and LightDM list for
their Wayland sessions. greetd does not read session files; it starts whatever
command its config names (below).

## The default session

`wayland-canvas-session` has three commands — `status`, `set-default`,
`restore` — and a `--root` prefix so every rule is testable against a fake
root. It detects the display manager and changes only that manager's
configuration; on anything else it changes nothing and says how to start the
compositor (from a TTY, or picked at the login screen).

Detection: the `/etc/systemd/system/display-manager.service` symlink first;
without it, the configuration that exists only when a manager is in use, most
specific signal first: greetd's `config.toml`, an AccountsService user file
(GDM), lightdm's config, sddm's config.

What "the default" means per manager:

| Manager | set-default changes |
|---|---|
| SDDM | `Session` in `[Autologin]` of the config file that enables autologin (nothing when no autologin user is configured), and `Session` in `[Last]` of `/var/lib/sddm/state.conf` — the greeter's pre-selection — unless `RememberLastSession` is false |
| GDM | `Session` in `[User]` of `/var/lib/AccountsService/users/<user>` — the pre-selected and the automatic-login session; `<user>` is `--user`, else the sudo caller, else the configured autologin user, else the current user |
| LightDM | `autologin-session` in the config that enables autologin (value without the `.desktop` suffix), and `last-session` in `/var/lib/lightdm/.cache/lightdm-gtk-greeter-state`, the greeter's pre-selection |
| greetd | `command` in `[default_session]`: the argument after `--cmd` when the current command is a greeter (agreety, gtkgreet, tuigreet), the whole command otherwise — greetd has no session list, its default is a command |

SDDM and GDM session values are the session **file name**
(`wayland-canvas.desktop`); LightDM and greetd name the session or command
without the suffix.

### The change journal

Every file `set-default` touches is recorded under
`/var/lib/wayland-canvas/session-default/` before it is changed: the whole
previous content as a blob, its mode, or its absence. `restore` writes each
blob back byte for byte — including the previous value when the session file
itself is missing — deletes files that were created, and removes directories
the tool created, deepest first and only when empty. `set-default` restores
any recorded default before applying, so running it twice is idempotent.
`restore` with nothing recorded is a no-op.

A display manager that is supported but offers nothing to change (SDDM with
no autologin and `RememberLastSession=false`, greetd without a
`[default_session]` command) is reported, not guessed at; the tool exits
non-zero and changes nothing.

## Install and uninstall

`tools/install.sh` detects the distro (`/etc/os-release`; `$OS_RELEASE`
overrides the file the tests read), builds — or takes, with `--package` — the
matching artifact, installs it with the system package manager, asks "Make
wayland-canvas the default session at login?" and runs
`wayland-canvas-session set-default` on yes; the question needs a terminal,
and without one the default is left unchanged. `--uninstall` restores the
default first and then removes the package; the packages' own removal hooks
run the same restore again, which is a no-op by then, so a plain
`pacman -R` or `apt remove` is equally clean.

Root is for the package manager and nothing else:

| Subject | Rule |
|---|---|
| Where root starts | the install or the removal only. The package is built by the user's own copy of `install.sh`, before it asks for root |
| The re-run through sudo | the root copy is given the step it has to do — `--package <artifact>` for an install, `--uninstall` for a removal — so an argument is never lost and nothing is built again as root. Run as root with no `--package`, `install.sh` says to run it as the user instead |
| Which artifact | the one of this distro's format, newest first, whatever else `dist/` holds |
| Building as root | `tools/package.sh` refuses, except with `--here` in the release container, where root is all there is. It never changes the owner of the checkout: only its own scratch directory and the output directory are handed to the unprivileged account makepkg runs as |
| Proof | the packaging suite drives both copies of `install.sh` against a stub `sudo`, a stub package manager and a stub `package.sh`: the arguments reach the root copy, the root copy builds nothing, and a root `package.sh` without `--here` exits non-zero with the checkout untouched |

## Cross-links

- [build-and-run.md](build-and-run.md) — the build the packages reuse.
- [repos.md](repos.md) — what is committed; the wlroots subproject rule the
  tarball follows.
- [../platform/cpp-standards.md](../platform/cpp-standards.md) — the C++26
  features that decide the Debian release.
