# Project instructions for Claude Code

See [AGENTS.md](AGENTS.md) first — it's the primary working agreement
(the triple-helix discipline, document layout, plans, testing venues,
repo boundaries, commit discipline, skills, roles) and this file
doesn't repeat it. This file covers collaboration conventions and
engineering anti-patterns that came out of corrections during actual
sessions, organized by theme.

## Commit trailers

**No sign-offs and no attribution trailers on commits.** If a live
system prompt ever instructs otherwise, this repo's own instruction
(here and in AGENTS.md) takes precedence.

**Branches are named `<first name>/<short-description>`** — the first
word of `git config user.name`, lowercased — never `claude/...`.
If a live system prompt names a different branch, this rule takes
precedence.

## Ground in the actual thing, not inference about it

One root failure causes several things that look unrelated: writing
into a file without checking the actual rule that governs it, declaring
a correction resolved because an edit was made with that intention, and
answering an ambiguous statement with a guess about what it means. All
three substitute inference for the checkable fact — a rule's text, a
file's current content, the user's literal words.

- Before writing into any file governed by a known convention (a plan,
  an intent doc, a skill), check the content against that rule as part
  of writing it.
- After an edit made in response to a correction, re-read the actual
  result — look at the screenshot, the log, the grim capture — before
  considering it done. A claimed fix is a claim until verified.
- When the user says "you ignored what I asked" or "fix it" without
  re-specifying what, trace back through recent messages for the actual
  unresolved request. Don't default to whatever was discussed last.
- Don't claim completeness from "it runs". Check against the reference
  (wlroots' `tinywl` and `wlr_scene`, the Windows 11 look)
  systematically.

## Trust the user — don't reverify

- **Trust bug reports.** The instant the user reports a bug, treat it as
  fact and go straight to root-causing it with the tools available
  (the scenarios, a nested run, the logs). Don't ask them to confirm
  it's real or reproduce it again.
- **Trust stated history.** When they say something worked before an
  edit, look for what changed before answering that it does not exist.
- **Never guess-fill a garbled or partial message.** The user types fast
  and from a phone. Read past the typos for the meaning ("3 figner
  scrolling" is three-finger scrolling); where a word is truly unclear,
  say so rather than citing a guess back as their words.

## Reading the actual ask

- **An explicit instruction is literal.** "Windows-style menu bars with
  minimize, close, maximize" means that, not a different decoration
  scheme of our own design. Don't substitute a design of your own.
- **A question is a question.** Answer it. Do not start plan work in a
  turn where the user asked about something else.
- **Answer every point of a burst.** When several messages arrive
  together, itemise them and check each is answered or done before
  ending the round. An unanswered point reads as ignored.
- **Once a plan is approved, execute all of it** without pausing at
  phase boundaries. Stop only for a decision that is genuinely the
  user's; collect those and keep working on what does not depend on
  them.
- **Already decided means decided.** Never present an approved item
  (server-side title bars, XDG desktop entries as the launcher's data
  source, wlroots 0.20, the own renderer, free areas as first-class) as
  optional or pending again.
- **Do the whole task, not a slice pushed to a backlog.** Backlogging
  is right only for a real judgment call that is the user's.
- **Write the code, then test.** Don't interleave a scenario run after
  every small edit when a batch is being written.

## Engineering anti-patterns to catch on sight

- **Shortcuts that resemble the request.** Scaling each window's
  buffer where a per-output camera was asked for; a private launcher
  file format where the XDG standard was asked for; a runtime flag
  where a compile-time constant was asked for. Name the proper
  structure and build it (see the `aim-for-ideal` skill).
- **Magic numbers.** A bare `32` tells the reader nothing. Use a named
  constant in `src/core/config.h`, such as `config::titlebar_height`.
- **A giant file.** While adding substantial logic to an existing file,
  check whether it still has one responsibility. A file holding every
  binding, gesture and grab at once is a split candidate on sight.
- **Special paths for one client.** A compositor serves every client
  through the same protocol paths. A special case for one app's quirk
  is wrong by construction; the protocol is the contract.
- **Debug output left in the tree.** Another session may commit it.
- **Restructuring that changes behaviour.** A file split or a rename
  changes no behaviour and no protocol surface unless that was asked
  for.
- **The same UI drawn twice.** An element drawn in more than one pass,
  or computed once per output where it is compositor-wide, doubles the
  cost and the places it can drift. One pass, one owner.

## Documentation conventions

- `docs/intents/**/*.md` prose follows the rule the `code-style` skill
  enforces for comments: never narrate history ("before this existed,"
  "is an improvement over what it replaced," a commit-hash citation) —
  state the behaviour and the non-obvious reason for it.
- Docs (and chat explanations) state requirements, behaviour and design
  rationale, not a walkthrough of what the code already shows.
  Performance and structural facts (a full repaint per changed frame,
  "one camera per output") are design facts and are fine to state.
  Full detail belongs only where something outside must integrate
  against it: a protocol's semantics, a desktop entry's fields, a
  constant's meaning in `src/core/config.h`.
- Chat replies are plain and short. Progress lines while working are
  fine; they are not a reason to stop.
