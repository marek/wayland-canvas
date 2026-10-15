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
| Contents | the `wayland-canvas` binary, `wayland-canvas-session` in `bin`, `wayland-canvas.desktop` in `share/wayland-sessions`, the top bar's Quickshell config in `share/wayland-canvas/shell`, the licence (`/usr/share/licenses` on Arch, `/usr/share/doc/wayland-canvas/copyright` on Debian) |
| Build targets | `ninja -C build package-arch`, `ninja -C build package-deb`; artifacts under `build/packages/` |
| Arch build | `makepkg` over a tarball of the local source tree (the wlroots subproject included, `.git` stripped), built by the same meson native file as a normal build, installed with `--skip-subprojects=wlroots` because wlroots is linked in statically |
| Debian build | inside a rootless Debian root filesystem downloaded from the official image registry, cached under `tmp/dl/`, entered with user namespaces (`unshare --map-root-user --map-auto`); build dependencies installed there by apt; `Depends` computed by `dpkg-shlibdeps` from the built binary |
| Debian release | the one whose toolchain builds the C++26 library features the compositor needs (`std::function_ref`, `std::inplace_vector`, `std::print`); the in-root build probes exactly those three features before compiling, so an unsuitable `--dist` override fails early |
| Prompts | never from the packages themselves: pacman forbids prompts in install scripts, and the Debian maintainer scripts stay non-interactive. `tools/install.sh` is the one flow that asks, for both formats |
| Removal hooks | Arch `pre_remove` and Debian `prerm` run `wayland-canvas-session restore`; Debian `postrm purge` removes `/var/lib/wayland-canvas` |
| Top bar runtime | optional: quickshell is `optdepends` on Arch and `Recommends` on Debian (in sid) — the compositor starts the bar when `qs` is on PATH (`-B` turns it off), and the package is complete without it ([../shell/bar.md](../shell/bar.md)) |

## The session file

`wayland-canvas.desktop` (`Exec=wayland-canvas`, `DesktopNames=wayland-canvas`)
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

`tools/install.sh` detects the distro (`/etc/os-release`), builds — or takes,
with `--package` — the matching artifact, installs it with the system package
manager, asks "Make wayland-canvas the default session at login?" and runs
`wayland-canvas-session set-default` on yes; the question needs a terminal,
and without one the default is left unchanged. `--uninstall` restores the
default first and then removes the package; the packages' own removal hooks
run the same restore again, which is a no-op by then, so a plain
`pacman -R` or `apt remove` is equally clean.

## Cross-links

- [build-and-run.md](build-and-run.md) — the build the packages reuse.
- [repos.md](repos.md) — what is committed; the wlroots subproject rule the
  tarball follows.
- [../platform/cpp-standards.md](../platform/cpp-standards.md) — the C++26
  features that decide the Debian release.
