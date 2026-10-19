# The README as a visual front page, the detail in a guide

Date: 2026-10-19 15:00 EDT

## Context

The top-level `README.md` becomes a front page someone new can take in
at a glance: what wayland-canvas is for, a walkthrough video, pictures
of realistic work on it, a quick start and links into the docs. Only
that README changes shape; the other READMEs in the repo (the docs
indexes, the scenario notes) stay as they are.

- **The purpose.** Different work wants different space. Code and
  writing want focus, and tiling gives it. Design, drawing and product
  review want room: material laid out in groups, as on a large dinner
  table, to step back over and lean in on. wayland-canvas holds both on
  one plane: a desktop can be tiled, the canvas around it is free, and
  a swipe moves between them. The README leads with this, and
  [overview.md](../intents/overview.md) states it as the project's
  purpose.
- **Visual.** The pictures and the video are real captures: the
  compositor with real apps (Chrome, a terminal, Krita, a music
  player) arranged as a person would work, with captions saying what
  each gesture does. Nothing from the user's own profiles, accounts or
  files appears; third-party content is free to use and credited.
- **Reproducible.** A tool in `tools/` stages the scenes on the
  headless backend and makes the media, so they are remade when the UI
  changes instead of going stale.
- **Nothing dropped.** Everything the README explains in detail today —
  install, packages, the default session, building, running, the
  control socket, gestures, mouse, keyboard, `canvas.conf`, the
  settings window, the shell and its plugins, capture and screen
  sharing, the session, protocols, limitations, working on the repo —
  moves to a user guide in `docs/guide/`, the public counterpart of
  the intents. The license stays in `LICENSE`; the README names it in
  one line.
- **Kept current.** The guide is where a user-visible change is
  described from now on; the README stays a front page that links into
  it. `AGENTS.md` says so.

Links: [overview.md](../intents/overview.md),
[workspace/repos.md](../intents/workspace/repos.md),
[workspace/build-and-run.md](../intents/workspace/build-and-run.md).

## Shots

| Shot | What it shows |
|---|---|
| Hero | The overview: a tiled desktop for code beside a free table of design material, both on one plane |
| Tiling | Code on a tiled desktop: an editor in a terminal, docs in Chrome, a terminal running the tests |
| Table | Design and review on the canvas: Krita with a drawing, reference images, Chrome pages, sticky notes as labels, a music player, zoomed out to see the groups |
| Walkthrough | About a minute and a half: the tiled desktop, out to the overview, a hop to the table, a look around it, the launcher, tiling a desktop, back; a caption per step |

## Tasks

- [x] Plan written and indexed
- [ ] Intent: the purpose in `docs/intents/overview.md`; the guide and the media in the docs layout (`AGENTS.md`, `docs/intents/README.md`, `docs/intents/workspace/repos.md`, the `intent-docs` skill); the pointers that named the README (`platform/protocols.md`, `tools/install.sh`)
- [ ] The guide in `docs/guide/`: an index and a page per subject, carrying every section the README held
- [ ] The media tool: `tools/media.sh` and its tour program stage the scenes on the headless backend at scale 2 with real apps in scratch profiles, take the stills and record the walkthrough
- [ ] The media made and looked at: hero, tiling, table, the walkthrough video and its animated preview, with credits for the third-party content
- [ ] The README rewritten: tagline, links row, hero, why, the two ways of working with their pictures, the walkthrough, features linking into the guide, quick start, the docs table, contributing, license
- [ ] Every relative link resolves, and every fact of the old README is found in the guide
- [ ] `tools/check.sh` passes with the tour program in the build
- [ ] The walkthrough as a GitHub attachment, for a player inline in the README instead of a linked file (the user to decide: it needs an upload on GitHub)
