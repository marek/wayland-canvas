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
	uint32_t accent, accent_text; // overview target; text on an accent fill
	uint32_t combine, abandon;
	uint32_t dim; // over wallpapers while zoomed out
	uint32_t panel, panel_text; // overview labels, hints, shelf
	uint32_t picker_cross; // the picker's crosshair lines
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
	.accent_text = 0x14161bff, // near-black on the mid-blue (6:1; white manages 2.8)
	.combine = 0x3ddc84ff,
	.abandon = 0xff6b5bff,
	.dim = 0x000000a0,
	.panel = 0x101216e0,
	.panel_text = 0xe8eaedff,
	.picker_cross = 0xffffffcc,
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
	.accent_text = 0xffffffff, // white on the dark blue, as the light launcher rows
	.combine = 0x1a9e68ff,
	.abandon = 0xe5484dff,
	.dim = 0x000000a0, // dims wallpapers in the overview in both modes
	.panel = 0xf4f5f8f0,
	.panel_text = 0x1f2430ff,
	.picker_cross = 0x1f2430cc,
	.launcher = 0xf4f5f8f5,
	.launcher_border = 0xd1d4dcff,
	.launcher_search = 0xe6e8ecff,
	.launcher_selected = 0x3760bfff,
	.launcher_hover = 0x00000014,
	.launcher_text = 0x1f2430ff,
	.launcher_dim_text = 0x767a86ff,
	.launcher_backdrop = 0x00000060,
};

// Canvas items (sticky notes and whatever else plugins put there): a card
// of the item's colour, a darker strip along the top, its text in dark ink
// wrapped to the width and clipped to the card.
inline constexpr double item_strip_darken = 0.85; // the strip's ink relative to the card's
inline constexpr int item_strip_height = 26; // canvas px
inline constexpr int item_text_padding = 10; // canvas px around the text
inline constexpr const char *item_font = "Sans 10";
inline constexpr uint32_t item_text_color = 0x1f2430ff; // dark ink on any card colour

// Panning.
inline constexpr double scroll_pan_speed = 1.5; // 2-finger scroll over empty canvas

// Hops: a 3-finger swipe, in the normal view and the overview alike.
// Touchpad units are libinput's (roughly pointer pixels).
inline constexpr double hop_threshold = 36.0; // 3-finger travel that triggers a jump to the next desktop
inline constexpr double hop_cone_deg = 60.0; // how far off the swipe direction a desktop may lie

// Per-desktop scale (Super + 2-finger pinch): a desktop's frame is its
// base size divided by its scale, and its windows are shown at it.
inline constexpr double desktop_scale_min = 0.25;
inline constexpr double desktop_scale_max = 2.0;
// Released pinches settle on this step of scale; 100 % is one of the steps,
// so a near-100 pinch catches it exactly.
inline constexpr double desktop_scale_step = 0.05;

// Overview (zoomed-out view).
inline constexpr double overview_swipe_distance = 260.0; // 4-finger travel for a full zoom transition
inline constexpr double overview_commit = 0.25; // progress past which a released zoom gesture completes
inline constexpr double overview_max_zoom = 0.30; // relative to a view at 100 %; further out below it
inline constexpr double overview_min_zoom = 0.02;
inline constexpr int overview_margin = 80; // logical px kept around the content
inline constexpr double abandon_distance = 200.0; // 4-finger sideways travel that abandons a desktop
inline constexpr double pinch_tile_scale = 1.30; // 3-finger spread past this tiles
inline constexpr double pinch_combine_scale = 0.72; // 3-finger pinch below this combines
inline constexpr double combine_overlap = 0.5; // dropping a frame this far onto another combines them

inline constexpr double anim_ms = 280.0;
inline constexpr int tile_gap = 8;
inline constexpr int desktop_gap = 8; // canvas px between desktops made beside desktop 1
inline constexpr double drag_threshold = 6.0; // px before a press becomes a drag
inline constexpr uint32_t double_click_ms = 400;
// Two lone Super taps this close are the double tap; a single tap with an
// action of its own waits this long while an action holds the double tap.
inline constexpr uint32_t double_tap_ms = 300;

