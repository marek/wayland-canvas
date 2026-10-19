# Opening apps

The launcher and the standard XDG desktop entries it lists: how to open an
app, add your own, and hide one you never use.

## The launcher

Tap the Windows (logo) key, or press Super+Space: a Start-style launcher lists
every installed app (XDG `.desktop` entries from `~/.local/share/applications`
and `/usr/share/applications`, with icons from your icon theme). Type to
filter, ↑/↓ or the mouse to pick, Enter or click to launch, Esc or a click
outside to close. New windows open in the middle of the screen you're looking
at (or of the overview's target). An app launched from the overview opens in
the overview's target even if you leave the overview before its window
appears; the window then waits there without taking the keyboard, and a hint
says where.

## Adding an app

To add your own entry, use the standard XDG desktop-entry file that every
Linux launcher reads: save e.g. `~/.local/share/applications/my-tool.desktop`

```ini
[Desktop Entry]
Type=Application
Name=My Tool
GenericName=What it does
Exec=/path/to/command --some-flag
Icon=utilities-terminal
Terminal=false
Keywords=tool;helper;
```

It shows up the next time the launcher opens (entries are re-read on every
open). `Icon` is a theme icon name or an absolute image path; `Terminal=true`
runs the command inside `$TERMINAL`. `Exec` only allows double quotes, so put
anything shell-heavy in a script, and check files with `desktop-file-validate`.

## Hiding an app

To hide an app, put a copy of its `.desktop` file in
`~/.local/share/applications` with `NoDisplay=true`; yours wins over the
system one.

## See also

[Gestures, mouse and keyboard](controls.md) for the launcher key, and
[Install, build and run](install.md) for the `$TERMINAL` fallback.
