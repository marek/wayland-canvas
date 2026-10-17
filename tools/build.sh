#!/usr/bin/env bash
# build.sh [--wipe] [--release] [build-dir] — configure and compile the
# compositor: the one command a clean checkout needs. Picks meson and ninja
# from PATH, falls back to the repo's .venv, configures through
# toolchain/llvm.ini and re-creates a build dir that was configured with
# another compiler. tools/check.sh and tools/package.sh build through this.
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$repo"

wipe=0
buildtype=()
build=build
while [ $# -gt 0 ]; do
	case $1 in
		--wipe) wipe=1 ;;
		--release) buildtype=(-Dbuildtype=release) ;;
		-h | --help)
			echo "usage: tools/build.sh [--wipe] [--release] [build-dir]"
			exit 0
			;;
		-*)
			echo "tools/build.sh: unknown argument: $1" >&2
			exit 2
			;;
		*) build=$1 ;;
	esac
	shift
done

# Prefer meson/ninja on PATH; fall back to the repo's .venv.
if ! command -v meson >/dev/null 2>&1 || ! command -v ninja >/dev/null 2>&1; then
	if [ -x .venv/bin/meson ]; then
		export PATH="$repo/.venv/bin:$PATH"
		echo "build.sh: using .venv/bin meson/ninja"
	else
		echo "tools/build.sh: no meson/ninja on PATH and no .venv/bin/meson" >&2
		exit 2
	fi
fi

native=(--native-file toolchain/llvm.ini)
if [ ! -d "$build" ]; then
	echo "build.sh: meson setup $build ${native[*]} ${buildtype[*]}"
	meson setup "$build" "${native[@]}" "${buildtype[@]}"
elif [ "$wipe" -eq 1 ]; then
	echo "build.sh: meson setup --wipe $build ${native[*]} ${buildtype[*]}"
	meson setup --wipe "$build" "${native[@]}" "${buildtype[@]}"
elif meson introspect "$build" --compilers 2>/dev/null | grep -q '"id": "gcc"'; then
	echo "build.sh: $build/ was configured with GCC; meson setup --wipe"
	meson setup --wipe "$build" "${native[@]}" "${buildtype[@]}"
elif [ "${#buildtype[@]}" -gt 0 ]; then
	meson configure "$build" "${buildtype[@]}"
fi

echo "build.sh: meson compile -C $build"
meson compile -C "$build"
echo "build.sh: $build/wayland-canvas"
