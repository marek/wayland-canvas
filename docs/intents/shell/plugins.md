# Plugins

> Owns the shell's plugin model: what a plugin is, where plugins are
> found, the config that enables them, the places they contribute to and
> the API they are written against. The compositor's half of that API is
> [../platform/ipc.md](../platform/ipc.md).

## Requirement

Everything the shell shows is a plugin: the top bar, each indicator and
its panel, the overview toolbar, sticky notes, the light/dark switch. A
plugin is an independent folder of QML and JavaScript, loaded because it
exists in a plugin directory or because the config names its directory,
and written against a documented API without changing the shell or the
compositor.

## Design considerations

- **Plugins run in the shell, not the compositor.** The shell (Quickshell:
  QML and JavaScript) loads them at run time. A plugin that fails to load
  is skipped and the rest keep working; nothing a plugin does can take the
  compositor down. What plugins need from the compositor goes over its
  control socket.
- **Presence is enough.** A folder with a valid manifest is loaded; the
  config only disables plugins, hides and orders contributions, adds
  directories and sets settings.
- **Plugins contribute to named places and call the API**; they never
  reach into another plugin. Everything a plugin contributes has a global
  name, `<plugin id>/<item id>`.
- **The bar is a plugin like any other**, so it can be replaced.

## Behavior

### A plugin

| Part | Rule |
|---|---|
| Folder | `<plugin directory>/<plugin id>/`; the id is lowercase and dotted (`canvas.network`) and equals the folder name |
| Manifest | `plugin.json`: `id`, `name`, `version`, `apiVersion`, `description`, `contributes`, `settings` (the declarations below) |
| Code | The QML components and JavaScript files the manifest names, relative to the folder |

What `contributes` may hold:

| Key | Each entry | Shown |
|---|---|---|
| `surfaces` | `id`, `entry`, `perOutput` | Top-level windows the plugin owns (the bar, the overview toolbar, note windows) |
| `barWidgets` | `id`, `entry`, `section` (`left`, `center`, `right`), `order`, optional `panel` | In the bar, whichever plugin provides it |
| `panels` | `id`, `entry` | Through the common trigger; the id names the panel's layer surface, `wayland-canvas-panel-<id>` |
| `toolbarButtons` | `id`, `icon`, `label`, and one of `action`, `panel`, `menu` ([toolbar.md](toolbar.md)) | In the overview toolbar |
| `actions` | `id`, `entry` (a component whose `run(args)` performs it) | Wherever a button, a menu entry or the control socket's `action.run` names it |

`apiVersion` is an integer. A plugin asking for a newer API than the shell
offers is not loaded.

### Settings a plugin declares

`settings` holds one entry per setting the plugin reads, and the settings
window builds its Plugins page from the entries alone
([settings.md](settings.md)), so no plugin's own choices need code in the
settings window. An entry is the default value on its own, or an object:

| Key | Meaning |
|---|---|
| `default` | The value when the config overrides nothing |
| `type` | `switch`, `choice`, `number` or `text`; without a type the setting is the plugin's own and shows nowhere |
| `label`, `description` | Its row's label and the smaller line under it |
| `group` | The section header it sits under, in declaration order |
| `choices` | For a `choice`: entries of `value`, `label` and an optional `description` |
| `min`, `max`, `step` | For a `number` |
| `order` | Its place within its group; equal orders keep declaration order |

A setting written from the window goes through `api.plugin.setSetting`,
which writes `plugins.<id>.settings` in the shell config and applies it at
once. A slider writes once, when the drag ends, not on every motion of it.

### Where plugins are found

In this order; a plugin in a later directory replaces one with the same id
in an earlier one:

1. Built in: beside the shell — `<shell dir>/../plugins`, which is `plugins/`
   in the source tree and `<datadir>/wayland-canvas/plugins` when installed.
2. The user's: `$XDG_DATA_HOME/wayland-canvas/plugins` (default
   `~/.local/share/wayland-canvas/plugins`).
3. Each path in the config's `pluginPaths`.

### Config

`$XDG_CONFIG_HOME/wayland-canvas/shell.json` (default
`~/.config/wayland-canvas/shell.json`); every key is optional.

| Key | Meaning |
|---|---|
| `pluginPaths` | Extra plugin directories |
| `plugins.<id>.enabled` | `false` disables a found plugin |
| `plugins.<id>.settings` | Overrides of that plugin's setting defaults |
| `bar.layout.left`, `.center`, `.right` | Bar widget names in order; widgets not listed follow in their declared section and order |
| `bar.hidden` | Bar widget names (`canvas.agents/agents`) left out of the bar |
| `toolbar.order` | Toolbar button names in order |
| `toolbar.position` | The toolbar's edge: `right` (the default), `left`, `top`, `bottom` ([toolbar.md](toolbar.md)) |
| `appbar.position` | The app bar's edge: `left` (the default), `right`, `top`, `bottom`; the toolbar's own edge makes one shared strip ([app-bar.md](app-bar.md)) |
| `appbar.enabled` | `false` leaves the app bar off every screen |

Edits to the config, a plugin folder or a plugin's files apply while the
shell runs: the shell reloads with them. What the host writes itself — a
plugin setting, the mode, a change from the settings window
([settings.md](settings.md)) — applies at once without a reload.

### The common trigger

