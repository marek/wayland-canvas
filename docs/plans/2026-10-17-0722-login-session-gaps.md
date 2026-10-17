# Login session gaps

Date: 2026-10-17 07:22 EDT

## Context

wayland-canvas is the session SDDM logs into, and it stands in for Omarchy's Hyprland session day to day: the screen
locks on a key, after idle and before sleep; notifications show; an app that asks for admin rights gets a password
dialog; X11-only apps run; a night light warms the screen; the services and apps that start with a graphical login
start with this one and stop with it. Installing through the repo's install script is safe: it never builds as root
and never changes who owns the checkout.

Requirements:

- While locked, only the lock surfaces are drawn (an output without one yet shows an opaque fill), input reaches
  only the locker, canvas shortcuts and gestures do nothing except VT switching, and nothing behind the lock appears
  in a screenshot, a recording or a shared screen. A locker that dies leaves the session locked; a new locker can
  take it over.
- The night light changes what the display shows, never a capture, wherever the output has a gamma table; an output
  without one (nested, headless) gets the filter drawn into its frames.
- A notification popup never takes the keyboard; the password dialog holds it while open.
- Another graphical session of the same user (Hyprland on another VT) keeps what it holds: its notification server,
  its polkit agent and its `graphical-session.target`. A wayland-canvas session on a TTY neither takes nor stops
  them. Nested and headless runs start no session services.
- Tests never touch the machine's logind session, its polkit agent or the user's session bus: locking the real
  session would lock the running desktop. They use private buses and scratch PAM services.

Settled:

- The lock screen, the notification server and the password dialog are built-in shell plugins on Quickshell's own
  services (`WlSessionLock` with `PamContext`, `NotificationServer`, `PolkitAgent`). A run with `-B` has none of
  them; other lockers (swaylock, hyprlock) and notification daemons work through the same protocols.
- Unlocking checks the password through PAM, against a service file the package installs.
- Lock triggers: a `lock` action in the bindings table, `loginctl lock-session`, suspend and hibernate (the lock is
  drawn before the machine sleeps), and an idle timeout set in the settings window, 5 minutes by default; an idle
  inhibitor holds it off.
- The night light is built in: a switch and a colour temperature, kept in `canvas.conf`, set from the brightness
  panel, the settings window and a toggle action. The wlr gamma-control protocol is supported too, for wlsunset and
  gammastep; while a client holds an output's gamma, the built-in filter steps aside on that output.
- X11 windows are windows like any other: title bars, desktops, the overview, minimize, maximize, fullscreen,
  close. Xwayland starts with the first X11 client, not at login, and a machine without it simply has no X11. On a
  hi-dpi output X11 windows are drawn at scale 1 and enlarged: right-sized, a little soft.
- Session services come from the compositor's own systemd integration, not uwsm: a login starts a session target
  bound to `graphical-session.target` and XDG autostart once it has handed over its environment, and stops it when
  the session ends, a crash included. The same on Arch and Debian.
- Out of scope: screens turning off after idle, fingerprint unlock, a sunset schedule for the night light
  (wlsunset and gammastep keep theirs through the protocol), Hyprland's own CTM protocol for hyprsunset.

The contract goes into new intents for the lock screen, notifications, the password dialog, X11 windows, the night
light and the login session's services, and into rows of [platform/protocols.md](../intents/platform/protocols.md),
[shell/plugins.md](../intents/shell/plugins.md),
[input/keyboard-and-pointer.md](../intents/input/keyboard-and-pointer.md),
[shell/settings.md](../intents/shell/settings.md) and [workspace/packaging.md](../intents/workspace/packaging.md).
These decisions close three open boxes of
[2026-10-14-1550-infinite-canvas-compositor.md](2026-10-14-1550-infinite-canvas-compositor.md).

## Where things stand

- Omarchy's shell (`quickshell -p /usr/share/omarchy/shell`) is the machine's notification server, polkit agent and
  lock screen, and it runs inside Hyprland's own service (`wayland-wm@hyprland.desktop.service`). No other
  notification daemon, polkit agent or locker is installed, so a wayland-canvas login has none.
