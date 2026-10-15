# wayland-canvas intent corpus

Style: precise, normative, table-heavy, light on prose. The
`code-style` skill's rules (no history narration, only the non-obvious,
concise) apply to every doc here exactly as to a code comment. Docs
state requirements and rationale, never a walkthrough of the code. The
`intent-docs` skill is how to write one.

**Sources.** This corpus is written from what was decided: the users'
words in the working sessions, the plan-time design prose, the plans,
and the user-facing README. It is not read off the code; a constant's
name in `src/config.h` is cited only to say where something is set. A
doc is as long as what was decided about its subject; a subject touched
once gets a short doc. A number appears only where it was decided
(finger counts, the Windows 11 title-bar metrics 32 px / 46 px,
screen-sized desktops, the dpi rule); tuning thresholds are compile-time
constants in `src/config.h`, and the doc says so rather than listing
their values.

**Cornerstones.** Two docs win when another doc disagrees with them:

- [canvas/infinite-canvas.md](canvas/infinite-canvas.md) owns the
  plane, the cameras and what normal mode means.
- [workspace/repos.md](workspace/repos.md) owns what may be committed
  and what may be pushed.

## Folders

| Folder | Holds |
|---|---|
| `intents/*.md` | What the project is and the words it uses |
| [workspace/](workspace/) | The repo, building and running |
| [canvas/](canvas/) | The plane, the desktops, the overview |
| [input/](input/) | Touchpad gestures, keyboard and pointer |
| [windows/](windows/) | Title bars and window management |
| [apps/](apps/) | The launcher, desktop entries and the top bar |
| [render/](render/) | The renderer, frame policy, hi-dpi |
| [platform/](platform/) | wlroots, protocols, outputs, the C++ standard, the control IPC |
| [bugs/](bugs/) | Bug case studies — the one place history is told |

## Docs

| Doc | Owns |
|---|---|
| [overview.md](overview.md) | What wayland-canvas is, the principles |
| [glossary.md](glossary.md) | Term disambiguation |
| [workspace/repos.md](workspace/repos.md) | The single repo, identity, pushing, what is committed, the wlroots subproject |
| [workspace/build-and-run.md](workspace/build-and-run.md) | Build deps, meson, running: TTY, nested, options, scale |
| [workspace/packaging.md](workspace/packaging.md) | The two package formats, the session file, the default-session rules per display manager, the change journal, install and uninstall guarantees |
| [canvas/infinite-canvas.md](canvas/infinite-canvas.md) | The plane, per-output cameras, 1:1 vs zoomed, the pixel grid, panning, momentum, snap, free areas, the grid |
| [canvas/desktops.md](canvas/desktops.md) | What a desktop is, membership, frames never move windows, ids, create, abandon, combine, tile |
| [canvas/overview.md](canvas/overview.md) | Entering and leaving, fit, the target model, hop semantics, the overview UI: tabs, help line, shelf, hints, pointer and key actions |
| [input/gestures.md](input/gestures.md) | The touchpad gesture table (normal vs overview), finger counts, natural direction, follow-the-fingers transitions, commit/spring-back, forwarding to clients, hold stops momentum, sloppy pinches treated as swipes |
| [input/keyboard-and-pointer.md](input/keyboard-and-pointer.md) | Bindings table, Super tap, why every canvas shortcut includes Super, pointer focus, drags, scroll, touchpad settings, nested-mode notes |
| [windows/title-bars.md](windows/title-bars.md) | Server-side title bars ("menu bars"): the Windows 11 look, buttons, states, negotiation policy, resize zones |
| [windows/window-management.md](windows/window-management.md) | Placement, focus, stacking, maximize area, fullscreen, minimize/restore paths, the tiling contract, resize anchoring, close |
| [apps/launcher.md](apps/launcher.md) | Opening, layout, search ranking, keys, pointer, launching, Terminal=true |
| [apps/desktop-entries.md](apps/desktop-entries.md) | The standard XDG entry as the way to add items, precedence, overrides, TryExec, Exec quoting, icon theme lookup order |
| [apps/bar.md](apps/bar.md) | The top bar: look, sections, widgets, click actions, autostart |
| [render/rendering.md](render/rendering.md) | The own renderer and why, repaint policy, frame callbacks, presentation, enter/leave policy, needs_frame |
| [render/hidpi.md](render/hidpi.md) | Scale choice, crispness, nested 1x |
| [platform/wlroots.md](platform/wlroots.md) | wlroots as the reference, version, subproject, the listener-balance rule, request_* handlers start the action |
| [platform/protocols.md](platform/protocols.md) | The supported protocols table: protocol → what it enables / policy |
| [platform/outputs.md](platform/outputs.md) | Outputs, layout, per-output camera, hotplug, layer-shell usable area |
| [platform/cpp-standards.md](platform/cpp-standards.md) | What the source looks like: C++26 on clang, listeners and ownership, classes, names |
| [platform/ipc.md](platform/ipc.md) | The control socket: path, line protocol, commands, the msg client |
| [bugs/screencopy-never-returned.md](bugs/screencopy-never-returned.md) | grim hung forever: a frame rendered only on own damage, never on needs_frame |
| [bugs/resize-anchor-lost-on-release.md](bugs/resize-anchor-lost-on-release.md) | Left/top resizes drifting: the client's final commit arrives after the button release |
| [bugs/grabs-that-never-ended.md](bugs/grabs-that-never-ended.md) | Fake-device tests found pointer grabs that never ended and canvas shortcuts bound without Super |

## Reading paths

- **New here**: this README → overview → glossary → workspace/repos →
  workspace/build-and-run → platform/wlroots →
  platform/cpp-standards.
- **Changing gestures or the canvas**: canvas/infinite-canvas →
  canvas/desktops → canvas/overview → input/gestures →
  input/keyboard-and-pointer.
- **Changing windows or title bars**: windows/title-bars →
  windows/window-management → render/rendering.
- **Changing the launcher**: apps/launcher → apps/desktop-entries.
- **Changing the top bar or the control IPC**: apps/bar → platform/ipc →
  apps/launcher.
- **Changing rendering**: render/rendering → render/hidpi →
  platform/outputs.
- **Adding a protocol**: platform/protocols → platform/wlroots →
  platform/outputs.

## Build plans

Plans are volatile and live in [../plans/](../plans/README.md). A plan
keeps tasks and checkboxes; what it decides is written here.
