#!/usr/bin/env bash
# Agent tooling suite: the plan tools, the autopilot hook and agent-setup.sh
# against scratch copies. Every check prints a PASS/FAIL line; the script
# exits non-zero if any check failed.
#
# Covers: tools/plan-items.sh tells a box that waits for a person (it ends in
# (the user) or (the user to decide)) from work, whatever its words or file
# name say, and finds the marker at the end of a wrapped title; check-plans.sh
# fails every other wording; the Stop hook lists the boxes that are work and
# only those; the hooks are wired once, in .claude/settings.json, and
# agent-setup.sh takes the wiring an older version wrote out of an existing
# .claude/settings.local.json without touching what else is in it.
#
# Scratch copies live under $CANVAS_TMP (default: the repo's git-ignored tmp/).
set -uo pipefail

T=$(cd "$(dirname "$0")" && pwd -P)
REPO=$(dirname "$T")
S=${CANVAS_TMP:-$REPO/tmp}/agent-tools-test

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

run_check() { # run_check <name> <regex> <cmd...> — succeeds, and its output matches
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

fails_with() { # fails_with <regex> <cmd...> — the command fails and says so
	local want=$1
	shift
	! "$@" >"$S/out" 2>&1 && grep -q "$want" "$S/out"
}

is() { # is <file> <content> — whole content
	test "$(cat "$1")" = "$2"
}

rm -rf "$S"
mkdir -p "$S"

# plan-items.sh: one box per line, by what it waits for

P=$S/items
mkdir -p "$P"
cat >"$P/2026-01-01-0000-user-demo.md" <<'EOF'
# Demo

Date: 2026-01-01 00:00 UTC

## Tasks

- [ ] Plain work, see docs/guide/user-manual.md and the deferred queue
- [ ] Waiting for the build is no wait on a person
- [ ] An on-screen run (the user)
- [ ] Choose the chord (the user to decide)
- [ ] A long title that wraps onto a second line and
  ends there with the marker (the user)
  - A note under the box that says (the user) is not its title
- [x] Done work (the user, on their machine)
- [ ] Other wording (the user, on their machine)
- [ ] Waiting on the bar (waiting on the bar decision)
- [ ] A taskbar: the user to decide whether and when
- [ ] A full stop after the marker (the user).
- [ ] Deferred (later)
- [ ] Seen on screen (owed)
EOF
kinds=$(printf '%s\t%s\n' \
	2026-01-01-0000-user-demo.md:7 work \
	2026-01-01-0000-user-demo.md:8 work \
	2026-01-01-0000-user-demo.md:9 user \
	2026-01-01-0000-user-demo.md:10 user \
	2026-01-01-0000-user-demo.md:11 user \
	2026-01-01-0000-user-demo.md:15 stray \
	2026-01-01-0000-user-demo.md:16 stray \
	2026-01-01-0000-user-demo.md:17 stray \
	2026-01-01-0000-user-demo.md:18 stray \
	2026-01-01-0000-user-demo.md:19 stray \
	2026-01-01-0000-user-demo.md:20 stray)
plan_kinds() { "$REPO/tools/plan-items.sh" "$1" | cut -f1,2; }
kinds_are() { # kinds_are <plan> <expected> — one line per open box: file:line, kind
	[ "$(plan_kinds "$1")" = "$2" ]
}
check "plan-items: words and file names that only mention the user are work; the marker, wrapped or not, is the user's; every other wording is stray" \
	kinds_are "$P/2026-01-01-0000-user-demo.md" "$kinds"
joined_title() {
	"$REPO/tools/plan-items.sh" "$P/2026-01-01-0000-user-demo.md" | grep -q 'wraps onto a second line and ends there with the marker'
}
check "plan-items: the wrapped title is joined" joined_title

# check-plans.sh fails a stray wording and passes the markers

C=$S/plans
mkdir -p "$C"
printf '# Plans\n\n| Plan | What | Done | Open |\n|---|---|---|---|\n' >"$C/README.md"
for name in 2026-01-01-0000-clean 2026-01-01-0001-stray; do
	printf '# Demo\n\nDate: %s %s:%s UTC\n\n## Context\n\nA goal.\n\n## Tasks\n\n- [ ] Work\n- [ ] On screen (the user)\n- [ ] Choose (the user to decide)\n' \
		"${name:0:10}" "${name:11:2}" "${name:13:2}" >"$C/$name.md"
	printf '| [%s.md](%s.md) | Demo | 0 | 0 |\n' "$name" "$name" >>"$C/README.md"
done
printf -- '- [ ] On screen (the user, on their machine)\n' >>"$C/2026-01-01-0001-stray.md"
"$REPO/tools/check-plans.sh" --fix "$C" >/dev/null 2>&1
run_check "check-plans: a plan with the markers only passes" '^FAIL 2026-01-01-0001-stray' \
	bash -c '"$1" "$2" || true' shell "$REPO/tools/check-plans.sh" "$C"
check "check-plans: the clean plan is not named in a failure" bash -c '! "$1" "$2" 2>&1 | grep -q "FAIL 2026-01-01-0000-clean"' shell "$REPO/tools/check-plans.sh" "$C"
check "check-plans: the stray box fails the run, naming its line" fails_with 'FAIL 2026-01-01-0001-stray.md:14: .*(the user) or (the user to decide)' \
	"$REPO/tools/check-plans.sh" "$C"
sed -i 's/(the user, on their machine)/(the user)/' "$C/2026-01-01-0001-stray.md"
check "check-plans: the marker in place, the run passes" "$REPO/tools/check-plans.sh" "$C"

# the Stop hook: boxes that are work, and only those

H=$S/hook
mkdir -p "$H/tools/hooks" "$H/docs/plans" "$H/.claude"
cp "$REPO/tools/hooks/autopilot-stop.sh" "$H/tools/hooks/"
cp "$REPO/tools/plan-items.sh" "$H/tools/"
printf '# Demo\n\nDate: 2026-01-01 00:00 UTC\n\n- [ ] Work named for the user guide, waiting on nothing\n- [ ] Second piece of work\n- [ ] On screen (the user)\n- [ ] Choose (the user to decide)\n' \
	>"$H/docs/plans/2026-01-01-0000-user-guide.md"
touch "$H/.claude/autopilot.on"
hook() { "$H/tools/hooks/autopilot-stop.sh"; }
run_check "hook: lists the box that only mentions the user, by file and line" '"decision":"block".*2 plan items.*2026-01-01-0000-user-guide.md:5: Work named for the user guide' hook
check "hook: names none of the boxes that wait for a person" bash -c '! grep -q "On screen\|Choose" "$1"' shell "$S/out"
printf '# Demo\n\nDate: 2026-01-01 00:00 UTC\n\n- [x] Done\n- [ ] On screen (the user)\n- [ ] Choose (the user to decide)\n' \
	>"$H/docs/plans/2026-01-01-0000-user-guide.md"
check "hook: only boxes for a person left, the turn may end" bash -c 'test -z "$("$1")"' shell "$H/tools/hooks/autopilot-stop.sh"
printf -- '- [ ] Work again\n' >>"$H/docs/plans/2026-01-01-0000-user-guide.md"
touch "$H/.claude/autopilot.pause"
check "hook: a pause lets the turn end" bash -c 'test -z "$("$1")"' shell "$H/tools/hooks/autopilot-stop.sh"
rm "$H/.claude/autopilot.pause" "$H/.claude/autopilot.on"
check "hook: autopilot off, the turn may end" bash -c 'test -z "$("$1")"' shell "$H/tools/hooks/autopilot-stop.sh"

# hooks are wired once, in the tracked .claude/settings.json

for script in "$REPO"/tools/hooks/*.sh; do
	name=$(basename "$script")
	check "settings.json wires $name once" bash -c \
		'test "$(jq --arg n "tools/hooks/$2" "[.hooks[][].hooks[].command | select(contains(\$n))] | length" "$1")" = 1' \
		shell "$REPO/.claude/settings.json" "$name"
done
check "settings.json names no hook script that does not exist" bash -c '
	for c in $(jq -r "[.hooks[][].hooks[].command | scan(\"tools/hooks/[a-z-]*\\\\.sh\")] | .[]" "$1"); do
		test -x "$2/$c" || exit 1
	done' shell "$REPO/.claude/settings.json" "$REPO"

# agent-setup.sh: skills and roles linked, an older version's hook wiring taken out

A=$S/setup
new_setup() { # new_setup — a scratch checkout holding only what agent-setup.sh reads
	rm -rf "$A"
	mkdir -p "$A/scripts" "$A/docs/skills/demo" "$A/docs/roles" "$A/tools/hooks" "$A/.claude/hooks"
	cp "$REPO/scripts/agent-setup.sh" "$A/scripts/"
	touch "$A/docs/skills/demo/SKILL.md" "$A/docs/roles/demo.md" "$A/tools/hooks/autopilot-stop.sh"
	ln -s ../../tools/hooks/autopilot-stop.sh "$A/.claude/hooks/autopilot-stop.sh"
}
old_local() { # the settings.local.json an older agent-setup.sh wrote
	cat <<'EOF'
{
  "hooks": {
    "Stop": [
      { "hooks": [ { "type": "command", "command": "${CLAUDE_PROJECT_DIR}/.claude/hooks/autopilot-stop.sh" } ] }
    ],
    "UserPromptSubmit": [
      { "hooks": [ { "type": "command", "command": "rm -f \"${CLAUDE_PROJECT_DIR}/.claude/autopilot.pause\"" } ] },
      { "hooks": [ { "type": "command", "command": "${CLAUDE_PROJECT_DIR}/.claude/hooks/reinforce-collaboration-rules.sh" } ] }
    ]
  }
}
EOF
}

new_setup
check "agent-setup: a fresh checkout gets skills and roles linked" bash -c '"$1/scripts/agent-setup.sh" && test -L "$1/.claude/skills/demo" && test -L "$1/.claude/agents/demo.md"' shell "$A"
check "agent-setup: it writes no settings.local.json and leaves no hook links" bash -c 'test ! -e "$1/.claude/settings.local.json" && test ! -e "$1/.claude/hooks"' shell "$A"

new_setup
old_local >"$A/.claude/settings.local.json"
chmod 640 "$A/.claude/settings.local.json"
check "agent-setup: a local file holding only the older wiring is removed" bash -c '"$1/scripts/agent-setup.sh" && test ! -e "$1/.claude/settings.local.json"' shell "$A"

new_setup
old_local | jq '.permissions = {"allow": ["Bash(ls)"]} | .hooks.Stop += [{"hooks": [{"type": "command", "command": "/home/me/mine.sh"}]}]' >"$A/.claude/settings.local.json"
chmod 640 "$A/.claude/settings.local.json"
"$A/scripts/agent-setup.sh" >/dev/null 2>&1
check "agent-setup: other keys and the user's own hooks stay, our wiring goes" bash -c '
	jq -e "(.permissions.allow == [\"Bash(ls)\"]) and (.hooks | keys == [\"Stop\"]) and (.hooks.Stop | length == 1) and (.hooks.Stop[0].hooks[0].command == \"/home/me/mine.sh\")" "$1/.claude/settings.local.json"' shell "$A"
check "agent-setup: the local file keeps its mode" bash -c 'test "$(stat -c %a "$1/.claude/settings.local.json")" = 640' shell "$A"
cp "$A/.claude/settings.local.json" "$S/local.once"
"$A/scripts/agent-setup.sh" >/dev/null 2>&1
check "agent-setup: a second run changes nothing" cmp "$A/.claude/settings.local.json" "$S/local.once"

echo "$passes passed, $fails failed"
rm -rf "$S"
[ "$fails" -eq 0 ]
