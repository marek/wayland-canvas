# Plugin framework

Date: 2026-10-15 12:44 EDT

## Context

The shell becomes a host of plugins: independent folders in a plugin directory, each with a manifest, loaded because
they are there, configured in `shell.json`, and written against one API. The top bar, each indicator with its panel,
the overview toolbar on the right edge (shown only in the overview, holding buttons that run an action, open a panel
or expand a submenu), sticky notes with a colour choice, and a light/dark switch in the bar's top-right corner are all
plugins. The compositor's half of the API is the control socket, which moves to JSON lines with methods, state and
events, and gains canvas items (what a sticky note is on the canvas) and the theme mode. What each part is:
[shell/plugins.md](../intents/shell/plugins.md), [shell/toolbar.md](../intents/shell/toolbar.md),
[shell/notes.md](../intents/shell/notes.md), [shell/theme.md](../intents/shell/theme.md),
[platform/ipc.md](../intents/platform/ipc.md).

## Compositor API

- [x] JSON lines on the control socket (json-glib): requests with ids, responses, errors, subscriptions and events; the packages and build docs name the dependency
- [x] Methods and events as the IPC intent lists them (the canvas items' aside), the state report, and `wayland-canvas msg <method> [params]` with `msg subscribe`
- [x] Canvas items: drawn above windows in canvas space, dragged in the normal view and the overview, double-click reported; their methods, events and state
- [x] The compositor's two palettes (title bars, canvas, overview UI, launcher, hints), switched by `theme.set`
- [x] In the overview, shell surfaces above the windows (the bar, the toolbar, panels, the note editor) take the pointer, and one holding the keyboard takes the keys before the overview's own

## Plugin host

- [x] Discovery in the built-in, user and configured directories, manifests, `apiVersion`, enabling, settings, storage, reload on change
- [x] The API object: plugin, storage, panels (the common trigger), actions, theme, compositor
- [x] The UI kit as the `WaylandCanvas.Ui` module the plugins import
- [x] The bar, every widget and every panel moved into plugins, behaviour unchanged
- [x] Reload by change notifications: no timer or process runs while the shell sits idle

## Plugins

- [ ] `canvas.toolbar`: the toolbar on the right edge in the overview, with action, panel and submenu buttons from every manifest
- [ ] `canvas.notes`: the note button with its colour submenu, notes as canvas items, the editor over a note, moving, recolouring, deleting, kept across restarts
- [ ] `canvas.theme`: the switch in the bar's top-right corner, both palettes for the shell, the compositor told
- [ ] Light mode keeps text readable on accent-coloured tabs and buttons, in the compositor and the shell
- [ ] (the user to decide) Whether the switch also sets the applications' light/dark preference, a system-wide setting other sessions share

## Verification

- [x] The IPC scenario covers the methods, errors, events and canvas items
- [ ] A scenario shows the toolbar only in the overview, runs an action, opens a panel and a submenu from it
- [ ] A scenario creates a note from the toolbar, moves it, and finds it again after a shell restart
- [ ] The switch flips the shell and the compositor, seen in screenshots of both modes
- [ ] Screenshots of the toolbar, the colour submenu, notes in the normal view and the overview, and both modes, inspected
- [ ] Warning-free build, every suite green, the intents and README name the plugin layout
