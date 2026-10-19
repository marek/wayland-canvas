#!/usr/bin/env bash
# package.sh [--arch] [--deb] [--here] [--dist <name>] [out-dir]
# One command for the distribution packages: with no format given it builds
# both. Artifacts land in <out-dir>, dist/ at the repo root by default.
#
#   tools/package.sh                 both formats, isolated (the local flow)
#   tools/package.sh --deb --here    build here, in this very root filesystem
#
# Isolated is the default on a contributor's machine: makepkg over a source
# tarball for Arch, a rootless Debian root filesystem for Debian. --here says
# the current system already *is* that environment — an archlinux or
# debian:sid container in .github/workflows/packages.yml — so no root
# filesystem is downloaded and the in-root build script runs directly.
set -euo pipefail

T=$(cd "$(dirname "$0")" && pwd -P)
REPO=$(dirname "$T")

want_arch=0
want_deb=0
here=0
dist=()
out=$REPO/dist
while [ $# -gt 0 ]; do
	case $1 in
		--arch) want_arch=1 ;;
		--deb) want_deb=1 ;;
		--here) here=1 ;;
		--dist)
			[ $# -ge 2 ] || {
				echo "package.sh: --dist needs a name" >&2
				exit 2
			}
			dist=(--dist "$2")
			shift
			;;
		--dist=*) dist=(--dist "${1#*=}") ;;
		-h | --help)
			sed -n '2,5p' "$0" | sed 's/^# \?//'
			exit 0
			;;
		-*)
			echo "package.sh: unknown option: $1" >&2
			exit 2
			;;
		*) out=$1 ;;
	esac
	shift
done
if [ "$want_arch" -eq 0 ] && [ "$want_deb" -eq 0 ]; then
	want_arch=1
	want_deb=1
fi
# Root belongs to the release container (--here), where the system already is
# the build environment; a root build on a contributor's machine writes root's
# files into their checkout and their caches.
if [ "$(id -u)" -eq 0 ] && [ "$here" -eq 0 ]; then
	echo "package.sh: refusing to build as root; run it as yourself (--here is for the release container)" >&2
	exit 2
fi
out=$(realpath -m "$out")
mkdir -p "$out"

version=$("$T/version.sh")

build_arch() {
	command -v makepkg >/dev/null 2>&1 || {
		echo "package.sh: --arch needs makepkg (an Arch system or container)" >&2
		exit 2
	}
	# makepkg refuses to run as root, and in the release container root is
	# all there is: drop to the image's own unprivileged nobody, with a
	# scratch tree and a HOME it can write. No account is created or
	# changed, and the checkout keeps its owner.
	local exec_as=()
	if [ "$(id -u)" -eq 0 ]; then
		local scratch=${CANVAS_TMP:-$REPO/tmp}
		local home=$scratch/makepkg-home
		mkdir -p "$home" "$scratch/packaging-arch" "$out"
		chown nobody "$scratch"
		chown -R nobody "$scratch/packaging-arch" "$out" "$home"
		echo "package.sh: makepkg as nobody (root cannot run it)"
		exec_as=(setpriv --reuid nobody --regid nobody --clear-groups
			env "HOME=$home" "CANVAS_TMP=$scratch")
	fi
	"${exec_as[@]}" "$T/package-arch.sh" "$out"
}

build_deb() {
	local mode=()
	if [ "$here" -eq 1 ]; then
		mode=(--here)
	fi
	"$T/package-deb.sh" "${mode[@]}" "${dist[@]}" "$out"
}

if [ "$want_arch" -eq 1 ]; then
	build_arch
fi
if [ "$want_deb" -eq 1 ]; then
	build_deb
fi

echo "package.sh: wayland-canvas $version"
ls -1 "$out"
