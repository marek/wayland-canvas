# Desktop entries

> Owns where app entries come from and how they are read: the
> directories, precedence, filtering, the Exec rules, and the icon
> lookup order. The launcher built on them is in
> [launcher.md](launcher.md).

## Requirement

Adding an item to the launcher is done the standard desktop way: an
XDG `.desktop` file in `~/.local/share/applications`. Nothing is
registered anywhere else, and the launcher picks it up the next time
it opens.

## Behavior

| Rule | Behaviour |
|---|---|
| Directories | `$XDG_DATA_HOME/applications` and every `$XDG_DATA_DIRS/applications` |
| Re-read | on every open of the launcher — there is no cache to invalidate |
| Precedence | the user's copy of an entry wins over the system one with the same desktop-file id |
| Hiding | `NoDisplay=true` in the winning copy keeps the entry out of the list |
| TryExec | an entry whose TryExec binary is missing is not listed |
| Terminal | `Terminal=true` runs the entry inside the terminal command (`${TERMINAL:-foot}`) |
| Exec quoting | quoted arguments in Exec allow double quotes only |

Icon lookup, in order:

1. The user's icon theme, read from gsettings in the compositor's own
   process: the loop never waits on a program for it.
2. That theme's `Inherits` chain.
3. `hicolor`.
4. Pixmaps.

Within a theme, directories of the Applications context come first;
the remaining theme directories serve icons stored elsewhere in the
theme.

Writing an entry: a `.desktop` file per the XDG specification, placed
in `~/.local/share/applications` and checked with
`desktop-file-validate`.

## Design considerations

- **The standard is the whole point** — an entry written for any
  desktop environment works here unchanged, and what the launcher
  lists is exactly what the desktop standards say is installed.
- **Re-reading on open keeps the launcher honest** for the price of
  one directory scan; a cached list would need invalidation hooks
  that a file drop has none of.

## Error states, corner cases

- `index.theme` directory lists are comma-separated; a reader that
  splits them on the GKeyFile default `;` finds no theme directories
  at all and only pixmaps icons appear.
- An entry that fails its rules — hidden, or TryExec missing — is
  simply absent from the list; it is not an error to surface.

## Cross-links

- [launcher.md](launcher.md) — opening, ranking, keys, launching.
- [../workspace/build-and-run.md](../workspace/build-and-run.md) —
  running the compositor that hosts the launcher.
