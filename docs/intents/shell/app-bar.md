# App bar

> Owns the bar of open apps in the overview: where it sits, what it holds,
> what a click does, and how it shares an edge with the toolbar. The toolbar
> it may share with is [toolbar.md](toolbar.md); the window list and the jump
> it calls are [../platform/ipc.md](../platform/ipc.md)'s.

## Requirement

While a screen shows the overview, a bar along one of its edges — the left
by default — shows every open app, one entry each, and clicking an entry
goes to that app's window: to the desktop the window is on, or to a view
centred on the window when it is on no desktop. The bar never launches
anything, and the user can turn it off.

## Design considerations

- **Seeing, not launching.** The canvas puts windows where the user left
  them, and a window out of view is otherwise reachable only by hunting
  the overview. The app bar answers "what is open, and where" — launching
  stays the launcher's ([../apps/launcher.md](../apps/launcher.md)), so
  the bar lists nothing that is not running.
- **Built like the bar.** It is a plugin with one surface per output
  (`canvas.apps`, [plugins.md](plugins.md)), holding entries the way
  `canvas.bar` holds widgets: the same UI kit, the same theme, the same
  live config.
- **Opposite the toolbar by default**, so the two do not fight for an
  edge: the toolbar's default is the right, the app bar's the left. On one
  edge they share a strip rather than overlapping, the apps last, because
  the toolbar's buttons are the fixed vocabulary and the apps come and go.
- **The compositor owns going there.** A plugin cannot move the view, so
  the jump is one control-socket call and the view decision — desktop or
  centred — is the compositor's, the same decision the overview's shelf
  makes for a minimized window
  ([../canvas/overview.md](../canvas/overview.md)).
- **Off is a setting, not a removal.** The user who does not want it
  switches it off in the settings window ([settings.md](settings.md));
  the plugin stays loaded and the switch applies at once.

## Behavior

| Subject | Rule |
|---|---|
| Place | The edge the shell config's `appbar.position` names, left by default: on the left or right edge a column centred vertically, on the top or bottom a row centred horizontally, 8 px from the edge — a top one below the bar. Above the canvas and below panels (the surface's namespace: `wayland-canvas-app-bar`). A new edge applies at once |
| When | Slides in as that screen enters the overview and out as it leaves, each screen on its own, exactly as the toolbar does |
| Off | `appbar.enabled` false: no surface on any screen |
| Entries | One per open app, in the order the apps' first windows opened; the app's icon and its name from the desktop entry its app id names ([../apps/desktop-entries.md](../apps/desktop-entries.md)), else the app id's first letter in its place. A dot under the mark, a second one for an app holding more than one window; the focused app's mark takes the accent, a wholly minimized app's is dimmed |
| Hovering | The app's name beside the entry, toward the screen's centre, as a toolbar button's label |
| Click | One window: the view goes straight there. Several: the entry unfolds them (below) |
| Going to a window | The compositor focuses it and moves the view — to its desktop at that desktop's scale, else to a view centred on the window — leaving the overview as a shelf click does |
| Windows of an app | A card beside the entry, one margin from the strip toward the screen's centre and kept on screen: a row per window with its title, the focused one marked, a minimized one saying so. A click on a row goes to that window; a second click of the entry, a click outside it, or leaving the overview folds the card |
| Minimized | An app whose windows are all minimized is still listed, marked as minimized; a click restores the window it goes to |
| Shared edge | `appbar.position` equal to `toolbar.position`: one strip holds the toolbar's buttons first, then a separator — a short hairline across the strip, in the panel's border colour — then the apps. The strip stays centred on its edge as one unit, and whatever opens from a toolbar button opens toward the screen's centre as before |

### What the compositor provides

The bar holds no state of its own: it draws the control socket's window
list and follows its event, and a click is one `window.focus` call
([../platform/ipc.md](../platform/ipc.md)). The view a jump lands on is
[../windows/window-management.md](../windows/window-management.md)'s
restore path.

## Error states, corner cases

- **Nothing open**: the bar does not show, as a toolbar with no buttons
  does not.
- **An app with no desktop entry**: its app id is its name and it takes
  the generic glyph; it is never left out.
- **A window closed between the draw and the click**: the call answers
  that the window is gone, the entry leaves with the next event, and the
  view does not move.
- **More entries than the edge holds**: the strip is clamped to the
  screen and the entries shrink to a minimum, then the list scrolls with
  the wheel.
- **The screen leaves the overview**: the bar goes with it, together with
  the toolbar when they share the edge.

## Cross-links

- [toolbar.md](toolbar.md) — the toolbar, its edges and the strip they
  share.
- [plugins.md](plugins.md) — the plugin, its surface and the shell config.
- [settings.md](settings.md) — the page that sets the edge and the switch.
- [../platform/ipc.md](../platform/ipc.md) — the window list, its event and
  `window.focus`.
- [../canvas/overview.md](../canvas/overview.md) — the overview the bar
  belongs to, and the shelf whose click it mirrors.
- [../windows/window-management.md](../windows/window-management.md) — the
  restore path a jump takes.
