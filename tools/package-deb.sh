#!/usr/bin/env bash
# Build the Debian package (.deb) inside a rootless Debian root filesystem,
# downloaded once from the official image registry and cached under
# $CANVAS_TMP/dl/. Everything inside the root filesystem runs through user
# namespaces (unshare --map-root-user --map-auto), so no root, sudo or
# docker is needed. Invoked by the meson run target `package-deb`:
#   ninja -C build package-deb
# The artifact lands in <build-root>/packages/.
#
# The default release is the one whose toolchain builds the C++26 library
# features the compositor needs (std::function_ref, std::inplace_vector,
# std::print) — see docs/intents/workspace/packaging.md. --dist overrides it
# (debian:trixie, ubuntu:26.04, ...); an override that lacks the toolchain
# fails the in-root build early, at its C++26 probe.
#
# set -e does not reach inside $(...) subshells, so every step that fills a
# variable from curl or python3 fails explicitly instead of quietly
# continuing with an empty one.
set -euo pipefail

T=$(cd "$(dirname "$0")" && pwd -P)
REPO=$(dirname "$T")
P=$REPO/packaging
S=${CANVAS_TMP:-$REPO/tmp}
DL=$S/dl

dist=debian:sid
build_root=
while [ $# -gt 0 ]; do
	case $1 in
		--dist)
			[ $# -ge 2 ] || {
				echo "package-deb.sh: --dist needs a name" >&2
				exit 2
			}
			dist=$2
			shift 2
			;;
		--dist=*)
			dist=${1#*=}
			shift
			;;
		-* | --*)
			echo "package-deb.sh: unknown option: $1" >&2
			exit 2
			;;
		*)
			build_root=$1
			shift
			;;
	esac
done
[ -n "$build_root" ] || {
	echo "usage: package-deb.sh [--dist <name>] <build-root>" >&2
	exit 2
}
mkdir -p "$build_root/packages"

rootfs_dir=
staged_version=

fetch_rootfs() { # sets rootfs_dir; caches under $DL
	local dir=$DL/deb-rootfs-${dist//[:.\/]/-}
	if [ -f "$dir/.done" ]; then
		rootfs_dir=$dir
		return 0
	fi
	local repo=library/${dist%%:*}
	local ref=${dist#*:}
	rm -rf "$dir"
	mkdir -p "$dir"

	local token
	token=$(curl -fsS "https://auth.docker.io/token?service=registry.docker.io&scope=repository:${repo}:pull" |
		python3 -c 'import json,sys; print(json.load(sys.stdin)["token"])') || {
		echo "package-deb.sh: could not get a registry token for $repo" >&2
		return 2
	}
	local accept='application/vnd.oci.image.index.v1+json, application/vnd.docker.distribution.manifest.list.v2+json, application/vnd.oci.image.manifest.v1+json, application/vnd.docker.distribution.manifest.v2+json'
	local manifest
	manifest=$(curl -fsS -H "Authorization: Bearer $token" -H "Accept: $accept" \
		"https://registry-1.docker.io/v2/$repo/manifests/$ref") || {
		echo "package-deb.sh: no manifest for $repo:$ref" >&2
		return 2
	}

	# a multi-arch index: pick the linux/amd64 manifest (newer indexes carry
	# the arch only as a bashbrew annotation)
	local digest
	digest=$(printf '%s' "$manifest" | python3 -c '
import json, sys
m = json.load(sys.stdin)
if "manifests" in m:
    for e in m["manifests"]:
        p = e.get("platform", {})
        a = e.get("annotations", {})
        if (p.get("os") == "linux" and p.get("architecture") == "amd64") or \
                a.get("com.docker.official-images.bashbrew.arch") == "amd64":
            print(e["digest"])
            break
    else:
        sys.exit("no linux/amd64 manifest")
') || {
		echo "package-deb.sh: could not pick an amd64 manifest for $repo:$ref" >&2
		return 2
	}
	if [ -n "$digest" ]; then
		manifest=$(curl -fsS -H "Authorization: Bearer $token" -H "Accept: $accept" \
			"https://registry-1.docker.io/v2/$repo/manifests/$digest") || {
			echo "package-deb.sh: no $dist amd64 manifest" >&2
			return 2
		}
	fi

	local layer n=0
	while read -r layer; do
		n=$((n + 1))
		echo "package-deb.sh: $dist layer $n"
		curl -fsSL -H "Authorization: Bearer $token" -o "$dir/layer.tar" \
			"https://registry-1.docker.io/v2/$repo/blobs/$layer" || return 2
		# docker layer whiteouts: .wh.<name> deletes <name>, an opq marker
		# clears the directory it sits in
		while read -r wh; do
			case $wh in
				*/.wh..wh..opq) rm -rf "${wh%/*}"/* 2>/dev/null || true ;;
				*/.wh.*) rm -rf "${wh%/*.wh.*}/${wh##*/.wh.}" 2>/dev/null || true ;;
			esac
		done < <(tar -tf "$dir/layer.tar" | grep '\.wh\.' || true)
		# as fake root in a user namespace the archived owners and modes
		# survive; a plain extraction would make every file root-owned and
		# drop sticky bits, which breaks apt (its _apt user)
		unshare --map-auto --map-root-user tar -C "$dir" -xf "$dir/layer.tar" --numeric-owner || return 2
		find "$dir" -name '.wh.*' -delete
		rm "$dir/layer.tar"
	done < <(printf '%s' "$manifest" |
		python3 -c 'import json,sys; print("\n".join(l["digest"] for l in json.load(sys.stdin)["layers"]))')
	[ -d "$dir/etc" ] || {
		echo "package-deb.sh: $dist extracted to no /etc — the manifest was not an image" >&2
		return 2
	}
	touch "$dir/.done"
	rootfs_dir=$dir
}

