#!/usr/bin/env bash
# usage: tools/package.sh [--arch] [--deb] [--here] [--dist <name>] [out-dir]
#
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
#
# The builds' output goes through, and the last lines name what was built:
# one `package.sh: built <path>` for each package. tools/install.sh reads them.
set -euo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/lib.sh"

REPO=$(repo_root)

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
			[ $# -ge 2 ] || die "--dist needs a name"
			dist=(--dist "$2")
			shift
			;;
		--dist=*) dist=(--dist "${1#*=}") ;;
		-h | --help)
			usage_from_header
			exit 0
			;;
		-*) die "unknown option: $1 (try --help)" ;;
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
	die "refusing to build as root; run it as yourself (--here is for the release container)"
fi
out=$(realpath -m "$out")
mkdir -p "$out"

version=$("$tools_dir/version.sh")
scratch=${CANVAS_TMP:-$REPO/tmp}
mkdir -p "$scratch"
log=$(mktemp "$scratch/package-output.XXXXXX")
trap 'rm -f "$log"' EXIT

build_arch() {
	command -v makepkg >/dev/null 2>&1 ||
		die "--arch needs makepkg (an Arch system or container)"
	# makepkg refuses to run as root, and in the release container root is
	# all there is: drop to the image's own unprivileged nobody, with a
	# scratch tree and a HOME it can write. No account is created or
	# changed, and the checkout keeps its owner.
	local exec_as=()
	if [ "$(id -u)" -eq 0 ]; then
		local home=$scratch/makepkg-home
		mkdir -p "$home" "$scratch/packaging-arch" "$out"
		# nobody cannot write into the root-owned checkout, so the wlroots
		# source the tarball needs is fetched here (a no-op when it is there)
		"$tools_dir/fetch-wlroots.sh"
		chown nobody "$scratch"
		chown -R nobody "$scratch/packaging-arch" "$out" "$home"
		echo "package.sh: makepkg as nobody (root cannot run it)"
		exec_as=(setpriv --reuid nobody --regid nobody --clear-groups
			env "HOME=$home" "CANVAS_TMP=$scratch")
	fi
	"${exec_as[@]}" "$tools_dir/package-arch.sh" "$out" | tee -a "$log"
}

build_deb() {
	local mode=()
	if [ "$here" -eq 1 ]; then
		mode=(--here)
	fi
	"$tools_dir/package-deb.sh" "${mode[@]}" "${dist[@]}" "$out" | tee -a "$log"
}

if [ "$want_arch" -eq 1 ]; then
	build_arch
fi
if [ "$want_deb" -eq 1 ]; then
	build_deb
fi

echo "package.sh: wayland-canvas $version"
sed -n 's/^package-[a-z]*\.sh: built /package.sh: built /p' "$log"
