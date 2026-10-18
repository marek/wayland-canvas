# wayland-canvas intent corpus

Style: precise, normative, table-heavy, light on prose. The
`code-style` skill's rules (no history narration, only the non-obvious,
concise) apply to every doc here exactly as to a code comment. Docs
state requirements and rationale, never a walkthrough of the code. The
`intent-docs` skill is how to write one.

**Sources.** This corpus is written from what was decided: the users'
words in the working sessions, the plan-time design prose, the plans,
and the user-facing README. It is not read off the code; a constant's
name in `src/core/config.h` is cited only to say where something is set. A
doc is as long as what was decided about its subject; a subject touched
once gets a short doc. A number appears only where it was decided
(finger counts, the Windows 11 title-bar metrics 32 px / 46 px,
screen-sized desktops, the dpi rule); tuning thresholds are compile-time
constants in `src/core/config.h`, and the doc says so rather than listing
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
| [input/](input/) | Touchpad gestures, keyboard and pointer, the config file |
| [windows/](windows/) | Title bars and window management |
| [apps/](apps/) | Applications: the launcher, the desktop entries it finds them by, and the capture UI |
| [shell/](shell/) | The desktop shell and its plugins: the top bar, the plugin model, the overview toolbar, the app bar, sticky notes, light and dark, the settings window |
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
| [workspace/session-services.md](workspace/session-services.md) | What a graphical login starts besides the compositor, and when it stops |
| [workspace/packaging.md](workspace/packaging.md) | The two package formats, the session file, the default-session rules per display manager, the change journal, install and uninstall guarantees |
| [canvas/infinite-canvas.md](canvas/infinite-canvas.md) | The plane, per-output cameras, 1:1 vs zoomed, the pixel grid, panning and hops, free areas, the grid |
| [canvas/desktops.md](canvas/desktops.md) | What a desktop is, membership, frames never move windows, ids, create, abandon, combine, tiling slices |
| [canvas/overview.md](canvas/overview.md) | Entering and leaving, fit, the target model, hop semantics, the overview UI: tabs, help line, shelf, hints, pointer and key actions |
| [input/gestures.md](input/gestures.md) | The touchpad gesture table (normal vs overview), the swipe presets and the pinch switch, finger counts, hops going where the fingers point, follow-the-fingers transitions, commit/spring-back, forwarding to clients, sloppy pinches treated as swipes |
| [input/keyboard-and-pointer.md](input/keyboard-and-pointer.md) | Bindings table (every action's name and default keys), Super tap, why every default canvas shortcut includes Super, pointer focus, drags, scroll, touchpad settings, nested-mode notes |
| [input/config-file.md](input/config-file.md) | `canvas.conf`: place, first run, sections, chords, resolution against the defaults, reload, errors, edits |
| [windows/title-bars.md](windows/title-bars.md) | Server-side title bars ("menu bars"): the Windows 11 look, buttons, states, negotiation policy, resize zones |
| [windows/x11.md](windows/x11.md) | X11 applications through Xwayland: the window as a view, the two shell halves, what is out of scope for now |
| [windows/window-management.md](windows/window-management.md) | Placement, focus, stacking, maximize area, fullscreen, minimize/restore paths, the tiling contract, resize anchoring, close |
| [apps/launcher.md](apps/launcher.md) | Opening, layout, search ranking, keys, pointer, launching, Terminal=true |
| [apps/capture.md](apps/capture.md) | Shots and recordings: the key chords, the region/window/output picker, the toolbar, the thumbnail, the recorder |
| [apps/desktop-entries.md](apps/desktop-entries.md) | The standard XDG entry as the way to add items, precedence, overrides, TryExec, Exec quoting, icon theme lookup order |
| [shell/bar.md](shell/bar.md) | The top bar: look, sections, widgets, click actions, panels, autostart |
| [shell/plugins.md](shell/plugins.md) | What a plugin is, where plugins are found, the config, the common trigger, the API, the UI kit, the built-in plugins |
| [shell/toolbar.md](shell/toolbar.md) | The overview toolbar: its edge, when it shows, buttons, actions, panels and submenus |
| [shell/app-bar.md](shell/app-bar.md) | The app bar in the overview: its edge, the open apps it lists, the jump to a window, the strip it shares with the toolbar |
| [shell/settings.md](shell/settings.md) | The settings window: the gear, its pages, recording a shortcut, what each page writes |
| [shell/notifications.md](shell/notifications.md) | Notifications: the popups, their actions and expiry, the history and its bar widget, the silence switch |
| [shell/password-dialog.md](shell/password-dialog.md) | The password dialog for an app asking for admin rights: the polkit agent, what it shows, the keyboard, cancel |
| [shell/lock.md](shell/lock.md) | The lock screen: what a locked session shows and blocks, the triggers and the idle timeout, the locker plugin and its PAM service |
| [shell/notes.md](shell/notes.md) | Sticky notes: creating from the toolbar, colours, look, editing, moving, deleting, keeping |
| [shell/theme.md](shell/theme.md) | The light/dark switch, the two palettes, what follows the mode, keeping it |
| [render/rendering.md](render/rendering.md) | The own renderer and why, repaint policy, frame callbacks, presentation, enter/leave policy, needs_frame |
| [render/night-light.md](render/night-light.md) | The night light: the switch and the temperature, the gamma table and the in-frame fallback, the gamma-control protocol |
| [render/hidpi.md](render/hidpi.md) | Scale choice, crispness, nested 1x |
| [platform/wlroots.md](platform/wlroots.md) | wlroots as the reference, version, subproject, the listener-balance rule, request_* handlers start the action |
| [platform/protocols.md](platform/protocols.md) | The supported protocols table: protocol → what it enables / policy |
| [platform/outputs.md](platform/outputs.md) | Outputs, layout, per-output camera, hotplug, layer-shell usable area |
| [platform/cpp-standards.md](platform/cpp-standards.md) | What the source looks like: C++26 on clang, listeners and ownership, classes, names |
| [platform/ipc.md](platform/ipc.md) | The control socket: path, JSON-lines protocol, methods, events, state, surfaces on the canvas, the msg client |
| [bugs/screencopy-never-returned.md](bugs/screencopy-never-returned.md) | grim hung forever: a frame rendered only on own damage, never on needs_frame |
| [bugs/resize-anchor-lost-on-release.md](bugs/resize-anchor-lost-on-release.md) | Left/top resizes drifting: the client's final commit arrives after the button release |
| [bugs/grabs-that-never-ended.md](bugs/grabs-that-never-ended.md) | Fake-device tests found pointer grabs that never ended and canvas shortcuts bound without Super |
| [bugs/zoom-collapsed-the-grid.md](bugs/zoom-collapsed-the-grid.md) | Zooming a tiled desktop in and out left windows as bars at the screen's edge: a settled scale re-flowed the grid and its shrinking frame dropped them |
| [bugs/notes-that-never-followed.md](bugs/notes-that-never-followed.md) | Sticky notes inert on screen while their files changed: plain JS objects that told QML nothing, drags the plugin ran against its own coordinates, and checks that read only the storage |

## Reading paths

- **New here**: this README → overview → glossary → workspace/repos →
  workspace/build-and-run → platform/wlroots →
  platform/cpp-standards.
- **Changing gestures or the canvas**: canvas/infinite-canvas →
  canvas/desktops → canvas/overview → input/gestures →
  input/keyboard-and-pointer.
- **Changing shortcuts, gestures or the settings window**:
  input/config-file → input/keyboard-and-pointer → input/gestures →
  shell/settings → shell/toolbar.
- **Changing windows or title bars**: windows/title-bars →
  windows/window-management → render/rendering.
- **Changing the launcher**: apps/launcher → apps/desktop-entries.
- **Changing the top bar or the control IPC**: shell/bar → shell/plugins →
  platform/ipc → apps/launcher.
- **Changing the app bar**: shell/app-bar → shell/toolbar →
  shell/plugins → platform/ipc → windows/window-management.
- **Changing rendering**: render/rendering → render/hidpi →
  platform/outputs.
- **Adding a protocol**: platform/protocols → platform/wlroots →
  platform/outputs.

## Build plans

Plans are volatile and live in [../plans/](../plans/README.md). A plan
keeps tasks and checkboxes; what it decides is written here.
