# Triple helix adoption

Date: 2026-10-14 18:02 EDT

## Context

The working system of the user's `MilitaryForcesWorkspace` repo, adopted here: intents, plans kept as checklists,
skills, roles and the tools that check them, with the existing plans captured as checklists and intents written for
everything built so far (title bars, the launcher, gestures, the infinite canvas). The rules name whoever directs the
work generically, since there are several contributors: branches take the contributor's first name, and tools and
agents never claim the work for themselves.

The system comes from `MilitaryForcesWorkspace` (cloned to `~/workspaces/MilitaryForcesWorkspace`). It is adapted
here as follows:

| There | Here |
|---|---|
| Three repos (workspace, game, assets) | One repo |
| Tests in `tools/` (`check.sh`, scenarios, soaks) | `tests/` (unit and scenario suites) run by `tools/check.sh` |
| Implementation in the game and assets repos | `src/`, `meson.build`, `protocols/` |
| One named owner | "The user": whoever directs the session. Identity, branch and attribution rules are stated once in [workspace/repos.md](../intents/workspace/repos.md) |
| Branches `marek/<short-description>` | `<first name>/<short-description>`, the first word of `git config user.name` lowercased, enforced by `tools/hooks/no-claude-branches.sh` |
| ioquake3 as the reference | wlroots 0.20 (tinywl, wlr_scene) for protocol behaviour; Windows 11 for the title bar and launcher look |
| Skills for bots, models, assets, ioquake3 ports | Dropped; kept: plan-checklists, intent-docs, incremental-commits, code-style, verification, scratch-space, debugging-discipline, aim-for-ideal, robust-architecture, fresh-deploy, canvas-autopilot |
| Roles (eight) | developer, tester, reviewer, planner |

The repo-local `.venv` holds meson and ninja so nothing is installed system-wide.

## Steps

- [x] Clone `MilitaryForcesWorkspace` and read AGENTS.md, CLAUDE.md, the intent and plan rules, the roles, `check-plans.sh`, the hooks and `agent-setup.sh`
- [x] A brief of the users' words and the plan prose as the source for the intents (not the code)
- [x] AGENTS.md and CLAUDE.md for this repo
- [x] Skills: the eleven above, adapted
- [x] Roles: developer, tester, reviewer, planner
- [x] `tools/check-plans.sh`, `tools/hooks/` (autopilot stop, branch names, collaboration reminder), `scripts/agent-setup.sh`, `.claude/settings.json`, `.gitignore`
- [x] Intents: index plus 21 docs in `canvas/`, `input/`, `windows/`, `apps/`, `render/`, `platform/`, `workspace/`, `bugs/`, `overview.md`, `glossary.md`
- [x] Tests strand: the headless scenarios moved into `tests/scenarios/` (gestures, input, launcher with a custom desktop entry), suites `unit` and `scenarios`, `tools/check.sh`
- [x] Generic to whoever works: "the user" throughout; identity, branch and attribution rules and their purpose once in `workspace/repos.md`; the branch hook refuses `claude/` and any branch not under the contributor's first name
- [x] Plans: the compositor build, the launcher, the review findings, C++ and scriptability, and this one, with the index
- [ ] Verification: `tools/check-plans.sh` passes; no dangling relative links in `docs/` and the root `.md` files; the never-table grep finds nothing in `docs/intents` outside `bugs/`; `scripts/agent-setup.sh` run twice gives the same links; `tools/check.sh` passes
- [ ] Committed per phase: plans, conventions and tooling, intents, tests strand, review fixes
- [ ] Push (the user to decide)
