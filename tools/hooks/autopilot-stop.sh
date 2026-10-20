#!/usr/bin/env bash
# Stop hook: while autopilot is switched on, refuse to end the turn if a plan
# still has open items that do not need the user (the boxes ending in
# (the user) or (the user to decide) do; tools/plan-items.sh tells them apart).
# On only while .claude/autopilot.on exists (the canvas-autopilot skill creates
# and removes it); .claude/autopilot.pause lets a turn end on a real blocker.
set -euo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/../lib.sh"

ROOT=$(repo_root)
[ -d "$ROOT/docs/plans" ] || exit 0
[ -f "$ROOT/.claude/autopilot.on" ] || exit 0
[ -f "$ROOT/.claude/autopilot.pause" ] && exit 0
# a plan list that cannot be read lets the turn end
LIST=$("$ROOT/tools/plan-items.sh" "$ROOT"/docs/plans/2*.md 2>/dev/null | awk -F'\t' '$2 != "user" { print $1 ": " $3 }') || exit 0
OPEN=$(printf '%s' "$LIST" | grep -c . || true)
if [ "$OPEN" -gt 0 ]; then
  NEXT=$(printf '%s' "${LIST%%$'\n'*}" | sed "s/\\\\/\\\\\\\\/g; s/\"/\\\\\"/g" | tr -d '\t' | cut -c1-240)
  printf '{"decision":"block","reason":"Autopilot is on: %s plan items are still open and do not need the user. Do not stop and do not summarise. Continue now with: %s. If the user has moved to a different task, remove .claude/autopilot.on instead. Only if every remaining item is truly blocked on the user, write the blocker to .claude/autopilot.pause and then stop."}\n' "$OPEN" "$NEXT"
fi
exit 0
