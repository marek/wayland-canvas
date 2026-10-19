# Protocols and limitations

What wayland-canvas speaks to clients and tools, and what it does not do.

## Protocols

xdg-shell, xdg-decoration and KDE server-decoration (server-side by default;
clients that negotiate neither keep their own decorations), layer-shell (bars,
launchers, wallpapers, notifications), foreign-toplevel management (taskbars can
restore minimized windows), xdg-activation, pointer-gestures, relative-pointer,
cursor-shape, screencopy/export-dmabuf (grim, and screen sharing through
xdg-desktop-portal-wlr), ext-session-lock (the screen lock; swaylock and
hyprlock work too), wlr-gamma-control (wlsunset, gammastep), data-control,
primary selection,
viewporter, fractional-scale, presentation-time, single-pixel-buffer, idle-notify,
idle-inhibit, virtual-keyboard.

## Limitations

X11 windows are drawn at scale 1 on a hi-dpi output (right-sized, a little
soft); no output-configuration protocol (set the scale with `-S`), the night
light needs an output with a gamma ramp of its own (a nested or headless run
shows none), full repaint of an output per changed frame (no damage regions),
no window open/close animations.

## See also

[The session](session.md) for what runs beside the protocols, and
[Install, build and run](install.md) for the `-S` option.