- The compositor has no ext-session-lock-v1 and no gamma control, and the wlroots subproject is built with
  `xwayland=disabled` (`meson.build`). wlroots 0.20 offers all three: `wlr_session_lock_manager_v1`,
  `wlr_gamma_control_v1` with `wlr_gamma_control_v1_get_color_transform()` (a render pass takes a colour transform
  in `wlr_buffer_pass_options`), and `wlr_xwayland`; `WLR_XWAYLAND` names the Xwayland binary.
- Omarchy's night light, hyprsunset, speaks Hyprland's `hyprland_ctm_control_manager_v1`; no gamma-control client
  (wlsunset, gammastep) is installed.
- Omarchy's session services hang off `graphical-session.target` (fcitx5, the 1Password and keyring autostart
  entries, voxtype, bt-agent, the sleep lock, but also xdg-desktop-portal-hyprland), which uwsm starts for the
  Hyprland login; a wayland-canvas login starts none of it. The compositor already hands `WAYLAND_DISPLAY` and
  `XDG_CURRENT_DESKTOP` to the systemd user environment in a login and gives them back on exit (protocols.md,
  Activation environment and Hand-back).
- `tools/install.sh` re-runs itself with `exec sudo "$0" "$@"` inside `require_root`, where `$@` is the function's
  own, empty: the root copy loses `--package` and `--uninstall` and builds the package again as root, and
  `tools/package.sh` as root runs `chown -R nobody` over the checkout before dropping to `nobody`. `--uninstall`
  without sudo reinstalls. `install.sh` also takes whichever of `dist/*.pkg.tar.zst` and `dist/*.deb` `find` lists
  first. Until fixed, install with `tools/package.sh --arch`, then `sudo pacman -U dist/<package>.pkg.tar.zst` and
  `sudo wayland-canvas-session set-default`.
- Test tools on this machine: `notify-send`, `pkexec`, `pkcheck`, `Xwayland`, `xprop`. No X11 app (xterm, xeyes)
  and no swaylock or hyprlock, so the X11 and gamma proofs need small clients built with the tests (xcb is
  installed).

## Install flow

- [x] `tools/install.sh` keeps its arguments when it re-runs through sudo, and the root copy installs the package the user's copy built instead of building again
- [x] `tools/package.sh` refuses to run as root outside the release container (`--here`) and never changes the checkout's owner
- [x] `tools/install.sh` takes the package of this distro's format when `dist/` holds both
- [x] Packaging tests: through a stub `sudo`, `--package` and `--uninstall` reach the root copy and nothing is built there; a root `package.sh` without `--here` exits non-zero and leaves the checkout as it was

## Lock screen

