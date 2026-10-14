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
| xdg-decoration | negotiating server-side decorations; server-side is what the compositor offers ([../windows/title-bars.md](../windows/title-bars.md)) |
| server-decoration (KDE) | server-side decorations for clients that speak the KDE protocol instead |
| layer-shell | bars, launchers, wallpapers, notifications; the top layer hides under a fullscreen window; exclusive zones carve the usable area ([outputs.md](outputs.md)) |
| foreign-toplevel-management | taskbars and docks: each window's title, app id and state; activate, maximize, minimize, fullscreen and close requests are honoured ([../windows/window-management.md](../windows/window-management.md)) |
| xdg-activation | activation only with tokens that came from real input |
| pointer-gestures | the gestures the canvas does not claim are forwarded to clients ([../input/gestures.md](../input/gestures.md)) |
| relative-pointer | relative pointer motion for clients that ask for it |
| cursor-shape | standard cursor shapes |
| screencopy, export-dmabuf | screenshots and capture (grim); a copy asks for a frame through `needs_frame` ([../render/rendering.md](../render/rendering.md), [../bugs/screencopy-never-returned.md](../bugs/screencopy-never-returned.md)) |
| data-control, primary-selection | clipboard and primary selection |
| viewporter, fractional-scale | client-side scaling to the output scale ([../render/hidpi.md](../render/hidpi.md)) |
| presentation-time | per-frame presentation feedback ([../render/rendering.md](../render/rendering.md)) |
| single-pixel-buffer | solid-colour buffers |
| idle-notify, idle-inhibit | idle notification and inhibition |
| virtual-keyboard | virtual keyboards; the scenarios type with wtype |

## Cross-links

- [wlroots.md](wlroots.md), [outputs.md](outputs.md),
  [../render/rendering.md](../render/rendering.md),
  [../workspace/build-and-run.md](../workspace/build-and-run.md).
