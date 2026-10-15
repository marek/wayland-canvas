# Plugin framework

Date: 2026-10-15 12:44 EDT

## Context

The shell becomes a host of plugins: independent folders in a plugin directory, each with a manifest, loaded because
they are there, configured in `shell.json`, and written against one API. The top bar, each indicator with its panel,
the overview toolbar on the right edge (shown only in the overview, holding buttons that run an action, open a panel
or expand a submenu), sticky notes with a colour choice, and a light/dark switch in the bar's top-right corner are all
plugins. The compositor's half of the API is the control socket, which moves to JSON lines with methods, state and
events, and gains canvas items (what a sticky note is on the canvas) and the theme mode. What each part is:
[apps/plugins.md](../intents/apps/plugins.md), [apps/toolbar.md](../intents/apps/toolbar.md),
[apps/notes.md](../intents/apps/notes.md), [apps/theme.md](../intents/apps/theme.md),
[platform/ipc.md](../intents/platform/ipc.md).

## Compositor API

- [ ] JSON lines on the control socket (json-glib): requests with ids, responses, errors, subscriptions and events; the packages and build docs name the dependency
- [ ] Methods and events as the IPC intent lists them, the state report, and `wayland-canvas msg <method> [params]` with `msg subscribe`
- [ ] Canvas items: drawn above windows in canvas space, dragged in the normal view and the overview, double-click reported
- [ ] The compositor's two palettes (title bars, canvas, overview UI, launcher, hints), switched by `theme.set`

## Plugin host

- [ ] Discovery in the built-in, user and configured directories, manifests, `apiVersion`, enabling, settings, storage, reload on change
- [ ] The API object: plugin, storage, panels (the common trigger), actions, theme, compositor
- [ ] The UI kit as the `WaylandCanvas.Ui` module the plugins import
- [ ] The bar, every widget and every panel moved into plugins, behaviour unchanged

## Plugins

- [ ] `canvas.toolbar`: the toolbar on the right edge in the overview, with action, panel and submenu buttons from every manifest
- [ ] `canvas.notes`: the note button with its colour submenu, notes as canvas items, the editor over a note, moving, recolouring, deleting, kept across restarts
- [ ] `canvas.theme`: the switch in the bar's top-right corner, both palettes for the shell, the compositor told
- [ ] (the user to decide) Whether the switch also sets the applications' light/dark preference, a system-wide setting other sessions share

## Verification

- [ ] The IPC scenario covers the methods, errors, events and canvas items
- [ ] A scenario shows the toolbar only in the overview, runs an action, opens a panel and a submenu from it
- [ ] A scenario creates a note from the toolbar, moves it, and finds it again after a shell restart
- [ ] The switch flips the shell and the compositor, seen in screenshots of both modes
- [ ] Screenshots of the toolbar, the colour submenu, notes in the normal view and the overview, and both modes, inspected
- [ ] Warning-free build, every suite green, the intents and README name the plugin layout