// Input devices.
inline constexpr int repeat_rate = 25;
inline constexpr int repeat_delay = 600;
inline constexpr bool tap_to_click = true; // one finger left, two right, three middle
// Pressing the pad: two fingers are a right click and three a middle click,
// wherever they press (off: the pad's bottom-right corner is the right button).
inline constexpr bool clickfinger = true;
inline constexpr bool natural_scroll = true;
inline constexpr bool disable_while_typing = true;
inline constexpr int cursor_size = 24;

// Commands run through /bin/sh -c.
inline constexpr const char *terminal_cmd = "exec ${TERMINAL:-foot}";
// A window opens in the overview target it was launched for while its
// launch is this recent: a cold start of a large app fits, and it matches
// how long wlroots keeps an xdg-activation token.
inline constexpr double launch_trace_ms = 30000.0;

// X11 windows through Xwayland (docs/intents/windows/x11.md). Zoomed out,
// X's pointer is moved back to the middle of X's screen before it comes
// this close to an edge, in X pixels.
inline constexpr int x11_pointer_margin = 128;

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

// Shots, recordings and the capture UI. The recorder is an optional
// dependency: without it on PATH, recordings fail with a hint and shots
// keep working.
inline constexpr int capture_thumbnail_max_w = 240; // logical px
inline constexpr int capture_thumbnail_max_h = 140;
inline constexpr int capture_thumbnail_margin = 12; // from the output's corner
inline constexpr int capture_thumbnail_ms = 5000; // shown before it fades
inline constexpr int capture_toolbar_button_w = 84; // logical px
inline constexpr int capture_toolbar_button_h = 56;
inline constexpr int capture_toolbar_gap = 8; // between buttons and rows
inline constexpr int capture_toolbar_group_gap = 20; // shots | recordings
inline constexpr int capture_toolbar_pad = 12; // panel padding
inline constexpr int capture_toolbar_options_h = 36; // the options row
inline constexpr int capture_toolbar_footer_h = 24; // the "Esc closes" line
inline constexpr int capture_toolbar_margin = 24; // from the output's bottom
inline constexpr double capture_pick_outline = 2.0; // the selection's outline
inline constexpr int capture_pick_label_gap = 8; // the size label from it
inline constexpr int capture_countdown_ms = 1000; // one tick of the countdown
inline constexpr int capture_timer_steps[] = {0, 5, 10}; // none, 5 s, 10 s
inline constexpr const char *capture_recorder = "ffmpeg"; // encodes the frames
inline constexpr int capture_recorder_fps = 60; // frames piped per second
inline constexpr int capture_recorder_poll_ms = 200; // the reaper's poll
inline constexpr int capture_recorder_stop_ms = 2000; // teardown wait

// A plugin's surface on the plane names itself with this prefix, so the
// compositor knows it belongs on the canvas before it is told where
// (docs/intents/platform/ipc.md).
inline constexpr const char *canvas_surface_prefix = "wayland-canvas-canvas-";

// The lock screen (ext-session-lock-v1). The fill is what an output shows
// while no locker surface covers it: locked with nothing drawn yet, or a
// locker that died.
inline constexpr uint32_t lock_fill = 0x000000ff;
// The shell action a lock trigger runs: the locker plugin draws the lock
// and unlocks through the protocol.
inline constexpr const char *lock_action = "canvas.lock/lock";
// The shell actions the notification shortcuts run.
inline constexpr const char *notification_dismiss_action =
	"canvas.notifications/dismiss";
inline constexpr const char *notification_dismiss_all_action =
	"canvas.notifications/dismiss-all";
// How long a sleep is held back for the lock to be drawn; past it the
// machine sleeps anyway, locked or not.
inline constexpr int lock_sleep_wait_ms = 5000;
} // namespace config
