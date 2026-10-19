# Protocols

> Owns which Wayland protocols are supported and the policy each is
> held to.

## Requirement

Clients integrate through standard protocols. Every protocol enabled
carries a policy — what it enables and what binds it; a protocol
without a policy is not added.

| Protocol | Enables / policy |
|---|---|
| xdg-shell | application windows ([../windows/window-management.md](../windows/window-management.md)) |
| xdg-dialog-v1 | a wayland dialog says it is modal: it floats over the window it blocks, even on a tiled desktop ([../windows/window-management.md](../windows/window-management.md)) |
| Xwayland | X11 applications: the window manager is the compositor, and an X11 window is a view ([../windows/x11.md](../windows/x11.md)); the X server starts with the first X11 client |
| xdg-decoration | negotiating server-side decorations; server-side is what the compositor offers ([../windows/title-bars.md](../windows/title-bars.md)) |
| server-decoration (KDE) | server-side decorations for clients that speak the KDE protocol instead |
| layer-shell | bars, launchers, wallpapers, notifications; the top layer hides under a fullscreen window; exclusive zones carve the usable area ([outputs.md](outputs.md)) |
| foreign-toplevel-management | taskbars and docks: each window's title, app id and state; activate, maximize, minimize, fullscreen and close requests are honoured ([../windows/window-management.md](../windows/window-management.md)) |
| xdg-activation | activation only with tokens that came from real input |
| pointer-gestures | the gestures the canvas does not claim are forwarded to clients ([../input/gestures.md](../input/gestures.md)) |
| relative-pointer | relative pointer motion for clients that ask for it |
| cursor-shape | standard cursor shapes |
| screencopy, export-dmabuf | screenshots and capture (grim), and screen sharing through the portal ([below](#screen-sharing)); a copy asks for a frame through `needs_frame` ([../render/rendering.md](../render/rendering.md), [../bugs/screencopy-never-returned.md](../bugs/screencopy-never-returned.md)) |
| ext-image-copy-capture-v1 | offscreen capture (grim, the portal's sharing): an output or one window ([../render/rendering.md](../render/rendering.md)); a window source re-renders the window alone at its told scale, and a frame request is always answered — with a fresh render, never damage-gated |
| ext-foreign-toplevel-list-v1 | one handle per mapped window, its title and app id kept current; the identifier names a window for capture (grim -T) |
| data-control, primary-selection | clipboard and primary selection |
| viewporter, fractional-scale | client-side scaling to the output scale ([../render/hidpi.md](../render/hidpi.md)) |
| presentation-time | per-frame presentation feedback ([../render/rendering.md](../render/rendering.md)) |
| single-pixel-buffer | solid-colour buffers |
| idle-notify, idle-inhibit | idle notification and inhibition; an inhibitor also stands down the idle lock ([../shell/lock.md](../shell/lock.md)) |
| wlr-gamma-control-v1 | a client's gamma table per output (wlsunset, gammastep); while it holds one, the built-in night light steps aside there ([../render/night-light.md](../render/night-light.md)) |
| ext-session-lock-v1 | locking the session: one locker at a time, the lock outliving its client ([../shell/lock.md](../shell/lock.md)) |
| virtual-keyboard | virtual keyboards; the scenarios type with wtype |

## Screen sharing

Apps share a screen — a Chrome presentation, a video call — through
xdg-desktop-portal's ScreenCast over PipeWire, and the portal's wlroots
backend (xdg-desktop-portal-wlr) captures through the ext-image-copy-capture
protocols: monitors from the outputs' committed frames, windows from the
compositor's own window sources ([above](#requirement)).

| Subject | Rule |
|---|---|
| Desktop name | every client sees `XDG_CURRENT_DESKTOP=wayland-canvas:wlroots`; the session file names the same two ([../workspace/packaging.md](../workspace/packaging.md)) |
| Backends | `wayland-canvas-portals.conf`, installed beside the portal's own configs, sends ScreenCast and Screenshot to the wlroots backend and the rest to the GTK one; where it is not installed (a run from the build tree) the portal finds the wlroots backend by the `wlroots` desktop name |
| Activation environment | the services the session bus starts (the portal and its backends) learn this compositor's `WAYLAND_DISPLAY` and desktop name at start, when the bus is this run's: a login session hands them over through systemd and restarts the portals an earlier session left running; a run on a bus of its own (`dbus-run-session`) hands them over on that bus; a run on the user's systemd bus that is not a login (nested, headless) leaves them to the host |
| Hand-back | on exit a login puts back the two values it replaced in the user's systemd environment (unsetting the ones that were unset) and restarts the portals, so they serve the session they were taken from — another compositor running on another VT shares its screens again. Unless another session has taken them since: then they are left to it. A crash hands nothing back |
| Sharing chooser | the xdpw config shipped as the file `sysconfdir/xdg/xdg-desktop-portal-wlr/wayland-canvas` — xdpw reads the file named after each element of `$XDG_CURRENT_DESKTOP` before its own `config` — runs the compositor's own picker as the chooser: `chooser_type = dmenu`, `chooser_cmd = wayland-canvas msg capture.pick`, reaching the compositor through the `$WAYLAND_DISPLAY` the session hands over ([ipc.md](ipc.md)); per-desktop, so another compositor's xdpw on the same bus never reads it. A run from the build tree needs the file in `~/.config/xdg-desktop-portal-wlr/` with the build's binary in `chooser_cmd` (README) |
| Proof | the sharing scenario drives a private `dbus-run-session` portal — scratch XDG dirs, its own PipeWire, never the user's bus or portal — with the shipped chooser config byte for byte under its installed name and a session environment like the bus's own services have (no `$WAYLAND_CANVAS_SOCKET`, `wayland-canvas` found on `PATH`); its ScreenCast client must receive a PipeWire stream for a monitor and for a window. A machine without the portal pieces skips the phase |

The portal is one service per bus: while another session owns the user's
bus — a host compositor of a nested run, or a login on another VT — it shares
that session's screens, and a nested run is shared as the host's window.
Under `dbus-run-session` a run has a portal of its own, as a test only: the
apps in it lose the session's keyring (saved passwords and logins) and every
other service of the user's bus.

## Cross-links

- [wlroots.md](wlroots.md), [outputs.md](outputs.md),
  [../render/rendering.md](../render/rendering.md),
  [../workspace/build-and-run.md](../workspace/build-and-run.md).
