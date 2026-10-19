#!/usr/bin/env bash
# Packaging suite: the default-session tool against fake roots, one per
# supported display manager. Every check prints a PASS/FAIL line; the script
# exits non-zero if any check failed.
#
# Covers: set-default then restore leaves every file byte-identical; restore
# with nothing recorded is a no-op; repeated set-default is idempotent; a
# missing previous session is restored as it was; an unknown display manager
# changes nothing. Then the install flow against stub sudo, package managers
# and package.sh: the step reaches the root copy, the root copy builds
# nothing, the artifact matches the distro's format, and a root package.sh
# without --here refuses. Last, tools/version.sh on scratch repositories: a
# tagged commit, two tags on one, commits past a tag, no tag, a tarball's
# VERSION, a tree with neither, and a tag pacman cannot take.
#
# No check here installs anything, builds anything or runs a real sudo.
#
# Fake roots and stubs live under $CANVAS_TMP (default: the repo's
# git-ignored tmp/).
set -uo pipefail

T=$(cd "$(dirname "$0")" && pwd -P)
REPO=$(dirname "$T")
TOOL=$REPO/packaging/wayland-canvas-session
S=${CANVAS_TMP:-$REPO/tmp}/packaging-test

passes=0
fails=0

check() { # check <name> <cmd...> — passes when the command succeeds
	local name=$1
	shift
	if "$@" >/dev/null 2>&1; then
		echo "PASS $name"
		passes=$((passes + 1))
	else
		echo "FAIL $name"
		fails=$((fails + 1))
	fi
}

# run <name> <cmd...>: like check, but captures the tool's output for greps
run_check() { # run_check <name> <regex> <cmd...>
	local name=$1 want=$2
	shift 2
	if "$@" >"$S/out" 2>&1 && grep -q "$want" "$S/out"; then
		echo "PASS $name"
		passes=$((passes + 1))
	else
		echo "FAIL $name"
		fails=$((fails + 1))
	fi
}

restore_and_diff() { # restore_and_diff <root-dir> <pristine-dir> [tool args...]
	local r=$1 pristine=$2
	shift 2
	"$TOOL" "$@" --root "$r" restore && diff -r "$r" "$pristine"
}

same_tree() {
	diff -r "$1" "$2"
}

has_line() { # has_line <file> <regex>
	grep -q "$2" "$1"
}

root() { # root <name> — a fresh fake root, echoed
	local r=$S/$1
	rm -rf "$r"
	mkdir -p "$r"
	printf '%s\n' "$r"
}

rm -rf "$S"
mkdir -p "$S"

# --- sddm: autologin + remembered session ------------------------------------

r=$(root sddm)
mkdir -p "$r/etc/sddm.conf.d" "$r/var/lib/sddm"
printf '[Autologin]\nUser=marek\nSession=hyprland.desktop\n' >"$r/etc/sddm.conf.d/autologin.conf"
printf '[General]\nRememberLastUser=true\nRememberLastSession=true\n' >"$r/etc/sddm.conf.d/general.conf"
printf '[Last]\nUser=marek\nSession=hyprland-uwsm.desktop\n' >"$r/var/lib/sddm/state.conf"
cp -a "$r" "$S/sddm.pristine"

run_check "sddm status names the display manager" 'display manager: sddm' \
	"$TOOL" --root "$r" status
run_check "sddm status shows the previous default" 'autologin: user marek, session hyprland.desktop' \
	"$TOOL" --root "$r" status

check "sddm set-default exits 0" "$TOOL" --root "$r" set-default
check "sddm autologin session switched" has_line "$r/etc/sddm.conf.d/autologin.conf" '^Session=wayland-canvas\.desktop$'
check "sddm remembered session switched" has_line "$r/var/lib/sddm/state.conf" '^Session=wayland-canvas\.desktop$'
check "sddm journal exists" test -f "$r/var/lib/wayland-canvas/session-default/journal"

check "sddm restore exits 0" "$TOOL" --root "$r" restore
check "sddm restore is byte-identical" same_tree "$r" "$S/sddm.pristine"
check "sddm restore removes the state tree" test ! -e "$r/var/lib/wayland-canvas"

