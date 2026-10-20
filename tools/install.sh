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
# install or remove it, and the root copy is handed the step to do. The
# package is the one tools/package.sh reports having built.
set -euo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/lib.sh"

REPO=$(repo_root)

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
		-h | --help) usage_from_header; exit 0 ;;
		*) die "unknown argument: $1 (try --help)" ;;
	esac
done

# select_format — every fact that depends on the package format this distro
# takes, set once: build_flag for package.sh, and the package manager's
# package_installed, install_package <artifact> and remove_package
select_format() {
	local ID= ID_LIKE= id
	. "${OS_RELEASE:-/etc/os-release}"
	for id in "$ID" $ID_LIKE; do
		case $id in
			arch)
				build_flag=--arch
				package_installed() { pacman -Qi wayland-canvas >/dev/null 2>&1; }
				install_package() { pacman -U "$1"; }
				remove_package() { pacman -R wayland-canvas; }
				return 0
				;;
			debian | ubuntu)
				build_flag=--deb
				package_installed() {
					dpkg-query -W -f='${Status}' wayland-canvas 2>/dev/null | grep -q ' installed'
				}
				# apt/dpkg may drain stdin even with -y: keep it away from the
				# terminal the default-session question reads from
				install_package() { apt-get install -y "$1" </dev/null; }
				remove_package() { apt-get remove -y wayland-canvas </dev/null; }
				return 0
				;;
		esac
	done
	die "unsupported distro: build from source instead (see docs/guide/install.md), or package it yourself"
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

cmd_install() {
	local artifact built
	select_format

	if [ -n "$package" ]; then
		artifact=$(realpath "$package")
		[ -f "$artifact" ] || die "no such package: $package"
	else
		[ "$(id -u)" -ne 0 ] || die "run install.sh as yourself: it builds the package and asks for root only to install it (or pass --package <artifact>)"
		echo "install.sh: building the package"
		built=$("$REPO/tools/package.sh" "$build_flag")
		artifact=$(sed -n 's/^package.sh: built //p' <<<"$built" | tail -n 1)
	fi
	[ -n "$artifact" ] || die "package.sh reported no package"

	require_root --package "$artifact"
	echo "install.sh: installing $artifact"
	install_package "$artifact"

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
	select_format
	if ! package_installed; then
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
	remove_package
	echo "install.sh: done — the login points at its previous default again"
}

if [ "$uninstall" -eq 1 ]; then
	cmd_uninstall
else
	cmd_install
fi
