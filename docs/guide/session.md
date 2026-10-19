# The session

What a login brings up beside the compositor itself: the lock screen,
notifications, the password dialog, the night light, X11 apps and the session
services.

A login — picked at the display manager, or started from a TTY — brings up
what a graphical session needs:

| Part | What you get |
|---|---|
| Lock screen | Super+L, `loginctl lock-session`, suspend (the lock is drawn before the machine sleeps) and an idle timeout of 5 minutes, which the Session tab changes. The password is your own, checked through PAM against the service file the package installs. While locked, only the lock is on screen — in a screenshot and a shared screen too — and an idle inhibitor (a video player) holds the idle lock off |
| Notifications | Popups top-right with their actions, a bell in the bar with the history behind it, and a silence switch. Super+N dismisses the newest, Super+Shift+N all of them. Another notification daemon (mako, dunst) can serve the session instead by taking the bus name first |
| Password dialog | An app that asks for administrator rights (`pkexec`, a package manager) gets a dialog in the session; it holds the keyboard while it is open. A session that already has a polkit agent keeps it |
| Night light | A switch and a colour temperature, in the brightness panel and the Session tab, toggled with Super+Ctrl+N. It rides the output's gamma ramp, so a screenshot stays in true colours; wlsunset and gammastep work through the gamma-control protocol and take over while they run |
| X11 applications | Applications that speak only X11 run through Xwayland, which starts with the first of them. Their windows are windows like any other: title bar, desktop, overview, maximize, minimize, fullscreen, and a close button that asks them to close. Their menus and tooltips appear where the app puts them, and the clipboard works both ways with wayland apps |
| Session services | `wayland-canvas-session.target` is started once the compositor has handed its display to systemd, so `graphical-session.target` units and the XDG autostart entries come up with the session and stop with it |

## Nested and headless runs

A nested or headless run leaves all of this to the session that owns it: no
session services are started, no logind session is watched.

## See also

[Screenshots and screen sharing](capture.md) for how the portal behaves in a
nested run, and [Protocols and limitations](compatibility.md) for what the
session leans on.
