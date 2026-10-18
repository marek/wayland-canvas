#!/usr/bin/env bash
# Build the Arch package (.pkg.tar.zst) with makepkg from the local source
# tree. Usually reached through tools/package.sh or the meson run target
# `package-arch` (ninja -C build package-arch).
# The artifact lands in <out-dir> (default dist/); scratch under $CANVAS_TMP.
set -euo pipefail

T=$(cd "$(dirname "$0")" && pwd -P)
REPO=$(dirname "$T")
P=$REPO/packaging
S=${CANVAS_TMP:-$REPO/tmp}/packaging-arch

out=${1:-$REPO/dist}
out=$(realpath -m "$out")

# meson/ninja from PATH when they run there, else from the repo's .venv
bin=$("$REPO/tools/venv.sh" --if-needed)
if [ -n "$bin" ]; then
	export PATH="$bin:$PATH"
fi

export PACKAGER='Marek Kudlacz <marek@kudlacz.com>'

stage=$S/stage
rm -rf "$S"
mkdir -p "$stage" "$out"

version=$(sed -n "s/^[[:space:]]*version: '\(.*\)',/\1/p" "$REPO/meson.build")
[ -n "$version" ] || {
	echo "package-arch.sh: no version found in meson.build" >&2
	exit 2
}
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
		echo "package-arch.sh: missing build dependencies: ${missing[*]}" >&2
		exit 2
	fi
)

makepkg -f --nodeps

pkg=$(find . -maxdepth 1 -name '*.pkg.tar.zst' | head -1)
[ -n "$pkg" ] || {
	echo "package-arch.sh: makepkg produced no .pkg.tar.zst" >&2
	exit 2
}
cp "$pkg" "$out/"
echo "package-arch.sh: $out/${pkg#./}"
