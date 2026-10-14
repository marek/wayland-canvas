#!/bin/bash
# links the agent files kept under docs/ and tools/ into .claude/ (which is git-ignored): skills, roles, hooks.
# safe to run again; removes links whose target is gone. Writes .claude/settings.local.json only if it is missing.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
mkdir -p .claude/skills .claude/agents .claude/hooks
for d in .claude/skills .claude/agents .claude/hooks; do
  for l in "$d"/*; do [ -L "$l" ] && [ ! -e "$l" ] && rm "$l"; done
done
for s in docs/skills/*/; do
  n=$(basename "$s"); [ -f "$s/SKILL.md" ] && ln -sfn "../../docs/skills/$n" ".claude/skills/$n"
done
for r in docs/roles/*.md; do
  n=$(basename "$r"); [ "$n" = "README.md" ] && continue
  ln -sfn "../../docs/roles/$n" ".claude/agents/$n"
done
for h in tools/hooks/*.sh; do
  chmod +x "$h"; ln -sfn "../../tools/hooks/$(basename "$h")" ".claude/hooks/$(basename "$h")"
done
if [ ! -f .claude/settings.local.json ]; then
  cat > .claude/settings.local.json <<'EOF'
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
  echo "wrote .claude/settings.local.json"
fi
echo "skills: $(ls .claude/skills | wc -l | tr -d ' ')  roles: $(ls .claude/agents | wc -l | tr -d ' ')  hooks: $(ls .claude/hooks | wc -l | tr -d ' ')"
