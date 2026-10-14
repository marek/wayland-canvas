---
name: debugging-discipline
description: Lessons from debugging wayland-canvas - a repeated report is signal, not flakiness; check the environment before blaming the code and the code before blaming the environment; a second attempt at a timing or state bug should redesign rather than re-patch; fix a bug at the shared layer, not at the call site that reported it; drive a state transition explicitly. Use whenever a bug is reported again after a fix, whenever a test fails in a way that looks like a regression, whenever a first fix does not hold, and when hunting crashes, grabs that never end, or rendering corruption.
---

# Debugging discipline

## 1. A repeated report is signal, not noise

When the user reports the same failure again after it was called fixed,
revise the theory; do not explain the report away. They have more reps
than one captured log, and they are the one who has to keep reproducing
it. Go back to the literal request and to the code as it stands, and
start from there — each round that patches the last attempt instead of
the cause produces a different wrong thing.

Trust the report as stated. "The title bar draws twice" is a fact to
root-cause with the scenarios and a nested run, not a claim to confirm.

## 2. Environment or code: check, don't assume — in both directions

- **A failure that looks like a regression may be the environment.**
  Before bisecting, check: the nested host eating the gesture, a stale
  `WAYLAND_DISPLAY`, a leftover compositor holding the socket, a stale
  build, a client that never mapped.
- **A failure blamed on the environment may be the code.** "Probably
  the host swallowed it" is a theory that lets the investigation stop;
  it needs evidence like any other. Inject the gesture through the
  scenario path and see.

## 3. Crashes and corruption: use the tools built for it

A crash in a destroy path, a grab that never ends, garbled output is a
lifecycle or memory bug until shown otherwise.

- Build with ASan and UBSan.
- A crash inside wlroots' destroy handling is usually a listener that
  was added and never removed: wlroots asserts that a destroyed object
  has no listeners.
- Suspect any pointer/integer cast, arithmetic on `sizeof`, state kept
  past the destroy of the object it refers to (a desktop destroyed
  mid-swipe, an output destroyed mid-gesture).
- Prefer device-level injection (fake keyboard/pointer through the
  backend) over calling handlers directly: it walks the real path and
  finds grabs and leaks that direct calls cannot.

`docs/intents/bugs/grabs-that-never-ended.md` is the case study.

## 4. A second attempt should redesign, not re-patch

If a fix for a race, a timing-dependent failure or "it works sometimes"
does not hold on the next report, ask what design would make the whole
class of bug impossible before reaching for another targeted patch.
Anchoring that stopped at the button release could not be patched into
behaving; anchoring until the client acks the ending configure removed
the class.

## 5. Fix it at the layer every caller shares

Before wiring a fix into the call site that reported the bug, ask
whether every other call site of the same shape has the same gap. If a
grab leaked on one button path, every grab path has the same hole: the
fix belongs where a grab ends, once. If one gesture left stale state
behind, ask which other gestures leave the same state behind — the
answer is usually "all of them".

## 6. Drive a state transition explicitly

A real transition — an output plugged or unplugged, a client mapping or
destroying, a gesture beginning or ending — gets its own explicit step
that the rest waits on, not "leave the state half valid and let an
error path notice". A gesture that ends while its output is gone ends
through a path that clears its state; it does not leave the offset for
the next gesture to trip over.

## 7. Log enough to see it

When behaviour is wrong and the cause is not visible, run with `-d`,
add logging behind it where the debug output is thin (camera, zoom,
gesture state, hit-test results) and read it, rather than guessing from
the outcome. Temporary logging comes out of the source before the turn
ends.