- [x] Intent: the lock screen: the protocol's policy, what locking hides and blocks, the triggers and the idle timeout, the locker plugin and its PAM service, a dead locker; the `lock` action in the bindings table, the timeout in the settings window
- [x] Compositor: ext-session-lock-v1 with the locked-state rules above, a dead locker leaving the session locked, a new locker taking over
- [x] Triggers: the `lock` action, `loginctl lock-session`, suspend and hibernate held until the lock is drawn, the idle timeout with inhibitors honoured — the action and the idle timeout proven in the lock scenario; the logind pair written against sd-bus
- [ ] Triggers proof: the logind pair (Lock, PrepareForSleep with the delay inhibitor) against a stand-in logind on a private system bus
- [x] Shell: the lock plugin on every output, the password through PAM; the package installs the PAM service file
- [x] Settings window: the idle timeout on a Session page (landing with the night light's switch, which shares the page)
- [x] Scenarios: locking hides the windows on screen and in a capture; keys, clicks and gestures reach no window; a killed locker leaves it locked; the idle timeout locks and an inhibitor holds it off
- [x] Scenario: a wrong password keeps it locked and the right one unlocks, through the lock plugin against a scratch PAM service

## Notifications

- [x] Intent: notifications: popups (place, stacking, expiry per urgency, actions, icons and images), dismiss and dismiss-all keys, the history and its bar widget, the silence switch, another server already holding the name
- [x] Shell: the notifications plugin: popups, the history panel, the silence switch; the keys in `canvas.conf` and the settings window
- [x] Scenarios on a private bus: `notify-send` shows a popup with its text; the keys dismiss one and all; the history lists them; silence holds popups back
- [ ] Scenario: an action click reaches the sender, a normal popup expires while a critical one stays, and with another server holding the name the shell runs without one

## Password dialog

- [x] Intent: the password dialog: when it opens, what it shows (the action's message, the identity, the password field), the keyboard, a wrong password, cancel, which output, a session that already has an agent
- [x] Shell: the polkit plugin, registered for the session it runs in; a refused registration leaves the shell running
- [x] Proof: the dialog's states driven through a seam in a scenario, never the machine's polkit agent; `pkexec true` on screen (the user)

## X11 apps

- [x] The shell seam: `View` answers the canvas the same whichever shell a window speaks, with the two halves in `view_xdg.cpp` and `view_x11.cpp` and no protocol left in the shared code
- [x] Intent: X11 windows: started on demand, `DISPLAY` handed over and back with `WAYLAND_DISPLAY`, windows as views (title bars, desktops, overview, states, close), override-redirect menus and tooltips, focus and activation, the clipboard both ways, scale 1 on hi-dpi, a machine without Xwayland
- [x] Build and package: wlroots with Xwayland; Arch `optdepends` xorg-xwayland, Debian `Recommends` xwayland
- [x] Compositor: X11 windows as views: map, title bars, move and resize, maximize, minimize and fullscreen requests, close through `WM_DELETE_WINDOW`, focus
- [x] Compositor: override-redirect windows placed beside their parent in canvas space and kept out of the window list, and the clipboard both ways between X11 and wayland clients
- [ ] Hi-dpi: an X11 window drawn at scale 1 and enlarged on a scaled output (the user: it needs a hi-dpi screen)
- [x] Scenario: an xcb client built with the tests maps a window with a title bar where the user is looking, takes a click and a key, is maximized from its title bar and closed by its × through `WM_DELETE_WINDOW`
- [x] Scenario: the popup beside its parent, the clipboard both ways, and `WLR_XWAYLAND` naming a missing binary leaving the compositor without X11

## Night light

- [x] Intent: the night light: the switch, the temperature range and default, where it is set, the gamma table and the in-frame fallback, the gamma-control protocol and how a client's table and the filter share an output, the IPC to read and set it
- [x] Compositor: the filter on every output that has a gamma ramp, kept in `canvas.conf`; wlr gamma-control; the IPC methods
- [x] Shell: the switch and the temperature in the brightness panel and the settings window; the toggle action in the bindings table
- [x] Scenario: the settings, the temperature's multipliers, the toggle key and the gamma-control client, headless — where no output has a ramp to warm
- [ ] (the user to decide) The in-frame fallback cannot be built as settled: the GLES2 renderer applies no colour transform to an output's pass (`features.output_color_transform` is false), and an overlay drawn at an alpha lightens the frame as much as it warms it, so a nested or headless run shows no night light. The ways out are the Vulkan renderer, which does apply one, or leaving it to the real backend only
- [ ] On screen, TTY: the night light warms the display and a grim shot of it stays in true colours (the user)

## Session services

- [x] Intent: the login session's services: what a login starts (`graphical-session.target`, XDG autostart) and when, how they stop (session end, crash), units that need Hyprland, another graphical session of the same user, TTY, nested and headless runs
- [x] Compositor and package: the session target unit shipped, started by a login after the environment hand-over, stopped when the session ends; a crash is cleaned up by the next login, which stops the target before starting it
- [x] Proof: the start, stop and crash rules as a unit test over the pure rules (`session-services`), and the lock scenario proving a headless run runs no systemctl at all through a stub first on its PATH

## Verification

- [x] README (installing, the lock, notifications, X11, the night light, the session services, the new keys) and `-h`; warning-free build; `tools/check.sh` green
- [ ] On screen as the SDDM login: lock by key, after idle and on closing the lid; a notification with an action; `pkexec true`; an X11 app; the night light on and a screenshot in true colours; fcitx5, 1Password and the other session services running (the user)
