#!/usr/bin/env bash
# Packaging suite: the default-session tool against fake roots, one per
# supported display manager. Every check prints a PASS/FAIL line; the script
# exits non-zero if any check failed.
#
# Covers: set-default then restore leaves every file byte-identical; restore
# with nothing recorded is a no-op; repeated set-default is idempotent; a
# missing previous session is restored as it was; an unknown display manager
# changes nothing.
#
# Fake roots live under $CANVAS_TMP (default: the repo's git-ignored tmp/).
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

# ------------------------------------------------------------------------------

echo "$passes passed, $fails failed"
rm -rf "$S"
[ "$fails" -eq 0 ]
