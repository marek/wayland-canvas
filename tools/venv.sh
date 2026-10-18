#!/usr/bin/env bash
# venv.sh [--if-needed] — the repo's .venv with meson and ninja: made when it
# is missing, made again when it was made at another path or its tools do not
# run. Prints its bin dir, for PATH.
#   --if-needed  print nothing when meson and ninja on PATH both run
# tools/build.sh, tools/check.sh and tools/package-arch.sh take meson and
# ninja through this.
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
venv=$repo/.venv

case ${1:-} in
	--if-needed)
		if meson --version >/dev/null 2>&1 && ninja --version >/dev/null 2>&1; then
			exit 0
		fi
		;;
	"") ;;
	*)
		echo "usage: tools/venv.sh [--if-needed]" >&2
		exit 2
		;;
esac

usable() {
	[ -f "$venv/pyvenv.cfg" ] || return 1
	# a venv's scripts name its python by absolute path: after the checkout
	# moves they run the old path's python, or none
	[[ $(head -n 1 "$venv/bin/meson" 2>/dev/null) == "#!$venv/bin/python"* ]] || return 1
	"$venv/bin/meson" --version >/dev/null 2>&1 && "$venv/bin/ninja" --version >/dev/null 2>&1
}

if ! usable; then
	command -v python3 >/dev/null 2>&1 || {
		echo "tools/venv.sh: no python3 on PATH to make $venv with" >&2
		exit 2
	}
	# the floor meson.build itself asks for
	floor=$(sed -n "s/^[[:space:]]*meson_version: '\(.*\)',/\1/p" "$repo/meson.build")
	echo "venv.sh: making $venv (pip install meson$floor ninja)" >&2
	rm -rf "$venv"
	python3 -m venv "$venv"
	"$venv/bin/python" -m pip install --quiet --disable-pip-version-check "meson$floor" ninja
	usable || {
		echo "tools/venv.sh: $venv was made but its meson or ninja does not run" >&2
		exit 2
	}
fi
echo "$venv/bin"