check "sddm restore with nothing recorded is a no-op" "$TOOL" --root "$r" restore
check "sddm second restore still byte-identical" same_tree "$r" "$S/sddm.pristine"

"$TOOL" --root "$r" set-default >/dev/null 2>&1
cp -a "$r" "$S/sddm.once"
"$TOOL" --root "$r" set-default >/dev/null 2>&1
check "sddm repeated set-default is idempotent" same_tree "$r" "$S/sddm.once"
check "sddm idempotent run keeps the switched session" has_line "$r/etc/sddm.conf.d/autologin.conf" '^Session=wayland-canvas\.desktop$'

# --- sddm: autologin only, sessions not remembered ---------------------------

r=$(root sddm-autologin-only)
mkdir -p "$r/etc/sddm.conf.d"
printf '[Autologin]\nUser=marek\n' >"$r/etc/sddm.conf.d/autologin.conf"
printf '[General]\nRememberLastSession=false\n' >"$r/etc/sddm.conf.d/general.conf"
cp -a "$r" "$S/sddm-autologin-only.pristine"

check "sddm autologin-only set-default exits 0" "$TOOL" --root "$r" set-default
check "sddm autologin-only Session line added" has_line "$r/etc/sddm.conf.d/autologin.conf" '^Session=wayland-canvas\.desktop$'
check "sddm autologin-only remembered session untouched" test ! -e "$r/var/lib/sddm/state.conf"
check "sddm autologin-only restore is byte-identical" restore_and_diff "$r" "$S/sddm-autologin-only.pristine"

# --- sddm: previous session missing, state file missing ----------------------

r=$(root sddm-missing-previous)
mkdir -p "$r/etc/sddm.conf.d"
printf '[Autologin]\nUser=marek\n' >"$r/etc/sddm.conf.d/autologin.conf"
cp -a "$r" "$S/sddm-missing-previous.pristine"

check "sddm missing-previous set-default exits 0" "$TOOL" --root "$r" set-default
check "sddm missing-previous state file created" has_line "$r/var/lib/sddm/state.conf" '^Session=wayland-canvas\.desktop$'
check "sddm missing-previous restore is byte-identical" restore_and_diff "$r" "$S/sddm-missing-previous.pristine"
check "sddm missing-previous created dirs are gone" test ! -e "$r/var/lib/sddm"

# --- gdm: AccountsService -----------------------------------------------------

r=$(root gdm)
mkdir -p "$r/var/lib/AccountsService/users"
printf '[User]\nSession=gnome\nXSession=gnome\nSystemAccount=false\n' >"$r/var/lib/AccountsService/users/marek"
cp -a "$r" "$S/gdm.pristine"

run_check "gdm status names the display manager" 'display manager: gdm' \
	"$TOOL" --root "$r" --user marek status
check "gdm set-default exits 0" "$TOOL" --root "$r" --user marek set-default
check "gdm session switched" has_line "$r/var/lib/AccountsService/users/marek" '^Session=wayland-canvas\.desktop$'
check "gdm other keys untouched" has_line "$r/var/lib/AccountsService/users/marek" '^XSession=gnome$'
check "gdm restore is byte-identical" restore_and_diff "$r" "$S/gdm.pristine" --user marek

# gdm: the autologin user is picked up when --user is not given
r=$(root gdm-autologin)
mkdir -p "$r/etc/gdm3" "$r/var/lib/AccountsService/users"
printf '[daemon]\nAutomaticLoginEnable=true\nAutomaticLogin=karel\n' >"$r/etc/gdm3/custom.conf"
printf '[User]\nSession=gnome\n' >"$r/var/lib/AccountsService/users/karel"
cp -a "$r" "$S/gdm-autologin.pristine"

check "gdm autologin user inferred without --user" env SUDO_USER= "$TOOL" --root "$r" set-default
check "gdm inferred user's session switched" has_line "$r/var/lib/AccountsService/users/karel" '^Session=wayland-canvas\.desktop$'
check "gdm autologin restore is byte-identical" restore_and_diff "$r" "$S/gdm-autologin.pristine" --user karel