setup_rootfs() { # $1 rootfs dir — DNS resolution for apt
	local rootfs=$1
	if [ -f /run/systemd/resolve/resolv.conf ]; then
		cp --remove-destination /run/systemd/resolve/resolv.conf "$rootfs/etc/resolv.conf"
	else
		cp --remove-destination /etc/resolv.conf "$rootfs/etc/resolv.conf"
	fi
}

stage_in_rootfs() { # $1 rootfs dir; sets staged_version
	local rootfs=$1
	staged_version=$(sed -n "s/^[[:space:]]*version: '\(.*\)',/\1/p" "$REPO/meson.build")
	[ -n "$staged_version" ] || {
		echo "package-deb.sh: no version found in meson.build" >&2
		return 2
	}
	rm -rf "$rootfs/stage"
	mkdir -p "$rootfs/stage/packages"
	"$P/make-source-tarball.sh" "$rootfs/stage/wayland-canvas-$staged_version.tar.xz"
	cp "$P/debian/control" "$P/debian/prerm" "$P/debian/postrm" "$P/debian/build-deb.sh" "$rootfs/stage/"
}

run_in_rootfs() { # $1 rootfs dir, rest: arguments for /stage/build-deb.sh
	local rootfs=$1
	shift
	unshare --map-auto --map-root-user --mount --pid --fork --mount-proc \
		chroot "$rootfs" /usr/bin/env -i \
		PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
		HOME=/root \
		DEBIAN_FRONTEND=noninteractive \
		/bin/bash -e /stage/build-deb.sh "$@"
}

fetch_rootfs
setup_rootfs "$rootfs_dir"
stage_in_rootfs "$rootfs_dir"
run_in_rootfs "$rootfs_dir" "$staged_version"

deb=$(find "$rootfs_dir/stage/packages" -name '*.deb' | head -1)
[ -n "$deb" ] || {
	echo "package-deb.sh: the in-root build produced no .deb" >&2
	exit 2
}
cp "$deb" "$build_root/packages/"
echo "package-deb.sh: $build_root/packages/$(basename "$deb")"
