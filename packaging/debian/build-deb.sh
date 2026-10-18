#!/bin/bash
# Runs inside the disposable Debian root filesystem (entered with user
# namespaces by tools/package-deb.sh, fake root, /stage staged by it):
# installs the build dependencies with apt, probes the toolchain for the
# C++26 library features the compositor needs, builds with meson/ninja,
# computes the runtime dependencies with dpkg-shlibdeps and assembles the
# .deb with dpkg-deb. Leaves the artifact in the stage's packages/.
set -euo pipefail
version=$1
# /stage inside the root filesystem tools/package-deb.sh enters; $STAGE when
# that script runs this build here, in a container that already is Debian.
S=${STAGE:-/stage}

echo "deb-build: apt-get update"
apt-get update

echo "deb-build: apt-get install (build dependencies)"
apt-get install -y --no-install-recommends \
	clang g++ llvm meson ninja-build pkg-config dpkg-dev \
	libwayland-dev wayland-protocols libdrm-dev libgbm-dev libegl-dev libgles-dev \
	libinput-dev libseat-dev libxkbcommon-dev libpixman-1-dev \
	libdisplay-info-dev hwdata libcairo2-dev libpango1.0-dev \
	librsvg2-dev libglib2.0-dev libjson-glib-dev libsqlite3-dev libsystemd-dev \
	xwayland libxcb1-dev libxcb-composite0-dev libxcb-ewmh-dev libxcb-icccm4-dev \
	libxcb-render0-dev libxcb-res0-dev libxcb-xfixes0-dev libxcb-errors-dev

# std::function_ref, std::inplace_vector and std::print must all link and
# run; fail here rather than partway through a long build.
cat >/tmp/cxx26-probe.cc <<'EOF'
#include <functional>
#include <inplace_vector>
#include <print>

int main() {
	std::inplace_vector<int, 2> v;
	v.push_back(1);
	auto f = [] { return 1; };
	std::function_ref<int()> fr(f);
	std::print("probe {}", v[0] + fr());
}
EOF
clang++ -std=c++26 -fno-exceptions -fno-rtti /tmp/cxx26-probe.cc -o /tmp/cxx26-probe
/tmp/cxx26-probe
rm -f /tmp/cxx26-probe /tmp/cxx26-probe.cc

echo "deb-build: meson + ninja"
cd "$S"
tar -xJf "wayland-canvas-$version.tar.xz" --no-same-owner
cd "wayland-canvas-$version"
meson setup build --prefix=/usr --native-file toolchain/llvm.ini -Dbuildtype=release
ninja -C build

echo "deb-build: assembling the package"
pkg=$S/pkg
rm -rf "$pkg"
# wlroots is linked in statically; its own install artifacts stay out
DESTDIR="$pkg" meson install -C build --skip-subprojects=wlroots
packaging/check-lock-pam.sh "$pkg"
install -d "$pkg/usr/share/doc/wayland-canvas"
install -m644 LICENSE "$pkg/usr/share/doc/wayland-canvas/copyright"
install -d "$pkg/DEBIAN"
cp "$S/control" "$pkg/DEBIAN/control"
cp "$S/prerm" "$S/postrm" "$pkg/DEBIAN/"
chmod 755 "$pkg/DEBIAN/prerm" "$pkg/DEBIAN/postrm"

arch=$(dpkg --print-architecture)
sed -i "s/^Version:.*/Version: $version-1/" "$pkg/DEBIAN/control"
sed -i "s/^Architecture:.*/Architecture: $arch/" "$pkg/DEBIAN/control"
pkgdir=$(mktemp -d)
mkdir "$pkgdir/debian"
printf 'Source: wayland-canvas\nPackage: wayland-canvas\nArchitecture: any\n' >"$pkgdir/debian/control"
# every installed ELF, not just the compositor: the agent-usage helper is
# the one that links sqlite3, and a missing Depends only shows up at run time
# no process substitution here: the root filesystem this runs in has no
# /dev/fd, and the staged paths have no spaces to split on
elves=$(find "$pkg/usr/bin" -type f -exec sh -c 'head -c4 "$1" | grep -q ELF' _ {} \; -print)
[ -n "$elves" ] || {
	echo "deb-build: no installed binaries found under usr/bin" >&2
	exit 2
}
echo "deb-build: dpkg-shlibdeps over $(echo $elves | sed "s#$pkg##g")"
# shellcheck disable=SC2086
depends=$(cd "$pkgdir" && dpkg-shlibdeps -O $elves 2>/dev/null) || true
rm -rf "$pkgdir"
depends=${depends#shlibs:Depends=}
depends=$(printf '%s' "$depends" | sed 's/[[:space:]]*$//')
if [ -z "$depends" ]; then
	echo "deb-build: dpkg-shlibdeps produced no dependencies" >&2
	exit 2
fi
sed -i "s/^Depends:.*/Depends: $depends/" "$pkg/DEBIAN/control"
echo "deb-build: Depends: $depends"

dpkg-deb --build -Zxz "$pkg" "$S/packages/wayland-canvas_${version}-1_${arch}.deb"
