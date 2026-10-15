# Overview toolbar

> Owns the toolbar on the right edge of a screen in the overview: when it
> shows, what it holds, how plugins add buttons, and what a click does. The
> plugin model is [plugins.md](plugins.md); the overview itself is
> [../canvas/overview.md](../canvas/overview.md).

## Requirement

While a screen shows the overview, a vertical toolbar sits along its right
edge holding buttons that plugins register. A button runs an action, opens
a panel, or pops out a submenu; the sticky notes' colour choice is such a
submenu.

## Design considerations

- **Only in the overview**: the toolbar is for working on the canvas from
  above; in the normal view the screen belongs to windows.
- **Buttons come from plugins**, declared in their manifests
  (`toolbarButtons`), so a plugin adds one without touching the toolbar.
- **A submenu expands the toolbar** rather than opening a separate popup:
  it slides out of the toolbar beside its button, so the choice stays
  attached to what was clicked.
- **The toolbar never takes the keyboard**: the overview's keys keep
  working while it shows.

## Behavior

| Subject | Rule |
|---|---|
| Place | The right edge of each screen in the overview, vertically centred, 8 px from the edge, above the canvas and below panels |
| When | Slides in as that screen enters the overview and out as it leaves; each screen on its own |
| Button | A glyph in a 44 px square; hovering shows its label beside it |
| Order | The config's `toolbar.order`, then the rest by plugin id and manifest order |
| Action button | Runs the named action; the toolbar stays |
| Panel button | Opens the named panel to the left of the toolbar, level with the button, through the common trigger |
| Submenu button | Expands a flyout from the toolbar, to the left of the button: rows of a glyph or colour swatch and a label. Choosing an entry runs its action and folds the flyout; a second click of the button, Escape or a click outside folds it |

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

- [plugins.md](plugins.md) — manifests, actions and the common trigger.
- [notes.md](notes.md) — the note button and its colour submenu.
- [../canvas/overview.md](../canvas/overview.md) — the overview the
  toolbar belongs to.
