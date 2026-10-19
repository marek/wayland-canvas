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
  files appears.
- **Reproducible.** A tool in `tools/` stages the scenes on the
  headless backend and makes the media, so they are remade when the UI
  changes instead of going stale. The tools around the capture are
  Python; the tour that stages and captures runs inside the
  compositor's process, so it is C++ against the compositor's own
  classes.
- **The table's web pages.** The pages under review on the table are
  the Toronto Star's front page of the day it was recorded and
  r/programming (logged-out reddit.com: Old Reddit needs an account).
  Every page is recorded once and replayed on later runs, so the
  pictures keep that day; the snapshots stay in `tmp/media/pages/`, out
  of the repo, being third-party pages. Ads are taken out before the
  snapshot.
- **A note of the user's.** A pink sticky note on the table says
  "❤️ Murder Mittens is the best".
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
| Hero | The overview: the tiled desktops for code and writing above the table at 50 %, all on one plane |
| Tiling, writing | Code on a tiled desktop (an editor in a terminal, docs in Chrome, a terminal running the tests) and writing on another (a Markdown draft beside Wikipedia) |
| Table | Design and review on a desktop at 50 %: Krita with a drawing, reference plates, the Toronto Star and r/programming under review, sticky notes as labels and the pink Murder Mittens note, a music player |
| Walkthrough | About forty seconds: typing on the code desktop, a hop to writing, Super+arrows through the slices, the overview, the table's frame dragged off its windows and back, a hop to the table and in, leaning in with Super and a pinch, the launcher, home; a caption per step |

## Tasks

- [x] Plan written and indexed
- [x] Intent: the purpose in `docs/intents/overview.md`; the guide and the media in the docs layout (`AGENTS.md`, `docs/intents/README.md`, `docs/intents/workspace/repos.md`, the `intent-docs` skill); the pointers that named the README (`platform/protocols.md`, `tools/install.sh`)
- [x] The guide in `docs/guide/`: an index and a page per subject, carrying every section the README held
- [x] The media tool: `tools/media.py` and its tour program stage the scenes on the headless backend at scale 2 with real apps in scratch profiles, take the stills and record the walkthrough
- [x] Web pages recorded once and replayed: today's thestar.ca and r/programming on the table, ads taken out, the snapshots kept in `tmp/media/pages/`
- [x] The pink "❤️ Murder Mittens is the best" note on the table
- [x] The media made and looked at: hero, tiling, writing, table, the walkthrough video and its animated preview
- [x] The README rewritten: tagline, links row, hero, why, the two ways of working with their pictures, the walkthrough, features linking into the guide, quick start, the docs table, contributing, license
- [x] Every relative link resolves, and every fact of the old README is found in the guide
- [x] `tools/check.sh` with the tour program in the build: not run, by the user's call; the tour builds without warnings and the compositor's own sources are untouched
- [x] The writing desktop's page: the `wl_touch` section of the Wayland protocol specification (wayland.freedesktop.org) in place of the Wikipedia article
- [x] Installing defaults to the latest public release (README quick start, the guide's install page); building your own package and building from source stay in the guide
- [x] The README and the walkthrough captions in plain words, reviewed line by line; the contributing line says agentic development with intents, tests and implementation rather than naming the triple helix
- [x] The walkthrough as a GitHub attachment, played inline in the README; the video is no longer committed, and `tools/media.py` writes it to `tmp/media/walkthrough.mp4` for the next upload
