#!/usr/bin/env bash
# usage: tools/check.sh [--unit] [--sanitize]   (--unit: stop after the unit suites)
#
# Build (tools/build.sh) and run the test strands in order:
# `meson test --suite unit`, then the same suite on a second tree built
# without Xwayland (build-no-xwayland/, what a wlroots lacking it gets), then
# `meson test --suite packaging`, then `meson test --suite scenarios`. Exits
# non-zero on the first failure.
#
# The scenarios need `foot` and `grim` on PATH and a GPU render node at run
# time; when those programs are missing, meson registers no scenario tests
# and the scenarios step is a no-op.
#
# --sanitize builds build-asan/ with AddressSanitizer and
# UndefinedBehaviorSanitizer and runs its unit and scenarios suites there:
# any report, a leak included, aborts the run. The packaging suite runs
# nothing compiled, so it is left out, and so is the tree without Xwayland.
# tests/lsan.supp names the leaks of third-party code that are ignored.
set -euo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/lib.sh"

repo=$(repo_root)
cd "$repo"

unit_only=0
sanitize=0
for arg in "$@"; do
	case $arg in
		--unit) unit_only=1 ;;
		--sanitize) sanitize=1 ;;
		-h | --help)
			usage_from_header
			exit 0
			;;
		*) die "unknown argument: $arg (try --help)" ;;
	esac
done

build=build
build_args=()
test_args=(--print-errorlogs)
if [[ "$sanitize" -eq 1 ]]; then
	build=build-asan
	build_args=(--sanitize)
	# the instrumented compositor runs several times slower
	test_args+=(--timeout-multiplier 4)
	export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1
	export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
	export LSAN_OPTIONS=suppressions=$repo/tests/lsan.supp
fi

echo "check.sh: tools/build.sh ${build_args[*]}"
tools/build.sh "${build_args[@]}"

# the suites run through meson, from the build dir build.sh just made
use_venv

echo "check.sh: meson test -C $build --suite unit ${test_args[*]}"
meson test -C "$build" --suite unit "${test_args[@]}"
echo "check.sh: unit suite passed"

if [[ "$sanitize" -eq 0 ]]; then
	echo "check.sh: tools/build.sh --no-xwayland"
	tools/build.sh --no-xwayland
	echo "check.sh: meson test -C build-no-xwayland --suite unit ${test_args[*]}"
	meson test -C build-no-xwayland --suite unit "${test_args[@]}"
	echo "check.sh: no-Xwayland unit suite passed"
fi

if [[ "$unit_only" -eq 1 ]]; then
	echo "check.sh: done (unit only)"
	exit 0
fi

if [[ "$sanitize" -eq 0 ]]; then
	echo "check.sh: meson test -C $build --suite packaging ${test_args[*]}"
	meson test -C "$build" --suite packaging "${test_args[@]}"
	echo "check.sh: packaging suite passed"
fi

echo "check.sh: meson test -C $build --suite scenarios ${test_args[*]}"
meson test -C "$build" --suite scenarios "${test_args[@]}"
echo "check.sh: scenarios suite passed"
if [[ "$sanitize" -eq 1 ]]; then
	echo "check.sh: done (unit + scenarios, sanitized)"
else
	echo "check.sh: done (unit + no-Xwayland unit + packaging + scenarios)"
fi
