---
name: code-style
description: Comment, prose, naming and C++ conventions for wayland-canvas (the compositor source and the docs/intents corpus) - use before writing or reviewing comments, before writing or editing any intent doc, when choosing a name for a new file, function or type, and before checking code against the wlroots reference.
---

# Code style

## This applies to `docs/intents` prose, not just code comments

Every rule below — no history narration, only state the non-obvious,
keep it concise — governs `docs/intents/**/*.md` exactly as much as a
code comment. A doc section describing a system is a comment at
paragraph scale: it states behaviour and the non-obvious reason for it,
never "before the rework it was X" or "this is a real improvement over
what it replaced". The only place history is told is
`docs/intents/bugs/`.

## Comments are sparse

Add one only where the intent is not obvious from the code, as a short
one-liner:

```cpp
// quantize to the pixel grid: a half-pixel camera blurs every window
cam.cx = std::round(cam.cx * zoom) / zoom;
```

No block comments, no explanation of the obvious, no banner
separators. Leave existing comments as they are unless a change makes
one false.

## Comments describe behaviour, not history

A comment states what the code does and why that is right — never what
it used to do, what bug it fixed, or what was reported. That reasoning
lives in the commit message.

**Violating patterns** — if about to write one, stop and rephrase:
- `used to` / `previously` / `no longer` / `the old <thing>` /
  `replaces` / `instead of the`
- quoting a bug report or a remark from a session
- `was tried first`, `turned out to be`, `confirmed by running`,
  `observed crashing` — how it was found, not what is true
- comparing against an implementation that is no longer in the file
- a commit reference on a line of code (the commit message says that)

```cpp
// BAD - narrates history
// anchoring used to stop at button release, which let left-edge
// resizes drift; now it waits for the ack

// GOOD - states behaviour and why
// anchoring lasts until the client acks the ending configure: its
// final size commit arrives after the release event
```

**A fix often makes the surrounding comment false.** Check comments
near a change for claims about behaviour that no longer hold.

**Fix by asking**: what does this code do, and the non-obvious reason
it does it that way? Write only that.

## Keep it concise

Before finishing a comment or a doc paragraph, cut:

- **Self-evaluation** — "this cleanly solves", "the right pattern".
- **The same fact from a second angle** — if a clause starts "in other
  words", the first one was enough.
- **Stacked qualifiers** — say "X because Y", not three clauses
  building up to it.
- **Narrating a table before the table** — point at it.

## C++ conventions

The full rules — toolchain, class shapes, error states — live in
[../../intents/platform/cpp-standards.md](../../intents/platform/cpp-standards.md).
The short form:

- Every wlroots signal an object handles is a `Listener` member of
  that object (`src/core/util.h`); it disconnects when its owner is
  destroyed, so listener balance is structural, not counted by hand.
- Every compositor object has exactly one owner
  (`std::vector<std::unique_ptr<T>>`); its destroy handler ends with
  `destroy_owned(owner, this)`.
- A state, kind or mode is an `enum class`; `nullptr`, never `NULL`;
  named casts, never C casts; file-local code in an unnamed
  namespace; no macros of our own.
- **No magic numbers.** A raw `32` tells the reader nothing;
  `config::titlebar_height` in `src/core/config.h` does. Gesture and
  animation thresholds live there too.
- **Read the wlroots headers** to verify every API called; check
  `tinywl` and `wlr_scene` in the subproject for how a request handler
  is answered (start the action, don't re-emit the request).
- The wlroots subproject and other bundled third-party code is not
  restyled.

## Naming

- A name says what the thing is: `camera_zoom_path`,
  `usable_canvas_box`, `Desktops::nearest`.
- Types and concepts are `PascalCase`; functions, variables, members,
  enumerators, namespaces and files are `snake_case`; private data
  members end in an underscore (`name_`). No type prefix.
- **A new name must not collide with an existing one.** Grep `src/`
  first — the headers are the contract.
- Rename only when a name is actively misleading; grep every reference
  first.

## Structure

- A file mixing distinct responsibilities is a split candidate — check
  while writing, not when asked. One file holding every binding, gesture
  and grab at once would be that case; they are `input/bindings.cpp`,
  `input/gesture.cpp` and `input/grab.cpp`.
- A new file goes in the area folder of its concept (`src/canvas/`,
  `src/input/`, ...), included from `src/` as `"input/grab.h"`; the folders
  are listed in
  [../../intents/platform/cpp-standards.md](../../intents/platform/cpp-standards.md).
- Two features that need the same behaviour share the implementation;
  the gesture path, the key path and the pointer path drive the same
  canvas actions.
- **Short-circuit returns.** Handle the exceptional case first and
  return — a guard clause — so the main path stays flat and unindented:
  it is easier to read than nested or large if/else branches. This holds
  for every language here: C++, QML and its JavaScript, Python, shell.
