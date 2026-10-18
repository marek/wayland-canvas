#!/usr/bin/env bash
# Build (tools/build.sh) and run the test strands in order:
# `meson test --suite unit`, then `meson test --suite packaging`, then
# `meson test --suite scenarios`. Exits non-zero on the first failure.
#
# The scenarios need `foot` and `grim` on PATH and a GPU render node at run
# time; when those programs are missing, meson registers no scenario tests
# and the scenarios step is a no-op.
#
# Usage: tools/check.sh [--unit]   (--unit: stop after the unit suite)
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$repo"

unit_only=0
for arg in "$@"; do
	if [[ "$arg" == "--unit" ]]; then
		unit_only=1
	elif [[ "$arg" == "-h" || "$arg" == "--help" ]]; then
		echo "usage: tools/check.sh [--unit]"
		exit 0
	else
		echo "tools/check.sh: unknown argument: $arg" >&2
		exit 2
	fi
done

echo "check.sh: tools/build.sh"
tools/build.sh

# the suites run through meson, from the build dir build.sh just made
bin=$(tools/venv.sh --if-needed)
if [ -n "$bin" ]; then
	export PATH="$bin:$PATH"
fi

echo "check.sh: meson test -C build --suite unit --print-errorlogs"
meson test -C build --suite unit --print-errorlogs
echo "check.sh: unit suite passed"

if [[ "$unit_only" -eq 1 ]]; then
	echo "check.sh: done (unit only)"
	exit 0
fi

echo "check.sh: meson test -C build --suite packaging --print-errorlogs"
meson test -C build --suite packaging --print-errorlogs
echo "check.sh: packaging suite passed"

echo "check.sh: meson test -C build --suite scenarios --print-errorlogs"
meson test -C build --suite scenarios --print-errorlogs
echo "check.sh: scenarios suite passed"
echo "check.sh: done (unit + packaging + scenarios)"
