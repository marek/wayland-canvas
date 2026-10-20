# Working on wayland-canvas

How the repo is laid out, how to build and test it, and how a release
happens. For using the compositor, start at the [guide front
page](README.md).

## Layout

Development rules and the design corpus live beside the code — see
[AGENTS.md](../../AGENTS.md): the triple helix of **intents** (what the
compositor must satisfy), **tests** and **implementation**, with plans,
skills and roles beside it.

| Folder | What |
|---|---|
| `src/` | Compositor source, one folder per area: `core/`, `canvas/`, `windows/`, `input/`, `render/`, `platform/`, `apps/` (`src/core/config.h` holds the tunables) |
| `shell/` | The Quickshell shell: the plugin host and the `WaylandCanvas.Ui` kit its plugins draw with |
| `plugins/` | The built-in plugins (`canvas.bar`, the indicators with their panels, the `canvas.theme` light/dark switch, …) the host loads beside the shell |
| `tests/` | Unit tests and headless scenario sources (`tests/scenarios/`) |
| `tools/` | `build.sh` (build), `check.sh` (build + test), `package.sh` (both packages), `install.sh`, `check-plans.sh` and `plan-items.sh` (the plans), `media.py` (the README's pictures), `readme-preview.py` (the README as GitHub shows it), `hooks/` (wired in `.claude/settings.json`) |
| `scripts/` | `agent-setup.sh` (links skills and roles into `.claude/`) |
| `protocols/` | Protocol XML used beyond wlroots' set |
| `docs/intents/` | Normative design specs (the intent corpus) |
| `docs/plans/` | Dated, tracked work plans |
| `docs/skills/` | How to work here |
| `docs/roles/` | Working personas (developer, tester, reviewer, planner) |
| `docs/guide/` | This user guide |
| `docs/media/` | The README's screenshots |
| `tmp/` | Git-ignored scratch space (`CANVAS_TMP` overrides) |
| `packaging/` | Package definitions: Arch `PKGBUILD` + install scriptlet, Debian control and maintainer scripts, the session file, the default-session tool, the source-tarball builder |

## Build and test

```sh
tools/build.sh             # configures with toolchain/llvm.ini, then compiles
tools/check.sh             # builds, then the unit and headless scenario suites
```

The dependencies, the manual meson setup and the ways to run the result are
in [Install, build and run](install.md).

## The README and its pictures

`tools/readme-preview.py` shows `README.md` as GitHub will, at a local
address it prints and opens: GitHub's own Markdown renderer and
stylesheet, the pictures and the video served from the checkout, and every
other Markdown page of the checkout rendered the same way when a link leads
there. Reload after an edit. It needs the network; with the GitHub CLI
signed in it borrows its token for the renderer's higher rate limit.

The pictures in `docs/media/` and the walkthrough video are real captures.
`tools/media.py` fetches the tour's public-domain assets, builds the
compositor and runs `build/media-tour` — the headless backend with real
apps — then encodes the stills into `docs/media/` and the video into
`tmp/media/walkthrough.mp4`. The video is a GitHub attachment: upload it
by dragging it into any issue or comment box on GitHub, and put the link
GitHub gives it on its own line under Walkthrough in `README.md`. It
needs ffmpeg, curl, Chrome, Krita, imv, mpv, foot, Neovim, Quickshell,
dbus-daemon and openssl. `--stills` remakes only the four stills; `--keep` keeps the run's
scratch folder for a look; `--record` captures the web pages again.
Scratch lives under `tmp/media/` (`$CANVAS_TMP` overrides `tmp/`): the
fetched assets, the tour's run folder, and `pages/`, the captured web pages
(`tools/media/pages.tsv` lists them). A page is captured once and replayed
on every later run, so the pictures keep the day it was captured; delete
its files in `pages/` (or pass `--record`) to capture it again.

## Releases

Pushing a `v<version>` tag builds both packages on GitHub Actions — the same
`tools/package.sh` run inside an `archlinux` and a `debian:sid` container —
and drafts a release from them. The tag is the version: nothing is bumped
before tagging, and a build between tags is
`<version>.r<commits>.g<hash>` (`tools/version.sh`).

## See also

[Install, build and run](install.md) for building and running as a user, and
[The shell and its plugins](shell.md) for the `shell/` and `plugins/` code.
