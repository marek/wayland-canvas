#!/usr/bin/env bash
# check-lock-pam.sh <package-root> — fails the package build unless the lock
# screen's shipped settings name a PAM service file the package installs:
# without one the locker checks no password and a locked session stays locked.
set -euo pipefail

root=${1:?usage: check-lock-pam.sh <package-root>}
# the package ships the plugins folder under its own prefix, whichever it is
plugin_manifest=plugins/canvas.lock/plugin.json

mapfile -t found < <(find "$root" -type f -path "*/$plugin_manifest")
if [ "${#found[@]}" -ne 1 ]; then
	echo "check-lock-pam.sh: expected one $plugin_manifest in the package, found ${#found[@]}${found[*]:+: ${found[*]}}" >&2
	exit 1
fi
manifest=${found[0]}

# the setting names are the lock plugin's own, read by Lock.qml: a rename must stop the build
service=$(python3 -c '
import json, sys
path = sys.argv[1]
try:
    with open(path) as f:
        settings = json.load(f).get("settings", {})
except (OSError, ValueError) as error:
    sys.exit("check-lock-pam.sh: cannot read " + path + ": " + str(error))
for key in ("pamConfigDirectory", "pamService"):
    if not isinstance(settings.get(key), str):
        sys.exit("check-lock-pam.sh: " + path + " has no string setting " + key)
print(settings["pamConfigDirectory"] + "/" + settings["pamService"])
' "$manifest")

[ -f "$root$service" ] || {
	echo "check-lock-pam.sh: the lock screen reads $service, which the package does not install" >&2
	exit 1
}
echo "check-lock-pam.sh: the lock screen's PAM service $service is in the package"
