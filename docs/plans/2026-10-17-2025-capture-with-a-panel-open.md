# A capture chord while a shell panel holds the keyboard

Date: 2026-10-17 20:25 EDT

## Context

A screenshot chord was reported as doing nothing while the settings
window was open in the overview, and as working again the moment that
window closed. The report could not be reproduced on the current build:
with a bar panel open, both the whole-screen chord and the region
picker work, and the reporter's own retry on the current build worked
too — so what is left is coverage, not a known defect.

The contract this guards is simple and worth a test of its own: a
capture chord belongs to the compositor whoever holds the keyboard. A
shell surface may take typing keys, but Super-held chords are the
compositor's in every mode, so a panel can never swallow a screenshot.

## Tasks

- [x] Scenario: with a bar panel open, the whole-screen chord saves a
      shot and the region picker's drag belongs to the picker.
- [ ] Scenario: the same inside the overview with the settings window
      open — the phase has to open that window late in the bar run,
      which did not hold together; it wants a shorter run of its own or
      a step placed where the window is already up.
- [ ] Decide whether the reported failure was a stale build or a real
      race (the reporter saw it once, the current build does not) — if
      it returns, a bug case study under `docs/intents/bugs/`.
