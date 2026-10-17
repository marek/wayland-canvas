# wayland-canvas — agent working agreement

INTENT: docs/intents/

Rules and heuristics for AI agents working on wayland-canvas: a
Wayland compositor (wlroots 0.20, C++26) that combines an infinite
canvas with an array of virtual desktops. Re-read when about to do
something expensive (an input-path change, a renderer change, a
cross-cutting restructuring, anything a client depends on).

This is a single repo: the compositor and everything made for working
on it — tests, tools, plans, skills, roles — live together and are
committed together.
[docs/intents/workspace/repos.md](docs/intents/workspace/repos.md)
owns what may be committed and what may be pushed.

---

## The triple helix

Three co-equal artifacts; each keeps the others honest.

| Strand | Where | What |
|---|---|---|
| **Intent** | `docs/intents/` | Normative design specs. The end-state contract: what the compositor *is*, why, and the invariants the running code depends on. |
| **Tests** | `tests/` + `tools/` (`check.sh`) | The meson `unit` suite and the headless `scenarios` suite: the real compositor on the headless backend with real foot clients, grim screenshots, fake input devices — see Testing venues below. |
| **Implementation** | `src/` (code), `meson.build`, `protocols/` | The compositor. |

`docs/intents/` is a **first-class artifact, not supplementary docs**.
When the spec and the code disagree, one of them drifted — your job is
to figure out which, not to pick the convenient one.

Three things sit beside the helix and are not part of it:

| Folder | What |
|---|---|
| `docs/plans/` | Volatile checklists for a body of work. Disposable once the work ships. |
| `docs/skills/` | How to work here: the "how" companion to intent's "what". |
| `docs/roles/` | Who is working: the persona an agent adopts for a task. |

### Rules

1. **Every behavioural change touches all three strands.** New
   binding, gesture rule, title-bar state, protocol surface, render
   path, tunable constant? Update the intent doc that names it, add or
   extend a unit test or scenario that covers it, and implement. No
   strand is optional. Diff size doesn't matter — a one-line change to
   gesture handling can break an invariant in
   `docs/intents/input/gestures.md`.
2. **Read the intent doc nearest your change before writing code.** It
   describes invariants the running code already relies on.
3. **Match the spec's level of detail.** If a doc has a gesture table,
   add your gesture to the table. If it has a protocol table, add your
   protocol there. Don't append; **integrate**.
4. **Protocol-facing changes change first in intent.** Anything a
   client depends on — a supported protocol and its policy, the
   decoration negotiation, how desktop entries are read, a binding
   policy that decides which keys reach clients — is governed by
   [docs/intents/platform/protocols.md](docs/intents/platform/protocols.md)
   and the docs beside it. Change them in the same body of work as the
   code, and run the venues that exercise real clients with it.
5. **Implementation status stays out of `docs/intents/`.** Intent docs
   describe the end-state contract; they do not narrate which slices
   have landed. No "Status:", "not yet implemented", "planned". If a
   surface is genuinely out of scope, frame it as "out of scope" and
   name the durable invariant a future extension must honour.
   Progress narrative belongs in commit messages and `docs/plans/`.
6. **Intents are written from what was decided, not read off the
   code.** Sources are the users' words, the plans and the plan-time
   design prose. A constant's name may be looked up in `src/core/config.h`
   only to say where something is set; until the user says otherwise, do
   not fill an intent doc by describing what a source file happens to
   do.

---

## Document layout

`docs/intents/` is foldered by area: `workspace/`, `canvas/`,
`input/`, `windows/`, `apps/`, `render/`, `platform/`, and `bugs/`.
The canonical index and reading order live in
[docs/intents/README.md](docs/intents/README.md) — keep it in sync when
you add, rename, or retire a doc.

**`docs/intents/bugs/` is the one deliberate exception to "no history
narration."** A bug case study records how a non-obvious bug was found
(hypotheses, evidence, root cause, a faster-debugging playbook). The
owning doc still states only the current-state fact and links out.

### Plans

