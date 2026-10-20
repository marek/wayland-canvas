#!/usr/bin/env bash
# usage: tools/fetch-wlroots.sh
#
# Check the wlroots subproject out at the revision subprojects/wlroots.wrap
# names, without meson. The package builds read the checkout from the source
# tarball, so it has to exist before the tarball is made; a normal build lets
# meson fetch the same wrap instead. A checkout that is already there is left
# alone.
set -euo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/lib.sh"

REPO=$(repo_root)
wrap=$REPO/subprojects/wlroots.wrap
dir=$REPO/subprojects/wlroots

case ${1:-} in
	-h | --help)
		usage_from_header
		exit 0
		;;
	?*) die "unknown argument: $1 (try --help)" ;;
esac

[ -f "$wrap" ] || die "no $wrap"
if [ -d "$dir" ]; then
	echo "fetch-wlroots.sh: $dir is already there"
	exit 0
fi

field() { sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\(.*\)[[:space:]]*$/\1/p" "$wrap"; }
url=$(field url)
revision=$(field revision)
[ -n "$url" ] && [ -n "$revision" ] || die "$wrap names no url/revision"

echo "fetch-wlroots.sh: $url at $revision"
git clone --depth 1 --branch "$revision" "$url" "$dir"
