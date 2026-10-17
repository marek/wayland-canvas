# App launcher

> Owns opening apps from the desktop: how the launcher opens, what it
> lists, the ranking, its keys and pointer behaviour. Where the
> entries come from is in [desktop-entries.md](desktop-entries.md);
> where launched windows land is in
> [../windows/window-management.md](../windows/window-management.md).

## Requirement

Any installed app opens from the keyboard: Super+Space and start typing.
The launcher is Windows 11 in style — a centred dark panel with a search
field and the app list.

## Behavior

Opening:

The launcher opens on Super+Space by default — the `launcher` keys of
[../input/config-file.md](../input/config-file.md), which may hold a lone
tap of the logo key — and from the bar's logo button; in the overview its
keys work too. Its own keys close it again. What counts as a lone tap — and
that the key never fires one while acting as a modifier, including nested
inside Hyprland — is the logo-key rule owned by
[../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md).

The list holds the installed apps — every application entry that
survives the desktop-entry rules
([desktop-entries.md](desktop-entries.md)) — each row an icon and a
name. Panel width, row height, rows shown, icon size and fonts are
compile-time constants in `src/core/config.h`.

Typing filters the list; matches are ranked:

| Rank | Match |
|---|---|
| 1 | prefix of the entry name |
| 2 | prefix of any word of the name |
| 3 | substring of the name |
| 4 | generic name or keywords |
| 5 | command |

Keys and pointer:

| Input | Effect |
|---|---|
| Up / Down | move the selection |
| Enter | launch the selected app |
| Esc | close |
| Wheel | scroll the list |
| Click a row | launch that app |
| Click outside | close |

Launching runs the entry's Exec — quoting rules and Terminal=true are
owned by [desktop-entries.md](desktop-entries.md). A launched window
is placed like any new window
([../windows/window-management.md](../windows/window-management.md)):
centred on the screen being looked at; launched from the overview, on
the target the overview had at the launch, even when the overview
closed before the window appeared.

## Design considerations

- **Super+Space by default, not the tap**: the chord the user's other
  desktop opens apps with. The tap stays a key the user can give the
  launcher, and the tap rule keeps it apart from Super as a modifier
  ([../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)).
- **The ranking reads like the user types:** a name prefix is what
  almost every search is, so it outranks everything; the command is
  the last thing anyone remembers, so it ranks last.
- **Opening is always fresh** because the entries are re-read on
  every open ([desktop-entries.md](desktop-entries.md)) — installing
  or hiding an app needs no cache to be cleared.

## Cross-links

- [desktop-entries.md](desktop-entries.md) — the entry format,
  directories, precedence, icon lookup.
- [../windows/window-management.md](../windows/window-management.md)
  — placement of launched windows.
- [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)
  — Super+Space in the bindings table, the logo-key rules, nested
  mode.
