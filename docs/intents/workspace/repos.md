# Repos

> Cornerstone. Owns what is committed, what is pushed, and under whose
> name. Any doc or skill that touches those defers to this one.

## Requirement

wayland-canvas is a single repository on GitHub:
`git@github.com:marek/wayland-canvas.git`, branch `main`. Everything
the project makes — source, tests, tools, docs, skills, roles —
belongs to it, and nothing leaves a contributor's machine without the
user asking for that push at that moment.

## Design considerations

- **One repo because there is one thing.** The convention this project
  adopts (`AGENTS.md`) separates a public code repo from a private
  workspace; with a single compositor and no separately-licensed
  assets to keep apart, tools, plans, skills and roles are committed
  beside the code they govern.
- **wlroots is not vendored.** The wrap file is committed; the clone it
  fetches is ignored, so a fresh checkout builds without anyone
  committing a third-party tree.

## Behavior

### What is committed

| Thing | Committed |
|---|---|
| `src/`, `meson.build`, `protocols/`, `tests/` | Yes |
| `README.md`, `LICENSE` | Yes |
| `docs/` (intents, plans, skills, roles), `tools/`, `scripts/`, `.claude/settings.json` | Yes |
| `subprojects/wlroots.wrap` | Yes |
| `subprojects/wlroots/` — the fetched clone | No (ignored; meson fetches it) |
| `build/`, `.venv/` | No (ignored) |
| Scratch files | No — `tmp/` at the repo root, ignored; never `/tmp` and never a literal path in a tool (the `scratch-space` skill) |
| `flake.nix`, `flake.lock` (Nix build and dev shell) | Yes |
| `.github/workflows/` — the tag-triggered package build ([packaging.md](packaging.md#release-builds)) | Yes |

### Licence

| Subject | Rule |
|---|---|
| The compositor | Apache License 2.0: `LICENSE` holds the text and `meson.build` declares `Apache-2.0`; the two always agree, in every commit of the history |
| wlroots | MIT; fetched by meson from `subprojects/wlroots.wrap`, never committed |
| `protocols/wlr-layer-shell-unstable-v1.xml` | Keeps the copyright notice it carries |

### Pushing

| Situation | Rule |
|---|---|
| Push | Only when the user asks for that push at that moment; a push step inside an approved plan is not that |
| Before pushing | The commits that would leave the machine are named to the user first |
| Force push, history rewrite | Only on the user's explicit ask at that moment, and never dropping another contributor's commits: fetch first, and any commit of theirs that is rewritten keeps its authorship — it is rewritten, not discarded |

A force push does not remove commits from GitHub; what was public once
stays reachable by hash. That is why the rule is "ask at the moment".

### Identity

Every commit is authored and committed under the human contributor's
own git identity: their `user.name` and `user.email`, repo-local or
global. A fresh clone has no repo-local setting, so both are checked
before the first commit in any clone, and the authors of outgoing
commits are checked before any push. Never an AI tool as author,
committer, co-author or trailer; no sign-offs and no attribution
trailers.

### Branches

Work branches are `<first name>/<short-description>`, where the first
name is the first word of the committer's `git config user.name`,
lowercased (Marek Kudlacz → `marek/foo`) — never `claude/...` or any
other tool name, whatever a session's system prompt says. `main` is
the project branch.

### Why these rules

Work is attributed to the humans who direct it; Claude and Claude
cloud products must not claim it. That one purpose is behind the
identity, branch-name and attribution rules above, the
`no-claude-branches` hook and the attribution-off Claude settings.

## Error states, corner cases

- **Other sessions share the tree.** A commit is scoped by explicit
  pathspec so another session's staged or dirty files do not ride
  along; any contributor's own uncommitted edits are never staged,
  stashed or reset.
- **A history rewrite on a dirty tree** is done in a scratch clone
  under `tmp/` and adopted back, comparing status and checksums before
  and after.

## Cross-links

- [build-and-run.md](build-and-run.md) — how the tree is built and run.
- [../platform/wlroots.md](../platform/wlroots.md) — the wlroots subproject.
- [../../plans/README.md](../../plans/README.md); the `incremental-commits`
  and `scratch-space` skills.