Opening a panel is one call, `api.panels.toggle(<panel name>, <anchor
item>)`; any widget, button or action can open any plugin's panel. One panel
is open at a time, and it closes on a click outside it, on Escape or on a
second toggle. A bar widget whose manifest names a `panel` opens it on click
without code of its own. A toolbar button names an `action` (it runs), a
`panel` (it opens beside the toolbar) or a `menu` (a list of label and
action entries opening beside the toolbar).

An action also runs from outside the shell: the compositor's `action.run`
method pushes an `action` event, and the host runs the named plugin
action with its args ([../platform/ipc.md](../platform/ipc.md)) — so a
script or a keybinding can create a note without speaking to the shell.

### The API

Every component a plugin contributes receives `api` as it is created, so
its first bindings and its `Component.onCompleted` already have it:

| Member | Offers |
|---|---|
| `api.plugin` | The manifest; `settings`, the defaults merged with the config; `setSetting(key, value)`, which writes the config |
| `api.storage` | A directory of the plugin's own under `$XDG_STATE_HOME/wayland-canvas/plugins/<id>/`: `read(name)` and `write(name, value)` for JSON, `readText(name)` and `writeText(name, text)` for any other file, `list(callback)` with the names of its files, and `remove(name)` |
| `api.panels` | `toggle`, `toggle`'s anchor re-aims an already-open panel (another tray icon's menu), `open`, `close`, and the name of the open panel |
| `api.actions` | `run(<action name>, args)` |
| `api.theme` | `mode` (`light` or `dark`), `colors` for that mode, `setMode(mode)` ([theme.md](theme.md)) |
| `api.canvas` | A surface of the plugin's own on the plane: `place(<surface>, {x, y, width, height})` and `unplace(<surface>)`, in canvas coordinates, and for a drag the plugin hands to the compositor on a press, `move(<surface>, callback)` and `resize(<surface>, {edges, minWidth, minHeight}, callback)`, each answered with the rectangle at the release ([../platform/ipc.md](../platform/ipc.md)). What a plugin draws there is its own — the compositor places it and nothing more |
| `api.compositor` | The compositor's state (outputs and whether each shows the overview, desktops), `call(method, params, callback)` with its result or error, and its events ([../platform/ipc.md](../platform/ipc.md)) |
| `api.shell` | The shell config, live: `config`, `configPath`; every bar widget, hidden ones flagged (`barWidgets`: name, plugin name, section, order, hidden), `barHidden(name)`, `setBarHidden(name, hidden)`; `toolbarPosition`, `setToolbarPosition(edge)`. The host writes `shell.json` and applies the change at once, without a reload ([settings.md](settings.md)) |

A surface component is also handed the registry of everything loaded — the
ordered bar widgets and toolbar buttons with their components — because a
surface (the bar, the toolbar) hosts other plugins' contributions; the bar
follows `api.shell.barWidgets`, so a widget hidden or shown applies at once.

Plugins draw with the shell's UI kit, a QML module they import as
`WaylandCanvas.Ui`: the bar button, the panel card and its rows, sliders,
switches, tabs, choice rows, key chips, text and fields, all coloured from
`api.theme`.

### Built-in plugins

| Id | Contributes |
|---|---|
| `canvas.bar` | The top bar surface, one per output, hosting every bar widget |
| `canvas.launcher` | The logo widget, which opens the launcher |
| `canvas.clock`, `canvas.weather`, `canvas.tray`, `canvas.network`, `canvas.bluetooth`, `canvas.audio`, `canvas.brightness`, `canvas.power`, `canvas.agents` | A bar widget each, with its panel |
| `canvas.theme` | The light/dark switch, the right-most bar widget |
| `canvas.toolbar` | The overview toolbar surface, hosting every toolbar button |
| `canvas.apps` | The app bar surface, one per output: every open app, and the jump to its window ([app-bar.md](app-bar.md)) |
| `canvas.notes` | Sticky notes: a toolbar button with a colour menu, and the notes |
| `canvas.settings` | The settings window: the gear toolbar button and its panel ([settings.md](settings.md)) |
| `canvas.notifications` | The notification server: the popups, the bell widget with its history panel, and the dismiss and silence actions ([notifications.md](notifications.md)) |
| `canvas.polkit` | The password dialog an app asking for admin rights gets ([password-dialog.md](password-dialog.md)) |
| `canvas.lock` | The lock screen: the locker over every output and the `lock` action every trigger runs ([lock.md](lock.md)) |

## Error states, corner cases

- An invalid manifest, a too-new `apiVersion` or a QML error while loading:
  that plugin is skipped, with a log line naming it and the reason.
- A config that is not valid JSON: the shell runs on the defaults and logs
  the reason, and nothing writes the file while it fails to parse; a
  change the host would have written (a setting, the mode) holds for the
  running shell only.
- A widget or button naming a panel or action that does not exist: the
  click does nothing, logged once.
- A plugin disabled or removed while its panel is open: the panel closes.
- No bar plugin loaded: the compositor runs without a bar; the launcher and
  the overview stay reachable by keyboard.

## Cross-links

- [../platform/ipc.md](../platform/ipc.md) - the compositor API over the
  control socket.
- [bar.md](bar.md) - the bar and its widgets.
- [app-bar.md](app-bar.md) - the app bar, a surface plugin like the bar.
- [settings.md](settings.md) - the window that renders declared settings.
- [../apps/launcher.md](../apps/launcher.md).
