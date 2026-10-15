#!/usr/bin/env bash
# make-source-tarball.sh <output.tar.xz> — the source tarball the packages
# build from: the working tree without scratch and build output, plus the
# wlroots checkout (dereferenced, .git stripped) so a package build needs no
# network. The archive root is wayland-canvas-<version>/.
set -euo pipefail

P=$(cd "$(dirname "$0")" && pwd -P)
REPO=$(dirname "$P")
out=${1:?usage: make-source-tarball.sh <output.tar.xz>}

mkdir -p "$(dirname "$out")"
stage=$(mktemp -d "$(dirname "$out")/.tarball.XXXXXX")
trap 'rm -rf "$stage"' EXIT

version=$(sed -n "s/^[[:space:]]*version: '\(.*\)',/\1/p" "$REPO/meson.build")
[ -n "$version" ] || {
	echo "make-source-tarball.sh: no version found in meson.build" >&2
	exit 2
}

mkdir -p "$stage/wayland-canvas-$version"
tar -C "$REPO" -cf - -h \
	--exclude='.git' \
	--exclude='.claude' \
	--exclude='.venv' \
	--exclude='build' \
	--exclude='tmp' \
	--exclude='subprojects/.wraplock' \
	--exclude='subprojects/wlroots/.git' \
	. | tar -C "$stage/wayland-canvas-$version" -xf -

tar -C "$stage" -cJf "$out" "wayland-canvas-$version"
