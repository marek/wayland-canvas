#!/usr/bin/env bash
# usage: tools/check.sh [--unit] [--sanitize]   (--unit: stop after the unit suites)
#
# Check the include graph (tools/check-includes.py), build (tools/build.sh)
# and run the test strands in order:
# `meson test --suite unit`, then `meson test --suite packaging`, then
# `meson test --suite scenarios`. Exits non-zero on the first failure.
#
# The scenarios need `foot` and `grim` on PATH and a GPU render node at run
# time. A test that cannot run (those programs, or the bar scenario's `qs`
# and stand-ins, are missing) is still registered, as a skipped test; each
# suite's "passed" line lists what it skipped, with the reason, so a skipped
# scenario is never reported as passed.
#
# --sanitize builds build-asan/ with AddressSanitizer and
# UndefinedBehaviorSanitizer and runs its unit and scenarios suites there:
# any report, a leak included, aborts the run. The packaging suite runs
# nothing compiled, so it is left out.
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

echo "check.sh: tools/check-includes.py"
python3 tools/check-includes.py

echo "check.sh: tools/build.sh ${build_args[*]}"
tools/build.sh "${build_args[@]}"

# the suites run through meson, from the build dir build.sh just made
use_venv

skipped_count=0

# the tests the last meson run skipped, one "SKIP <test>: <reason>" line each
skipped_tests() {
	local log=$build/meson-logs/testlog.json
	[[ -f "$log" ]] || return 0
	python3 - "$log" <<'PY'
import json, sys
for line in open(sys.argv[1]):
    test = json.loads(line)
    if test["result"] == "SKIP":
        said = test["stdout"].strip().splitlines()
        print(said[-1] if said else "SKIP " + test["name"])
PY
}

# suite_passed <suite>: the suite's "passed" line, then what it did not run
suite_passed() {
	local skips
	skips=$(skipped_tests)
	if [[ -z "$skips" ]]; then
		echo "check.sh: $1 suite passed"
		return 0
	fi
	echo "check.sh: $1 suite passed, except for what it skipped:"
	sed 's/^/check.sh:   /' <<<"$skips"
	skipped_count=$((skipped_count + $(wc -l <<<"$skips")))
}

# finish <what ran>: the last line, with the skipped count when there is one
finish() {
	if [[ "$skipped_count" -gt 0 ]]; then
		echo "check.sh: done ($1; $skipped_count skipped, not run)"
	else
		echo "check.sh: done ($1)"
	fi
}

echo "check.sh: meson test -C $build --suite unit ${test_args[*]}"
meson test -C "$build" --suite unit "${test_args[@]}"
suite_passed unit

if [[ "$unit_only" -eq 1 ]]; then
	finish "unit only"
	exit 0
fi

if [[ "$sanitize" -eq 0 ]]; then
	echo "check.sh: meson test -C $build --suite packaging ${test_args[*]}"
	meson test -C "$build" --suite packaging "${test_args[@]}"
	suite_passed packaging
fi

echo "check.sh: meson test -C $build --suite scenarios ${test_args[*]}"
meson test -C "$build" --suite scenarios "${test_args[@]}"
suite_passed scenarios
if [[ "$sanitize" -eq 1 ]]; then
	finish "unit + scenarios, sanitized"
else
	finish "unit + packaging + scenarios"
fi
