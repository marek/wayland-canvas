#!/usr/bin/env bash
# fetch-wlroots.sh — check the wlroots subproject out at the revision
# subprojects/wlroots.wrap names, without meson. The package builds read the
# checkout from the source tarball, so it has to exist before the tarball is
# made; a normal build lets meson fetch the same wrap instead.
# A checkout that is already there is left alone.
set -euo pipefail

T=$(cd "$(dirname "$0")" && pwd -P)
REPO=$(dirname "$T")
wrap=$REPO/subprojects/wlroots.wrap
dir=$REPO/subprojects/wlroots

[ -f "$wrap" ] || {
	echo "fetch-wlroots.sh: no $wrap" >&2
	exit 2
}
if [ -d "$dir" ]; then
	echo "fetch-wlroots.sh: $dir is already there"
	exit 0
fi

field() { sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\(.*\)[[:space:]]*$/\1/p" "$wrap"; }
url=$(field url)
revision=$(field revision)
[ -n "$url" ] && [ -n "$revision" ] || {
	echo "fetch-wlroots.sh: $wrap names no url/revision" >&2
	exit 2
}

echo "fetch-wlroots.sh: $url at $revision"
git clone --depth 1 --branch "$revision" "$url" "$dir"
