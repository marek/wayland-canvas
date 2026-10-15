#!/usr/bin/env bash
# tools/install.sh — the one install flow for both package formats: build (or
# take, with --package) the package for this distro, install it with the
# system package manager, then ask whether wayland-canvas should be the
# default session and run wayland-canvas-session if so. The packages never
# prompt on their own.
#
#   tools/install.sh [--package <artifact>]    install and ask about the default
#   tools/install.sh --uninstall               restore the default, then remove
#
# --uninstall restores the default first (the packages' own removal hooks run
# the same restore again — it is idempotent), so the login is never left
# pointing at a session that no longer exists.
set -euo pipefail

T=$(cd "$(dirname "$0")" && pwd -P)
REPO=$(dirname "$T")

usage() {
	sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}

die() {
	echo "install.sh: $*" >&2
	exit 2
}

uninstall=0
package=
while [ $# -gt 0 ]; do
	case $1 in
		--uninstall) uninstall=1; shift ;;
		--package)
			[ $# -ge 2 ] || die "--package needs a path"
			package=$2
			shift 2
			;;
		--package=*) package=${1#*=}; shift ;;
		-h | --help) usage; exit 0 ;;
		*) die "unknown argument: $1 (try --help)" ;;
	esac
done

family() { # which package format this distro takes
	. /etc/os-release
	case "${ID:-}" in
		arch) echo arch; return 0 ;;
		debian | ubuntu) echo debian; return 0 ;;
	esac
	for id in ${ID_LIKE:-}; do
		case $id in
			arch) echo arch; return 0 ;;
			debian | ubuntu) echo debian; return 0 ;;
		esac
	done
	echo unknown
}

require_root() {
	if [ "$(id -u)" -ne 0 ]; then
		if command -v sudo >/dev/null 2>&1; then
			exec sudo "$0" "$@"
		fi
		die "this step needs root (installing and removing packages)"
	fi
}

installed() {
	case "$(family)" in
		arch) pacman -Qi wayland-canvas >/dev/null 2>&1 ;;
		debian) dpkg-query -W -f='${Status}' wayland-canvas 2>/dev/null | grep -q ' installed' ;;
		*) return 1 ;;
	esac
}

cmd_install() {
	local f artifact
	f=$(family)
	case $f in
		arch | debian) ;;
		*) die "unsupported distro: build from source instead (see README), or package it yourself" ;;
	esac

	if [ -n "$package" ]; then
		artifact=$(realpath "$package")
	else
		echo "install.sh: building the package"
		case $f in
			arch) "$REPO/tools/package-arch.sh" "$REPO/build" >/dev/null ;;
			debian) "$REPO/tools/package-deb.sh" "$REPO/build" >/dev/null ;;
		esac
		artifact=$(find "$REPO/build/packages" -maxdepth 1 \( -name '*.pkg.tar.zst' -o -name '*.deb' \) | head -1)
	fi
	[ -n "$artifact" ] || die "no package artifact found"

	require_root
	echo "install.sh: installing $artifact"
	case $f in
		arch) pacman -U "$artifact" ;;
		debian) apt-get install -y "$artifact" ;;
	esac

	if ask_default; then
		if [ -n "${SUDO_USER:-}" ]; then
			wayland-canvas-session --user "$SUDO_USER" set-default
		else
			wayland-canvas-session set-default
		fi
	else
		echo "install.sh: the default session is unchanged"
		echo "  pick wayland-canvas at the login screen, or start it from a TTY: wayland-canvas"
		echo "  to make it the default later: wayland-canvas-session set-default"
	fi
}

ask_default() {
	if [ ! -t 0 ]; then
		echo "install.sh: not running on a terminal; leaving the default session unchanged"
		return 1
	fi
	local answer
	while true; do
		read -r -p "Make wayland-canvas the default session at login? [y/N] " answer || return 1
		case $answer in
			y | Y | yes | YES | Yes) return 0 ;;
			n | N | no | NO | No | '') return 1 ;;
		esac
	done
}

cmd_uninstall() {
	local f
	f=$(family)
	case $f in
		arch | debian) ;;
		*) die "unsupported distro" ;;
	esac
	if ! installed; then
		echo "install.sh: wayland-canvas is not installed"
		return 0
	fi
	require_root
	# restore before the files go; the removal hooks run the same restore
	# again, which is a no-op by then
	if command -v wayland-canvas-session >/dev/null 2>&1; then
		wayland-canvas-session restore || true
	fi
	echo "install.sh: removing the package"
	case $f in
		arch) pacman -R wayland-canvas ;;
		debian) apt-get remove -y wayland-canvas ;;
	esac
	echo "install.sh: done — the login points at its previous default again"
}

if [ "$uninstall" -eq 1 ]; then
	cmd_uninstall
else
	cmd_install
fi
