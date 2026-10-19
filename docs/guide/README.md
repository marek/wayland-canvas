# wayland-canvas guide

An infinite-canvas Wayland compositor (wlroots 0.20): windows — with
Windows-11-style title bars — live anywhere on an unbounded plane, and
**virtual desktops are screen-sized dashed frames** you drag over the windows
however you like; you can work on a desktop or anywhere in between.
This guide is for people who use the compositor; working on its source is the
last page.

| Page | What it covers |
|---|---|
| [Install, build and run](install.md) | Packages, the default session, building from source, running, the control socket |
| [Concepts](concepts.md) | Canvas, desktop, free area, overview |
| [Gestures, mouse and keyboard](controls.md) | The touchpad gesture table, mouse actions, keyboard shortcuts |
| [Configuration](configuration.md) | `canvas.conf`, the settings window, the shell's `shell.json`, compile-time settings |
| [Opening apps](apps.md) | The launcher, adding an app, hiding an app |
| [The shell and its plugins](shell.md) | Bar, overview toolbar, sticky notes, light and dark, settings, plugins |
| [Screenshots, recordings and screen sharing](capture.md) | The capture keys and files, recordings, the portal |
| [The session](session.md) | Lock screen, notifications, night light, X11 apps, session services |
| [Protocols and limitations](compatibility.md) | What the compositor speaks, and what it does not do |
| [Working on wayland-canvas](development.md) | Repo layout, building, testing, releases |