# gdm: user file missing before set-default
r=$(root gdm-missing)
mkdir -p "$r/var/lib/AccountsService/users"
printf '[User]\nSession=gnome\n' >"$r/var/lib/AccountsService/users/greeter"
cp -a "$r" "$S/gdm-missing.pristine"
check "gdm missing user file created then restored" env TOOL="$TOOL" ROOT="$r" bash -c '
	"$TOOL" --root "$ROOT" --user marek set-default &&
	grep -q "^Session=wayland-canvas.desktop$" "$ROOT/var/lib/AccountsService/users/marek" &&
	"$TOOL" --root "$ROOT" --user marek restore &&
	diff -r "$ROOT" "$(dirname "$ROOT")/gdm-missing.pristine"'

# --- lightdm: autologin + greeter state ---------------------------------------

r=$(root lightdm)
mkdir -p "$r/etc/lightdm" "$r/var/lib/lightdm/.cache"
printf '[Seat:*]\nautologin-user=marek\n' >"$r/etc/lightdm/lightdm.conf"
printf '[greeter]\nlast-session=gnome\n' >"$r/var/lib/lightdm/.cache/lightdm-gtk-greeter-state"
cp -a "$r" "$S/lightdm.pristine"

run_check "lightdm status names the display manager" 'display manager: lightdm' \
	"$TOOL" --root "$r" status
check "lightdm set-default exits 0" "$TOOL" --root "$r" set-default
check "lightdm autologin session switched" has_line "$r/etc/lightdm/lightdm.conf" '^autologin-session=wayland-canvas$'
check "lightdm remembered session switched" has_line "$r/var/lib/lightdm/.cache/lightdm-gtk-greeter-state" '^last-session=wayland-canvas$'
check "lightdm restore is byte-identical" restore_and_diff "$r" "$S/lightdm.pristine"

# --- greetd: default_session command -------------------------------------------

r=$(root greetd)
mkdir -p "$r/etc/greetd"
printf '[terminal]\nvt = 1\n\n[default_session]\ncommand = "agreety --cmd sway"\nuser = "greeter"\n' >"$r/etc/greetd/config.toml"
cp -a "$r" "$S/greetd.pristine"

run_check "greetd status names the display manager" 'display manager: greetd' \
	"$TOOL" --root "$r" status
check "greetd set-default keeps the greeter, switches its --cmd" env TOOL="$TOOL" ROOT="$r" bash -c '
	"$TOOL" --root "$ROOT" set-default &&
	grep -q "^command = \"agreety --cmd wayland-canvas\"$" "$ROOT/etc/greetd/config.toml"'
check "greetd restore is byte-identical" restore_and_diff "$r" "$S/greetd.pristine"

# greetd: bare compositor command
r=$(root greetd-bare)
mkdir -p "$r/etc/greetd"
printf '[default_session]\ncommand = "sway"\n' >"$r/etc/greetd/config.toml"
cp -a "$r" "$S/greetd-bare.pristine"
check "greetd bare command replaced and restored" env TOOL="$TOOL" ROOT="$r" bash -c '
	"$TOOL" --root "$ROOT" set-default &&
	grep -q "^command = \"wayland-canvas\"$" "$ROOT/etc/greetd/config.toml" &&
	"$TOOL" --root "$ROOT" restore &&
	diff -r "$ROOT" "$(dirname "$ROOT")/greetd-bare.pristine"'

# --- unknown display manager: nothing changed ----------------------------------

r=$(root unknown)
cp -a "$r" "$S/unknown.pristine"
run_check "unknown display manager set-default fails and explains" 'start wayland-canvas from a TTY' \
	env bash -c '"$1" --root "$2" set-default; test $? -eq 1' shell "$TOOL" "$r"
check "unknown display manager changed nothing" same_tree "$r" "$S/unknown.pristine"
check "unknown display manager created no state" test ! -e "$r/var/lib"


# --- install.sh: the user builds, root installs --------------------------------

I=$S/install
export CANVAS_IT=$I
mkdir -p "$I/bin" "$I/repo/tools" "$I/repo/dist"
cp "$REPO/tools/install.sh" "$I/repo/tools/install.sh"

