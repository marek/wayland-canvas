# The version is the git tag

Date: 2026-10-19 09:25 EDT

## Context

A release is made by tagging a commit `v<version>` and nothing else. No
commit raises a number first, and a tag can never disagree with the version
the packages it builds carry. One script, `tools/version.sh`, says what the
version is, and everything that names one reads it: meson's project version,
the source tarball, both package builds and the release workflow.

- A commit tagged `v<version>` is that version, the highest when several tags
  point at it.
- A commit past the last tag is `<version>.r<commits since>.g<short hash>`,
  which pacman and dpkg both order after that tag and before the next.
- A commit with no tag behind it is `0.0.0.r<commits>.g<short hash>`.
- A tree with no git of its own, the source tarball the packages build from,
  reads the `VERSION` file the tarball is made with.
- A tag whose version pacman or dpkg cannot take (a hyphen, an underscore)
  fails the build instead of making a package under a mangled number.

`tools/ci-check-tag.sh` goes, since the tag is the version. The release
workflow checks out the whole history, so a manual run on a branch sees the
tags too. A Nix flake sees no tags: the flake's build names the untagged form
from the flake's own revision. The rules are in
[workspace/packaging.md](../intents/workspace/packaging.md#release-builds).

## Tasks

- [x] The intent: packaging.md's Version row in place of "Tag and version agree", and the full-history checkout
- [x] `tools/version.sh`, read by meson.build, `make-source-tarball.sh` (which writes `VERSION`), `package.sh`, `package-arch.sh` and `package-deb.sh`; the PKGBUILD and the Debian control file carry placeholders; `ci-check-tag.sh` and its workflow steps removed
- [x] The packaging suite proves `version.sh` on scratch repositories: a tag, two tags on one commit, commits past a tag, no tag, a tarball's `VERSION`, a tree with neither, a tag with a hyphen
- [x] `flake.nix` names no number: its build writes `VERSION` from the flake's revision
- [x] Both package builds carry the derived version; `tools/check.sh` green

The flake's missing inputs, found while checking its version, are
[2026-10-19-1009-the-flake-builds.md](2026-10-19-1009-the-flake-builds.md).
