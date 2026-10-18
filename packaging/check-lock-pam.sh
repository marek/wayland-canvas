#!/usr/bin/env bash
# check-lock-pam.sh <package-root> — fails the package build unless the lock
# screen's shipped settings name a PAM service file the package installs:
# without one the locker checks no password and a locked session stays locked.
set -euo pipefail

root=${1:?usage: check-lock-pam.sh <package-root>}
manifest=$root/usr/share/wayland-canvas/plugins/canvas.lock/plugin.json

service=$(python3 -c '
import json, sys
settings = json.load(open(sys.argv[1]))["settings"]
print(settings["pamConfigDirectory"] + "/" + settings["pamService"])
' "$manifest")

[ -f "$root$service" ] || {
	echo "check-lock-pam.sh: the lock screen reads $service, which the package does not install" >&2
	exit 1
}
echo "check-lock-pam.sh: the lock screen's PAM service $service is in the package"
