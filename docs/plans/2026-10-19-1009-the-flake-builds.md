# The flake builds

Date: 2026-10-19 10:09 EDT

## Context

`nix build` on the repo's flake stops before compiling anything: its
`buildInputs` list what the compositor needed when the flake was written, and
the libraries added since (json-glib, sqlite3, libsystemd for logind, xcb for
X11 windows) are missing. The flake is meant to build the compositor from
nixpkgs' own wlroots 0.20 and to give `nix develop` a shell with the same
inputs, so both come back by naming every input meson asks for. The build
also has to install everything under its own prefix (a Nix build cannot write
into another package's store path), and its check phase runs the suites that
need no display. The version part already works: the flake writes `VERSION`
from its own revision. The rules go into
[workspace/build-and-run.md](../intents/workspace/build-and-run.md).

## Tasks

- [ ] The flake names every library meson asks for, the optional logind and X11 ones included
- [ ] Everything installs under the build's prefix: the session target's directory follows the prefix
- [ ] The check phase runs the unit and packaging suites: their scripts' interpreters patched, git there for the version checks
- [ ] The intent: build-and-run.md says what `nix build` and `nix develop` give
- [ ] `nix build` passes; the binary it makes starts and prints its usage; the Arch and Debian packages still put the session target in `/usr/lib/systemd/user`
