#!/usr/bin/env bash
# ci-check-tag.sh <tag> — the release tag and the project version must agree:
# a v<version> tag names the version in meson.build, so the packages a tag
# builds carry that tag's number. Used by .github/workflows/packages.yml
# before anything is built.
set -euo pipefail

T=$(cd "$(dirname "$0")" && pwd -P)
REPO=$(dirname "$T")
tag=${1:?usage: ci-check-tag.sh <tag>}

version=$(sed -n "s/^[[:space:]]*version: '\(.*\)',/\1/p" "$REPO/meson.build")
[ -n "$version" ] || {
	echo "ci-check-tag.sh: no version found in meson.build" >&2
	exit 2
}
if [ "$tag" != "v$version" ]; then
	echo "ci-check-tag.sh: tag $tag does not name meson.build version $version (expected v$version)" >&2
	exit 1
fi
echo "ci-check-tag.sh: $tag matches meson.build version $version"