# package.sh here records the format it was asked for and leaves an artifact
# of it in dist/: no check in this file builds anything
cat >"$I/repo/tools/package.sh" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$CANVAS_IT/built"
d=$(cd "$(dirname "$0")/../dist" && pwd -P)
case $* in
	*--deb*) : >"$d/wayland-canvas_9.9.9_amd64.deb" ;;
	*) : >"$d/wayland-canvas-9.9.9-1-x86_64.pkg.tar.zst" ;;
esac
STUB

# sudo here records the root copy's command line and runs nothing
cat >"$I/bin/sudo" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$CANVAS_IT/sudo"
STUB

# a second PATH whose id says root drives the same script as the root copy
mkdir -p "$I/rootbin"
cat >"$I/rootbin/id" <<'STUB'
#!/usr/bin/env bash
[ "$*" = "-u" ] && { echo 0; exit 0; }
exec /usr/bin/id "$@"
STUB

for p in pacman apt-get dpkg-query wayland-canvas-session; do
	cat >"$I/bin/$p" <<'STUB'
#!/usr/bin/env bash
printf '%s %s\n' "$(basename "$0")" "$*" >>"$CANVAS_IT/ran"
case "$(basename "$0") $*" in
	'pacman -Qi wayland-canvas') exit 0 ;;
	'dpkg-query'*) echo 'install ok installed'; exit 0 ;;