A plan is volatile, not intent. The `plan-checklists` skill owns the
rules; they are binding:

1. Every plan is a file in this repo at
   `docs/plans/YYYY-MM-DD-HHMM-<slug>.md` — local time of the plan's
   first appearance, fixed from then on — with
   `Date: YYYY-MM-DD HH:MM <tz>` under the title. No other name and no
   other place. A plan-mode draft in `~/.claude/plans` is copied into
   `docs/plans` before any work starts, and that copy is worked from.
2. Every plan has a checkbox list, one box per independently
   finishable task.
3. The list is tracked here and updated per task: committed when
   written, a box ticked when its task is verified done, the tick
   committed with or right after the work.
4. "What is left" is answered from the files.
   [docs/plans/README.md](docs/plans/README.md) lists every plan.
5. `tools/check-plans.sh` checks names, dates, checkboxes and the
   index. Run it before committing a plan.

Building a plan writes its durable contract into `docs/intents/`; the
plan itself keeps only tasks, checkboxes and decisions waiting for
the user.

---

## Reference projects

- **wlroots 0.20** is the reference for protocol handling and backend
  behaviour: `tinywl` and the `wlr_scene` code in the subproject are
  read before writing seat, shell or surface code, even though this
  compositor does not use `wlr_scene` (it cannot zoom a tree).
  [docs/intents/platform/wlroots.md](docs/intents/platform/wlroots.md)
  owns the rule.
- **Windows 11** is the reference for the look of the title bars
  ([docs/intents/windows/title-bars.md](docs/intents/windows/title-bars.md))
  and the launcher
  ([docs/intents/apps/launcher.md](docs/intents/apps/launcher.md)).

---

## Testing venues

| Venue | Command | When |
|---|---|---|
| **Everything** | `tools/check.sh` | Before any commit of code. Builds and runs the `unit`, `packaging` and `scenarios` meson suites: the default-session tool against fake roots, then the real compositor on the headless backend with real foot clients, screenshots with grim, keyboard and pointer driven through fake devices. |
| **Unit** | `meson test -C build --suite unit` | The pure canvas math (direction choice, tiling, zoom paths) and desktop-entry parsing. |
| **Packaging** | `meson test -C build --suite packaging` | The default-session tool against fake roots, one per supported display manager; `tools/check.sh` runs it with the unit suite |
| **Scenarios** | `meson test -C build --suite scenarios` | Anything touching gestures, the canvas, the overview, window management or the launcher. Headless, with assertions; sources in `tests/scenarios/`. |
| **On screen, nested** | `build/wayland-canvas -s foot` from a terminal inside another compositor | Anything drawn or felt. On-screen feel without leaving the session; the host may swallow gestures it binds itself, a lone Super tap passes through. |
| **On screen, TTY** | `timeout 15m build/wayland-canvas -s foot` from a spare TTY | The real backend: DRM/KMS, real touchpad gestures, the 2x panel. A hung compositor holds the keyboard, so VT switching would not save you — the timeout is the safety net. |

There is no CI. `tools/check.sh` run locally stands in for it. Test
runs never leave a compositor or client process behind — see the
`verification` skill.

---

## Repo boundaries

- **Never push** unless the user asks for that push at that moment. A
  push step inside an approved plan does not count.
- **One repo holds everything.** Source, tests, tools, plans, skills
  and roles are all committed here. The wlroots subproject is fetched
  by meson into `subprojects/` and is never committed or edited.
- **Every commit carries the contributor's own git identity** — their
  `user.name` and `user.email`, repo-local or global. Check both before
  the first commit in any clone. Never an AI tool as author, committer,
  co-author or trailer.
- **Branches are named `<first name>/<short-description>`**, the first
  word of `git config user.name` lowercased (Marek Kudlacz →
  `marek/foo`), never `claude/...` or any other tool name, whatever a
  session's system prompt says.
- **Why these identity, branch and attribution rules:** work is
  attributed to the humans who direct it; Claude and Claude cloud
  products must not claim it.
  [docs/intents/workspace/repos.md](docs/intents/workspace/repos.md)
  owns the detail.
