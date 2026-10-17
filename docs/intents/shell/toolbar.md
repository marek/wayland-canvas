# Overview toolbar

> Owns the toolbar on an edge of a screen in the overview: where and when it
> shows, what it holds, how plugins add buttons, and what a click does. The
> plugin model is [plugins.md](plugins.md); the overview itself is
> [../canvas/overview.md](../canvas/overview.md).

## Requirement

While a screen shows the overview, a toolbar sits along one of its edges —
the right by default, else the left, the top or the bottom, as the user
chooses — holding buttons that plugins register. A button runs an action,
opens a panel, or pops out a submenu; the sticky notes' colour choice is such
a submenu, and the settings gear opens the settings window
([settings.md](settings.md)).

## Design considerations

- **Only in the overview**: the toolbar is for working on the canvas from
  above; in the normal view the screen belongs to windows.
- **Buttons come from plugins**, declared in their manifests
  (`toolbarButtons`), so a plugin adds one without touching the toolbar.
- **A submenu expands the toolbar** rather than opening a separate popup:
  it slides out of the toolbar beside its button, so the choice stays
  attached to what was clicked.
- **The edge is the user's**, and whatever opens from the toolbar opens
  toward the screen's centre on every edge, so it stays on screen.
- **The toolbar never takes the keyboard**: the overview's keys keep
  working while it shows, and Escape in the overview therefore leaves it
  — the overview is Escape's to act on, and leaving folds the flyout,
  the panel and the toolbar together.

## Behavior

| Subject | Rule |
|---|---|
| Place | The edge the shell config's `toolbar.position` names ([plugins.md](plugins.md)), right by default: on the right or left edge a column centred vertically, on the top or bottom a row centred horizontally, 8 px from the edge — the top one below the bar. Above the canvas and below panels (the surface's namespace: `wayland-canvas-toolbar`). A new edge applies at once |
| When | Slides in as that screen enters the overview and out as it leaves; each screen on its own |
| Below it | In the overview, the compositor's shelf and help line stay clear of a bottom toolbar ([../canvas/overview.md](../canvas/overview.md)) |
| Button | A glyph in a 44 px square; hovering shows its label beside it, toward the screen's centre |
| Order | The config's `toolbar.order`, then the rest by plugin id and manifest order |
| Action button | Runs the named action; the toolbar stays |
| Panel button | Opens the named panel on the toolbar's inner side, toward the screen's centre, level with the button and kept on the screen, through the common trigger |
| Submenu button | Expands a flyout from the toolbar on its inner side, level with the button: rows of a glyph or colour swatch and a label. Choosing an entry runs its action and folds the flyout; a second click of the button or a click outside folds it (Escape is not the toolbar's to catch: it never takes the keyboard, so in the overview Escape leaves the overview, folding everything) |

A toolbar button's manifest entry:

| Key | Meaning |
|---|---|
| `id`, `icon`, `label` | Its name, glyph and label |
| `action`, `args` | The action it runs, with its arguments |
| `panel` | The panel it opens |
| `menu` | Its submenu: entries of `label`, `icon` or `color`, `action`, `args` |

A button names exactly one of `action`, `panel` and `menu`.

## Error states, corner cases

- A button naming neither or more than one of `action`, `panel`, `menu`:
  skipped, logged with its plugin.
- The screen leaves the overview with a flyout or panel open: both close
  with the toolbar.
- No plugin registers a button: the toolbar does not show.

## Cross-links

- [plugins.md](plugins.md) — manifests, actions, the common trigger and the
  shell config.
- [notes.md](notes.md) — the note button and its colour submenu.
- [settings.md](settings.md) — the settings gear, and the window that picks
  the edge.
- [../canvas/overview.md](../canvas/overview.md) — the overview the
  toolbar belongs to.
