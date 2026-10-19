# Screenshots, recordings and screen sharing

macOS-style capture from the keyboard — where the files land and how
recordings are made — and sharing a screen to Chrome or a video call through
the portal.

## Screenshots and recordings

| Keys | Action |
|---|---|
| Super+Shift+3 | Screenshot of the whole screen |
| Super+Shift+4 (or Print) | Pick what to shot: drag a region, click a window, Space for windows only, Esc to cancel |
| Super+Shift+5 | The capture toolbar: shots and recordings of screen, window or region, with options (destination, timer, pointer) |
| Ctrl + any shot key | Copy to the clipboard instead of saving |
| Shift+Print | Screenshot of the whole screen |
| Alt+Print | Stop a running recording, else open the toolbar on its recordings |

A saved shot lands in `~/Pictures/Screenshots` (the XDG pictures dir), is
copied to the clipboard, and shows as a bottom-right thumbnail for a few
seconds — click it to open the file, swipe it away. A window is captured
alone: title bar and content, nothing over it, at the output's scale whatever
the canvas zoom. Recordings go to `~/Videos` (the XDG videos dir); a red dot
in the bar marks a running one and stops it on a click. The compositor itself
renders every recording frame offscreen (the same path a shot takes) and
pipes them to `ffmpeg`, so recordings work nested and from a TTY alike, and
none of the capture UI (picker, toolbar, thumbnail) is ever in them. Both
packages depend on ffmpeg for this; a build run from the checkout needs it
installed:

```sh
sudo pacman -S ffmpeg   # Debian: apt install ffmpeg
```

## Screen sharing

Apps share a screen through xdg-desktop-portal and PipeWire; install the
wlroots backend once:

```sh
sudo pacman -S xdg-desktop-portal-wlr   # Debian: apt install xdg-desktop-portal-wlr
```

wayland-canvas names itself `wayland-canvas:wlroots` (`XDG_CURRENT_DESKTOP`)
and hands its display to the portal at start. Sharing works from a
wayland-canvas login (a TTY, or the session at the login screen): run
`./build/wayland-canvas` as usual — the hand-over happens on your normal
session bus, so the keyring (saved passwords and logins), notifications and
the rest keep working. When it exits, the portal goes back to the session it
was taken from (Hyprland on another VT shares its own screens again).

Apps pick what to share with wayland-canvas's own picker (a screen, or a
window by clicking it). A package installs the portal backend's config for
that; a run from the build tree needs it once, with the build's binary as the
chooser:

```sh
mkdir -p ~/.config/xdg-desktop-portal-wlr
sed "s|= wayland-canvas msg|= $PWD/build/wayland-canvas msg|" \
    packaging/xdg-desktop-portal-wlr.conf > ~/.config/xdg-desktop-portal-wlr/wayland-canvas
```

The portal is one per login. While wayland-canvas runs nested inside another
compositor, apps share through *that* compositor's portal: pick the
wayland-canvas window in its picker. Don't start wayland-canvas under
`dbus-run-session` to get around this: it gets a portal of its own, but the
apps inside lose the keyring and every other session service.

## See also

[The session](session.md) for what a shared screen shows while locked, and
[Install, build and run](install.md) for running from a TTY or nested.