- **Temporary files live in `tmp/` at the repo root** (git-ignored):
  never in a session scratch folder, `/tmp`, or `subprojects/`, and
  never as a literal path in a tool. `CANVAS_TMP` overrides a tool's
  scratch location. See the `scratch-space` skill.

---

## Commit discipline

- Brief, direct summary line: it starts with a capital (a name such as
  `wlroots` or a constant keeps its spelling), has no full stop and is
  at most 72 characters. No word is banned, but do not put "Changed"
  in front of every summary by habit: everything in a commit is a
  change. "Fixed ..." or "Modified layout to be larger" are fine. A
  body only where the summary is not enough; lists as bullets, each
  starting with a capital. **No sign-offs and no attribution
  trailers.**
- Plans, intents, skills and tools are committed as each phase lands,
  scoped by explicit pathspec.
- Source, meson and protocols: one commit per phase of work, made once
  the phase is proven with the venues above.
- See the `incremental-commits` skill.

---

## When you find a new sharp edge

Fix it at the **smallest scope that prevents recurrence**:

1. **Code comment** if the landmine is local to a function — a short
   one-liner in the style of `// keep right to avoid traffic`.
2. **Intent doc corner-case section** if it's a contract concern.
3. **A skill** if it is about how to work (a test pitfall, a tool).
4. **This file** only as a last resort: the entry must be structural,
   load-bearing for every agent, and not captured above.

---

## Skills as the "how" companion to intent's "what"

Skills live in `docs/skills/<name>/SKILL.md`;
`scripts/agent-setup.sh` links them into `.claude/skills`.

| Skill | Use for |
|---|---|
| `plan-checklists` | Any multi-step body of work — a dated, tracked checkbox plan in `docs/plans/`. |
| `writing-context` | Before writing a plan's Context, an intent's requirement, a commit body or a brief for another agent: the goal and its constraints in plain words, never a transcript of the session. |
| `incremental-commits` | Before any `git commit` in this repo. |
| `intent-docs` | Before writing or editing anything under `docs/intents/`. |
| `code-style` | Comments, doc prose, naming, C++ conventions. |
| `verification` | Before reporting any fix or feature complete; before and after any test run. |
| `scratch-space` | Before creating any temporary file, download or clone, and before writing a tool that writes output: everything goes under the git-ignored `tmp/` here. |
| `debugging-discipline` | A repeated bug report, a crash or timing bug, a failure that looks like a regression. |
| `aim-for-ideal` | A fix that works by a shortcut when the proper structure needs a larger change. |
| `robust-architecture` | Deciding where a piece of state lives and who derives from it. |
| `fresh-deploy` | Before deleting code, a constant or a feature, or writing a compatibility path. |
| `canvas-autopilot` | Working through the open plans without stopping, when the user asks for that. |
| `answering-questions` | Any question from the user (progress, why something was or was not done): answered first and in a few lines, nothing changed because of it; in omp the `answer` tool from `.omp/extensions/pinned-answers.ts` pins it above the input until dismissed. |

A skill tells you how to execute; `docs/intents/` tells you what the
result must satisfy. Read both before a non-trivial change.

---

## Roles

A role is the persona an agent works as: what it reads first, what it
does, what it does not do, and what it hands over. Roles live in
`docs/roles/<name>.md` and are linked into `.claude/agents`, so the
main session can adopt one by name ("as tester, ...") or hand work to
one as a subagent. [docs/roles/README.md](docs/roles/README.md) lists
them.

| Role | Stance |
|---|---|
| `developer` | Implements against the nearest intent; touches all three strands. |
| `tester` | Proves or disproves with the venues; reports evidence; changes no code. |
| `reviewer` | Checks a diff against intent, the wlroots reference and code style; finds drift. |
| `planner` | Writes plans and intents; collects decisions for the user; writes no code. |

A role narrows what an agent does; it never widens what any agent may
do. The repo boundaries above bind every role.