esac
exit 0
STUB
done
chmod +x "$I/bin"/* "$I/rootbin"/* "$I/repo/tools"/*.sh

printf 'ID=arch\n' >"$I/arch-release"
printf 'ID=debian\n' >"$I/debian-release"

run_install() { # run_install <os-release> <extra PATH entries...> -- <args...>
	local os=$1
	shift
	: >"$I/sudo"
	: >"$I/ran"
	: >"$I/built"
	rm -f "$I/repo/dist"/*
	env PATH="$I/bin:$PATH" OS_RELEASE="$I/$os-release" \
		"$I/repo/tools/install.sh" "$@" >"$I/out" 2>&1
}

# the user's copy builds, then hands the artifact to the root copy
run_install arch
check "install builds as the user" grep -q -- '--arch' "$I/built"
check "install hands --package to the root copy" grep -q -- "--package $I/repo/dist/wayland-canvas-9.9.9-1-x86_64.pkg.tar.zst" "$I/sudo"
check "install runs no package manager as the user" test ! -s "$I/ran"

# --uninstall survives the re-run as well
run_install arch --uninstall
check "uninstall reaches the root copy with its argument" grep -q -- '--uninstall' "$I/sudo"
check "uninstall builds nothing" test ! -s "$I/built"

# the root copy installs what it was given and builds nothing
: >"$I/repo/dist/given.pkg.tar.zst"
: >"$I/sudo"
: >"$I/ran"
: >"$I/built"
env PATH="$I/rootbin:$I/bin:$PATH" OS_RELEASE="$I/arch-release" \
	"$I/repo/tools/install.sh" --package "$I/repo/dist/given.pkg.tar.zst" >"$I/out" 2>&1
check "the root copy installs the package it was given" grep -q -- "pacman -U $I/repo/dist/given.pkg.tar.zst" "$I/ran"
check "the root copy builds nothing" test ! -s "$I/built"
check "the root copy does not call sudo again" test ! -s "$I/sudo"
check "the default session is left alone without a terminal" grep -q 'default session is unchanged' "$I/out"

# a root run with nothing to install says to run it as the user
: >"$I/built"
env PATH="$I/rootbin:$I/bin:$PATH" OS_RELEASE="$I/arch-release" \
	"$I/repo/tools/install.sh" >"$I/out" 2>&1
check "a root run without a package refuses" grep -q 'run install.sh as yourself' "$I/out"
check "a root run without a package builds nothing" test ! -s "$I/built"

# dist/ holding both formats: this distro's one is taken
rm -f "$I/repo/dist"/*
: >"$I/repo/dist/wayland-canvas-9.9.9-1-x86_64.pkg.tar.zst"
: >"$I/repo/dist/wayland-canvas_9.9.9_amd64.deb"
: >"$I/sudo"
env PATH="$I/bin:$PATH" OS_RELEASE="$I/debian-release" \
	"$I/repo/tools/install.sh" >"$I/out" 2>&1
check "a Debian install takes the .deb" grep -q -- '--package .*\.deb$' "$I/sudo"
: >"$I/sudo"
rm -f "$I/repo/dist"/*
: >"$I/repo/dist/wayland-canvas-9.9.9-1-x86_64.pkg.tar.zst"
: >"$I/repo/dist/wayland-canvas_9.9.9_amd64.deb"
env PATH="$I/bin:$PATH" OS_RELEASE="$I/arch-release" \
	"$I/repo/tools/install.sh" >"$I/out" 2>&1
check "an Arch install takes the .pkg.tar.zst" grep -q -- '--package .*\.pkg\.tar\.zst$' "$I/sudo"

# --- package.sh refuses to build as root ---------------------------------------

P=$S/package
mkdir -p "$P/repo/tools"
cp "$REPO/tools/package.sh" "$P/repo/tools/"
cat >"$P/repo/tools/package-arch.sh" <<'STUB'
#!/usr/bin/env bash
printf 'built\n' >>"$CANVAS_IT/built-as-root"
STUB
chmod +x "$P/repo/tools"/*.sh
cp -a "$P/repo" "$P/pristine"

run_check "package.sh as root without --here refuses" 'refusing to build as root' \
	env PATH="$I/rootbin:$I/bin:$PATH" CANVAS_IT="$I" bash -c '"$1" --arch "$2"; test $? -ne 0' shell \
	"$P/repo/tools/package.sh" "$P/out"
check "package.sh as root built nothing" test ! -e "$I/built-as-root"
check "package.sh as root left the checkout as it was" same_tree "$P/repo" "$P/pristine"

# --- version.sh: the version is the git tag ------------------------------------

V=$S/version
mkdir -p "$V/repo/tools" "$V/tarball/tools"
cp "$REPO/tools/version.sh" "$V/repo/tools/"
cp "$REPO/tools/version.sh" "$V/tarball/tools/"
vgit() {
	git -C "$V/repo" -c user.name=scratch -c user.email=scratch@invalid \
		-c commit.gpgsign=false -c tag.gpgsign=false "$@"
}
vhash() { git -C "$V/repo" rev-parse --short=7 HEAD; }
fails_with() { # fails_with <cmd...>: succeeds when the command fails
	! "$@"
}
vgit init -q
vgit commit -q --allow-empty -m one
run_check "version.sh: no tag behind the commit" "^0\.0\.0\.r1\.g$(vhash)\$" "$V/repo/tools/version.sh"
vgit tag v1.2.3
run_check "version.sh: a tagged commit is its tag" '^1\.2\.3$' "$V/repo/tools/version.sh"
vgit tag v1.2.10
run_check "version.sh: two tags on one commit, the higher" '^1\.2\.10$' "$V/repo/tools/version.sh"
vgit commit -q --allow-empty -m two
vgit commit -q --allow-empty -m three
run_check "version.sh: past a tag, <tag>.r<commits>.g<hash>" "^1\.2\.10\.r2\.g$(vhash)\$" "$V/repo/tools/version.sh"
vgit tag v2.0-rc1
run_check "version.sh: a tag pacman cannot take fails" 'is not a version' \
	fails_with "$V/repo/tools/version.sh"
# the tarball sits inside this checkout's tmp/, as a package build's does:
# the enclosing checkout's tags are not its version
printf '3.4.5\n' >"$V/tarball/VERSION"
run_check "version.sh: a tree with no git of its own reads VERSION" '^3\.4\.5$' "$V/tarball/tools/version.sh"
rm "$V/tarball/VERSION"
run_check "version.sh: neither a checkout nor a tarball fails" 'no git checkout' \
	fails_with "$V/tarball/tools/version.sh"

# ------------------------------------------------------------------------------

echo "$passes passed, $fails failed"
rm -rf "$S"
[ "$fails" -eq 0 ]
