# The login session's services

> Owns what a graphical login starts besides the compositor — the services
> and autostarted applications of a desktop session — and when they stop.
> The environment the login hands over first is
> [../platform/protocols.md](../platform/protocols.md)'s; what the package
> installs is [packaging.md](packaging.md)'s.

## Requirement

Logging into wayland-canvas starts what a graphical session starts: the
units bound to `graphical-session.target` (input methods, keyrings,
agents, the sleep lock) and the XDG autostart entries. They come up once
the compositor has handed its display and desktop name to the user's
systemd manager, and they stop when the session ends.

## Design considerations

- **The compositor starts its own session target**, rather than being
  wrapped in a session manager (uwsm). A login is the same command from a
  display manager, a TTY or a `.desktop` file, and behaves the same on Arch
  and Debian.
- **The order matters.** The services inherit the session's environment, so
  the target is started after the hand-over, never before it: a portal or
  an input method started first would serve no display.
- **Only a login owns them.** A nested or headless run shares the user's
  bus with the session that already owns these services, and a run on a bus
  of its own has no user manager to start them in: both leave them alone.

## Behavior

| Subject | Rule |
|---|---|
| The unit | `wayland-canvas-session.target`, installed into the systemd user unit directory; it binds to `graphical-session.target` and wants `xdg-desktop-autostart.target`, so both come up with it |
| Start | a login starts the target once it has handed `WAYLAND_DISPLAY` and `XDG_CURRENT_DESKTOP` to the user manager |
| Stop | the session ending stops the target, and `graphical-session.target` with it |
| After a crash | nothing is stopped — a crashed compositor runs no code. The next login stops the target before starting it, so a crashed session's services never outlive it |
| A unit that needs Hyprland | one bound to another compositor's own target is not started by this one; a unit bound to `graphical-session.target` is |
| Another graphical session of the same user | the units are the user manager's, one set per user: a second graphical session on another VT takes them over as it starts, and the session it took them from keeps running without them |
| Nested, headless, a bus of its own | no target is started and none is stopped |

## Error states, corner cases

- **No systemd user manager** (a container, another init): the start
  command fails and is logged; the compositor runs on.
- **The target already running** when a login starts: it is stopped first,
  so its services are the new session's.

## Cross-links

- [packaging.md](packaging.md) — the unit the package installs.
- [../platform/protocols.md](../platform/protocols.md) — the hand-over
  that must come first, and the hand-back on exit.
- [build-and-run.md](build-and-run.md).
