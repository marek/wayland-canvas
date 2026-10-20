#!/usr/bin/env bash
# usage: tools/package-arch.sh [out-dir]
#
# Build the Arch package (.pkg.tar.zst) with makepkg from the local source
# tree. Usually reached through tools/package.sh or the meson run target
# `package-arch` (ninja -C build package-arch).
# The artifact lands in <out-dir> (default dist/); scratch under $CANVAS_TMP.
# The last line is `package-arch.sh: built <path>`.
set -euo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/lib.sh"

REPO=$(repo_root)
P=$REPO/packaging
S=${CANVAS_TMP:-$REPO/tmp}/packaging-arch

case ${1:-} in
	-h | --help)
		usage_from_header
		exit 0
		;;
	-*) die "unknown option: $1 (try --help)" ;;
esac
out=${1:-$REPO/dist}
out=$(realpath -m "$out")

use_venv

export PACKAGER=${PACKAGER:-'Marek Kudlacz <marek@kudlacz.com>'}

stage=$S/stage
rm -rf "$S"
mkdir -p "$stage" "$out"

version=$("$tools_dir/version.sh")
"$P/make-source-tarball.sh" "$stage/wayland-canvas-$version.tar.xz"
cp "$P/PKGBUILD" "$P/wayland-canvas.install" "$stage/"
sed -i "s/^pkgver=.*/pkgver=$version/" "$stage/PKGBUILD"
cd "$stage"

# makepkg's dependency resolver only knows pacman packages, but meson and
# ninja can come from the repo's .venv; verify every makedepend is present
# as one or the other before bypassing the resolver with --nodeps.
(
	source ./PKGBUILD
	missing=()
	for dep in "${makedepends[@]}"; do
		if ! pacman -Q "$dep" >/dev/null 2>&1 && ! command -v "$dep" >/dev/null 2>&1; then
			missing+=("$dep")
		fi
	done
	if [ "${#missing[@]}" -gt 0 ]; then
		die "missing build dependencies: ${missing[*]}"
	fi
)

makepkg -f --nodeps

pkg=$(find . -maxdepth 1 -name '*.pkg.tar.zst' | head -1)
[ -n "$pkg" ] || die "makepkg produced no .pkg.tar.zst"
cp "$pkg" "$out/"
echo "package-arch.sh: built $out/${pkg#./}"
