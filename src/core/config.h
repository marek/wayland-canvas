#pragma once

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

// Colours, 0xRRGGBBAA.
inline constexpr uint32_t color_canvas = 0x14161bff;
inline constexpr uint32_t color_grid = 0xffffff0c;
inline constexpr uint32_t color_title_active = 0x202020ff;
inline constexpr uint32_t color_title_inactive = 0x2b2b2bff;
inline constexpr uint32_t color_text_active = 0xffffffff;
inline constexpr uint32_t color_text_inactive = 0x8c8c8cff;
inline constexpr uint32_t color_button_hover = 0xffffff1a;
inline constexpr uint32_t color_button_press = 0xffffff0f;
inline constexpr uint32_t color_close_hover = 0xc42b1cff;
inline constexpr uint32_t color_close_press = 0xa52618ff;
inline constexpr uint32_t color_border_active = 0x0078d4ff;
inline constexpr uint32_t color_border_inactive = 0x3c3c3cff;
inline constexpr uint32_t color_frame = 0xc8ccd4d0; // virtual desktop dashed outline
inline constexpr uint32_t color_accent = 0x4c9bffff; // overview target
inline constexpr uint32_t color_combine = 0x3ddc84ff;
inline constexpr uint32_t color_abandon = 0xff6b5bff;
inline constexpr uint32_t color_dim = 0x000000a0; // over wallpapers while zoomed out
inline constexpr uint32_t color_panel = 0x101216e0; // overview labels, hints, shelf
inline constexpr uint32_t color_panel_text = 0xe8eaedff;

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
inline constexpr uint32_t color_launcher = 0x1f1f1ff5;
inline constexpr uint32_t color_launcher_search = 0x2b2b2bff;
inline constexpr uint32_t color_launcher_selected = 0x0078d4ff;
inline constexpr uint32_t color_launcher_hover = 0xffffff14;
inline constexpr uint32_t color_launcher_text = 0xffffffff;
inline constexpr uint32_t color_launcher_dim_text = 0x9a9a9aff;
inline constexpr uint32_t color_launcher_backdrop = 0x00000060;

// Top bar: a Quickshell client autostarted by Server::start() when qs is on
// PATH. Its config is found beside the compositor binary at one of the two
// relative paths below (build tree, install prefix), or at
// $WAYLAND_CANVAS_SHELL_DIR when set. -B starts the compositor without it.
inline constexpr const char *bar_program = "qs";
inline constexpr const char *bar_shell_build_dir = "../shell";
inline constexpr const char *bar_shell_install_dir = "../share/wayland-canvas/shell";

// Control socket for shell clients and `wayland-canvas msg`, created in
// $XDG_RUNTIME_DIR as <prefix>-<wayland display>.sock and exported to
// spawned clients as $WAYLAND_CANVAS_SOCKET.
inline constexpr const char *ipc_socket_prefix = "wayland-canvas";
} // namespace config
