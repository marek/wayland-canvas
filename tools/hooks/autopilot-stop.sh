#!/bin/bash
# Stop hook: while autopilot is switched on, refuse to end the turn if a plan still has open items that do not need the user.
# On only while .claude/autopilot.on exists (the canvas-autopilot skill creates and removes it); .claude/autopilot.pause lets a turn end on a real blocker.
ROOT=$(cd "$(dirname "$0")/../.." 2>/dev/null && pwd)
[ -d "$ROOT/docs/plans" ] || ROOT=${CLAUDE_PROJECT_DIR:-}
[ -n "$ROOT" ] && [ -d "$ROOT/docs/plans" ] || exit 0
[ -f "$ROOT/.claude/autopilot.on" ] || exit 0
[ -f "$ROOT/.claude/autopilot.pause" ] && exit 0
LIST=$(cd "$ROOT/docs/plans" && grep -HE '^[[:space:]]*- \[ \]' 2*.md 2>/dev/null | grep -viE 'user|deferred|waiting|\(later\)')
OPEN=$(printf '%s' "$LIST" | grep -c .)
if [ "$OPEN" -gt 0 ]; then
  NEXT=$(printf '%s\n' "$LIST" | head -1 | sed "s/\\\\/\\\\\\\\/g; s/\"/\\\\\"/g" | tr -d '\t' | cut -c1-240)
  printf '{"decision":"block","reason":"Autopilot is on: %s plan items are still open and do not need the user. Do not stop and do not summarise. Continue now with: %s. If the user has moved to a different task, remove .claude/autopilot.on instead. Only if every remaining item is truly blocked on the user, write the blocker to .claude/autopilot.pause and then stop."}\n' "$OPEN" "$NEXT"
fi
exit 0
