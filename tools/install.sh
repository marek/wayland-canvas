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
#
# The package is built by the user's own copy; root is asked for only to
# install or remove it, and the root copy is handed the step to do.
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
	. "${OS_RELEASE:-/etc/os-release}"
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

# require_root <step...> — re-runs this script through sudo with the step the
# root copy has to do, so nothing of the user's command line is lost
require_root() {
	if [ "$(id -u)" -ne 0 ]; then
		if command -v sudo >/dev/null 2>&1; then
			exec sudo "$0" "$@"
		fi
		die "this step needs root (installing and removing packages)"
	fi
}

newest() { # newest <dir> <name pattern> — the newest matching file, if any
	find "$1" -maxdepth 1 -name "$2" -printf '%T@ %p\n' 2>/dev/null |
		sort -nr | head -1 | cut -d' ' -f2-
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
		*) die "unsupported distro: build from source instead (see docs/guide/install.md), or package it yourself" ;;
	esac

	local pattern
	case $f in
		arch) pattern='*.pkg.tar.zst' ;;
		debian) pattern='*.deb' ;;
	esac

	if [ -n "$package" ]; then
		artifact=$(realpath "$package")
		[ -f "$artifact" ] || die "no such package: $package"
	else
		[ "$(id -u)" -ne 0 ] || die "run install.sh as yourself: it builds the package and asks for root only to install it (or pass --package <artifact>)"
		echo "install.sh: building the package"
		case $f in
			arch) "$REPO/tools/package.sh" --arch >/dev/null ;;
			debian) "$REPO/tools/package.sh" --deb >/dev/null ;;
		esac
		artifact=$(newest "$REPO/dist" "$pattern")
	fi
	[ -n "$artifact" ] || die "no package artifact found"

	require_root --package "$artifact"
	echo "install.sh: installing $artifact"
	case $f in
		arch) pacman -U "$artifact" ;;
		# apt/dpkg may drain stdin even with -y: keep it away from the
		# terminal the default-session question reads from
		debian) apt-get install -y "$artifact" </dev/null ;;
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
	require_root --uninstall
	# restore before the files go; the removal hooks run the same restore
	# again, which is a no-op by then
	if command -v wayland-canvas-session >/dev/null 2>&1; then
		wayland-canvas-session restore || true
	fi
	echo "install.sh: removing the package"
	case $f in
		arch) pacman -R wayland-canvas ;;
		debian) apt-get remove -y wayland-canvas </dev/null ;;
	esac
	echo "install.sh: done — the login points at its previous default again"
}

if [ "$uninstall" -eq 1 ]; then
	cmd_uninstall
else
	cmd_install
fi
