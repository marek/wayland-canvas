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
| `tools/` | `build.sh` (build), `check.sh` (build + test), `package.sh` (both packages), `install.sh`, `check-plans.sh`, hooks |
| `scripts/` | `agent-setup.sh` (links skills, roles and hooks into `.claude/`) |
| `protocols/` | Protocol XML used beyond wlroots' set |
| `docs/intents/` | Normative design specs (the intent corpus) |
| `docs/plans/` | Dated, tracked work plans |
| `docs/skills/` | How to work here |
| `docs/roles/` | Working personas (developer, tester, reviewer, planner) |
| `docs/guide/` | This user guide |
| `docs/media/` | The README's screenshots and video |
| `tmp/` | Git-ignored scratch space (`CANVAS_TMP` overrides) |
| `packaging/` | Package definitions: Arch `PKGBUILD` + install scriptlet, Debian control and maintainer scripts, the session file, the default-session tool, the source-tarball builder |

## Build and test

```sh
tools/build.sh             # configures with toolchain/llvm.ini, then compiles
tools/check.sh             # builds, then the unit and headless scenario suites
```

The dependencies, the manual meson setup and the ways to run the result are
in [Install, build and run](install.md).

## Releases

Pushing a `v<version>` tag builds both packages on GitHub Actions — the same
`tools/package.sh` run inside an `archlinux` and a `debian:sid` container —
and drafts a release from them. The tag is the version: nothing is bumped
before tagging, and a build between tags is
`<version>.r<commits>.g<hash>` (`tools/version.sh`).

## See also

[Install, build and run](install.md) for building and running as a user, and
[The shell and its plugins](shell.md) for the `shell/` and `plugins/` code.
