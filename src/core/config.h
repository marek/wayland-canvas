#pragma once

#include <cstddef>
#include <cstdint>

// Compile-time configuration (dwl-style): edit and rebuild. Keybindings live in input.cpp
// next to the actions they call.
namespace config {

// Server-side window decorations, logical pixels (Windows 11 metrics).
inline constexpr int titlebar_height = 32;
inline constexpr int button_width = 46;
inline constexpr int border_width = 1;
inline constexpr int resize_margin = 8; // invisible resize grab area outside the frame
inline constexpr const char *title_font = "Sans 10";
inline constexpr int title_padding = 12;
inline constexpr const char *ui_font = "Sans 10";
inline constexpr const char *ui_font_bold = "Sans Bold 10";

// The compositor's palette, 0xRRGGBBAA: one entry per thing it draws. The
// active one is Theme::colors() (core/theme.h); the compositor starts dark.
struct Palette {
	uint32_t canvas;
	uint32_t grid;
	uint32_t title_active, title_inactive;
	uint32_t text_active, text_inactive;
	uint32_t button_hover, button_press;
	uint32_t close_hover, close_press, close_text;
	uint32_t border_active, border_inactive;
	uint32_t frame, frame_hover; // desktop outlines; hover: the one under the pointer
	uint32_t accent; // overview target
	uint32_t combine, abandon;
	uint32_t dim; // over wallpapers while zoomed out
	uint32_t panel, panel_text; // overview labels, hints, shelf
	uint32_t launcher, launcher_border, launcher_search, launcher_selected,
		launcher_hover, launcher_text, launcher_dim_text, launcher_backdrop;
};

inline constexpr Palette palette_dark{
	.canvas = 0x14161bff,
	.grid = 0xffffff0c,
	.title_active = 0x202020ff,
	.title_inactive = 0x2b2b2bff,
	.text_active = 0xffffffff,
	.text_inactive = 0x8c8c8cff,
	.button_hover = 0xffffff1a,
	.button_press = 0xffffff0f,
	.close_hover = 0xc42b1cff,
	.close_press = 0xa52618ff,
	.close_text = 0xffffffff,
	.border_active = 0x0078d4ff,
	.border_inactive = 0x3c3c3cff,
	.frame = 0xc8ccd4d0, // virtual desktop dashed outline
	.frame_hover = 0xffffffff,
	.accent = 0x4c9bffff,
	.combine = 0x3ddc84ff,
	.abandon = 0xff6b5bff,
	.dim = 0x000000a0,
	.panel = 0x101216e0,
	.panel_text = 0xe8eaedff,
	.launcher = 0x1f1f1ff5,
	.launcher_border = 0x3a3a3aff,
	.launcher_search = 0x2b2b2bff,
	.launcher_selected = 0x0078d4ff,
	.launcher_hover = 0xffffff14,
	.launcher_text = 0xffffffff,
	.launcher_dim_text = 0x9a9a9aff,
	.launcher_backdrop = 0x00000060,
};

// The light palette, anchored on the bar's ([shell/theme.md]): background
// #e1e2e7, text #3760bf, attention #f52a65.
inline constexpr Palette palette_light{
	.canvas = 0xe1e2e7ff,
	.grid = 0x00000012,
	.title_active = 0xf2f3f7ff,
	.title_inactive = 0xe4e5eaff,
	.text_active = 0x1f2430ff,
	.text_inactive = 0x767a86ff,
	.button_hover = 0x0000001a,
	.button_press = 0x0000000f,
	.close_hover = 0xf52a65ff,
	.close_press = 0xd52251ff,
	.close_text = 0xffffffff,
	.border_active = 0x3760bfff,
	.border_inactive = 0xb0b3bcff,
	.frame = 0x5a6072d0,
	.frame_hover = 0x1f2430ff,
	.accent = 0x3760bfff,
	.combine = 0x1a9e68ff,
	.abandon = 0xe5484dff,
	.dim = 0x000000a0, // dims wallpapers in the overview in both modes
	.panel = 0xf4f5f8f0,
	.panel_text = 0x1f2430ff,
	.launcher = 0xf4f5f8f5,
	.launcher_border = 0xd1d4dcff,
	.launcher_search = 0xe6e8ecff,
	.launcher_selected = 0x3760bfff,
	.launcher_hover = 0x00000014,
	.launcher_text = 0x1f2430ff,
	.launcher_dim_text = 0x767a86ff,
	.launcher_backdrop = 0x00000060,
};

// Panning. Touchpad units are libinput's (roughly pointer pixels).
inline constexpr double pan_speed = 1.0; // canvas px per unit at zoom 1
inline constexpr bool natural_pan = true; // the canvas follows the fingers like a map
inline constexpr double inertia_tau_ms = 325.0; // momentum decay time constant
inline constexpr double inertia_min_speed = 0.25; // px/ms to keep coasting after release
inline constexpr double snap_radius = 0.10; // landing this close (fraction of screen) to a desktop snaps onto it
inline constexpr double scroll_pan_speed = 1.5; // 2-finger scroll over empty canvas

// Per-desktop scale (Super + 2-finger pinch): a desktop's frame is its
// base size divided by its scale, and its windows are shown at it.
inline constexpr double desktop_scale_min = 0.5;
inline constexpr double desktop_scale_max = 2.0;
// Released pinches settle on this step of scale; 100 % is one of the steps,
// so a near-100 pinch catches it exactly.
inline constexpr double desktop_scale_step = 0.05;

// Overview (zoomed-out view).
inline constexpr double overview_swipe_distance = 260.0; // 4-finger travel for a full zoom transition
inline constexpr double overview_commit = 0.25; // progress past which a released zoom gesture completes
inline constexpr double overview_max_zoom = 0.30;
inline constexpr double overview_min_zoom = 0.02;
inline constexpr int overview_margin = 80; // logical px kept around the content
inline constexpr double hop_threshold = 36.0; // 3-finger travel that triggers a jump to the next desktop
inline constexpr double hop_cone_deg = 60.0; // how far off the swipe direction a desktop may lie
inline constexpr double abandon_distance = 200.0; // 4-finger sideways travel that abandons a desktop
inline constexpr double pinch_tile_scale = 1.30; // 3-finger spread past this tiles
inline constexpr double pinch_combine_scale = 0.72; // 3-finger pinch below this combines
inline constexpr double combine_overlap = 0.5; // dropping a frame this far onto another combines them

inline constexpr double anim_ms = 280.0;
inline constexpr int tile_gap = 8;
inline constexpr double drag_threshold = 6.0; // px before a press becomes a drag
inline constexpr uint32_t double_click_ms = 400;

// Input devices.
inline constexpr int repeat_rate = 25;
inline constexpr int repeat_delay = 600;
inline constexpr bool tap_to_click = true;
inline constexpr bool natural_scroll = true;
inline constexpr bool disable_while_typing = true;
inline constexpr int cursor_size = 24;

// Commands run through /bin/sh -c.
inline constexpr const char *terminal_cmd = "exec ${TERMINAL:-foot}";

// App launcher: tap the logo key (or Super+Space).
inline constexpr int launcher_width = 600;
inline constexpr int launcher_search_height = 56;
inline constexpr int launcher_row_height = 44;
inline constexpr int launcher_max_rows = 10;
inline constexpr int launcher_icon = 28;
inline constexpr const char *launcher_font = "Sans 11";
inline constexpr const char *launcher_search_font = "Sans 13";

// Top bar: a Quickshell client autostarted by Server::start() when qs is on
// PATH. Its config is found beside the compositor binary at one of the two
// relative paths below (build tree, install prefix), or at
// $WAYLAND_CANVAS_SHELL_DIR when set. -B starts the compositor without it.
inline constexpr const char *bar_program = "qs";
inline constexpr const char *bar_shell_build_dir = "../shell";
inline constexpr const char *bar_shell_install_dir = "../share/wayland-canvas/shell";

// Control socket for shell clients and `wayland-canvas msg`, created in
// $XDG_RUNTIME_DIR as <prefix>-<wayland display>.sock and exported to
// spawned clients as $WAYLAND_CANVAS_SOCKET. Lines over `ipc_max_line`
// bytes and subscribers with over `ipc_max_queued` bytes of unread events
// are answered then dropped, so a client can never stall the compositor.
inline constexpr const char *ipc_socket_prefix = "wayland-canvas";
inline constexpr std::size_t ipc_max_line = 64 * 1024;
inline constexpr std::size_t ipc_max_queued = 1024 * 1024;
} // namespace config
