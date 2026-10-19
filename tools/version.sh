#!/usr/bin/env bash
# version.sh — the project version, from the git tag
# (docs/intents/workspace/packaging.md). meson, the source tarball and both
# package builds read it here and nowhere else:
#
#   a commit tagged v<version>      <version>, the highest of several tags
#   past the last tag               <version>.r<commits since>.g<short hash>
#   no tag behind the commit        0.0.0.r<commits>.g<short hash>
#   no git of its own (a tarball)   the VERSION file the tarball carries
#
# A version pacman or dpkg cannot take (a hyphen, an underscore) fails here
# rather than in the middle of a package build.
set -euo pipefail

T=$(cd "$(dirname "$0")" && pwd -P)
REPO=$(dirname "$T")

git_() { git -C "$REPO" "$@"; }

from_git() {
	local base commit tag count
	if base=$(git_ describe --tags --match 'v[0-9]*' --abbrev=0 2>/dev/null); then
		commit=$(git_ rev-list -n 1 "$base")
		tag=$(git_ tag --points-at "$commit" --list 'v[0-9]*' | sort -V | tail -n 1)
		count=$(git_ rev-list --count "$commit..HEAD")
		if [ "$count" -eq 0 ]; then
			printf '%s\n' "${tag#v}"
		else
			printf '%s.r%s.g%s\n' "${tag#v}" "$count" "$(git_ rev-parse --short=7 HEAD)"
		fi
	else
		printf '0.0.0.r%s.g%s\n' "$(git_ rev-list --count HEAD)" "$(git_ rev-parse --short=7 HEAD)"
	fi
}

# Only this tree's own git: a tarball unpacked inside another checkout (as
# the package builds do, under tmp/) must not take that checkout's tags.
if [ "$(git_ rev-parse --show-toplevel 2>/dev/null)" = "$REPO" ]; then
	version=$(from_git)
elif [ -f "$REPO/VERSION" ]; then
	version=$(head -n 1 "$REPO/VERSION")
else
	echo "version.sh: $REPO is no git checkout and has no VERSION file" >&2
	exit 2
fi

if [[ ! $version =~ ^[0-9][0-9A-Za-z.+~]*$ ]]; then
	echo "version.sh: '$version' is not a version pacman and dpkg both take: a digit first, then only letters, digits, '.', '+' and '~'" >&2
	exit 2
fi
printf '%s\n' "$version"
