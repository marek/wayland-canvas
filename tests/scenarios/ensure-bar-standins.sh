#!/usr/bin/env bash
# Provisions and verifies the bar scenario's stand-in Python environment,
# and prints that environment's interpreter path as the last stdout line
# when everything is present. Any failure exits non-zero, in which case
# meson does not register the bar scenario at all.
#
# The stand-ins run on python-dbusmock, which needs dbus-python and
# PyGObject from the system Python's distro packages and dbusmock itself
# from pip: a git-ignored venv on the system interpreter with
# --system-site-packages, under the repo's tmp/. The repo's own .venv
# (tools/venv.sh) has no site packages of the system's, so the stand-ins get
# their own.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
scratch=${CANVAS_TMP:-$root/tmp}
venv_dir=$scratch/bar-standins/venv
python=${CANVAS_BAR_PYTHON:-/usr/bin/python3}

if ! command -v "$python" >/dev/null 2>&1; then
	echo "ensure-bar-standins: $python not found" >&2
	exit 1
fi
if ! command -v dbus-daemon >/dev/null 2>&1; then
	echo "ensure-bar-standins: dbus-daemon is not on PATH" >&2
	exit 1
fi
# The weather stand-in answers wttr.in from a canned response; every other
# URL is passed to the real curl, so it must exist.
if ! command -v curl >/dev/null 2>&1; then
	echo "ensure-bar-standins: curl is not on PATH" >&2
	exit 1
fi
if ! "$python" -c 'import dbus, gi' >/dev/null 2>&1; then
	echo "ensure-bar-standins: $python has no dbus-python/PyGObject (distro packages)" >&2
	exit 1
fi

if [ ! -x "$venv_dir/bin/python" ] || ! "$venv_dir/bin/python" -c 'import dbusmock' >/dev/null 2>&1; then
	echo "ensure-bar-standins: creating $venv_dir (pip install python-dbusmock)" >&2
	rm -rf "$venv_dir"
	"$python" -m venv --system-site-packages "$venv_dir"
	"$venv_dir/bin/pip" install --quiet python-dbusmock
fi

"$venv_dir/bin/python" -c 'import dbus, gi, dbusmock' >/dev/null 2>&1 || {
	echo "ensure-bar-standins: $venv_dir/bin/python cannot import dbus, gi and dbusmock" >&2
	exit 1
}

echo "$venv_dir/bin/python"
