#pragma once

// `wayland-canvas msg capture.pick` as xdg-desktop-portal-wlr's chooser:
// reads the monitor and window list from stdin, runs the compositor's own
// picker, and prints the chosen line back byte-exactly. Silence means the
// user cancelled; only a transport failure is an error exit.
int capture_pick_bridge();
