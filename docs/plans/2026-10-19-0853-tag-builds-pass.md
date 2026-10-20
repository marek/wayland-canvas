# Tag builds pass on GitHub

Date: 2026-10-19 08:53 EDT

## Context

A pushed `v*` tag builds the Arch and Debian packages on GitHub Actions and
drafts a release from them; no tag build has passed yet, for two reasons.

The tags pushed so far, v1.0.0 and v1.0.1, sit on commits whose `meson.build`
still names 0.1.0, so `tools/ci-check-tag.sh` fails them before anything is
built, as it should: a tag passes only on a commit whose `meson.build` names
its number. Past that check the Arch job stops at the dependency check before
makepkg. The workflow installs its own copy of `packaging/PKGBUILD`'s
makedepends, and the X11 build dependencies (xcb-util-errors, xcb-util-wm,
xorg-xwayland) went into the PKGBUILD but never into that copy. The Debian
job already builds: `packaging/debian/build-deb.sh` installs its own build
dependencies.

The fix keeps one list: the Arch job reads the makedepends from the PKGBUILD.
The next release is 1.0.2, a number no tag has used, so the tags already
pushed stay where they are. Pushing the commits and the tag is the user's
step. The rules are in
[workspace/packaging.md](../intents/workspace/packaging.md#release-builds).

## Tasks

- [x] The Arch job installs git, checks out, checks the tag, then installs the makedepends read from `packaging/PKGBUILD`; packaging.md's Release builds table says where each job's build dependencies come from
- [x] `meson.build`, the PKGBUILD, the Debian control file and `flake.nix` name 1.0.2; `tools/ci-check-tag.sh v1.0.2` passes
- [x] The Arch job's steps, run as root in a rootless `archlinux:base-devel` root filesystem, build the package
- [ ] On GitHub, after the user pushes: main and a new v* tag build both packages, named for the tag, and draft the release (the user)
- [x] The packages require what they use: the Arch `depends` name librsvg and lcms2, which the compositor links (lcms2 a makedepend too, so every build links it), and ffmpeg, which records; the Debian `Depends` adds ffmpeg to the computed libraries
