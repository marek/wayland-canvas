#ifndef CV_SERVER_H
#define CV_SERVER_H

#include <stdbool.h>
#include <cairo.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <xkbcommon/xkbcommon.h>
#include "desktop_entry.h"
#include "geometry.h"

struct cv_server;
struct cv_output;

/* ---- views (xdg toplevels) ---- */

enum cv_deco_part {
	CV_DECO_NONE = 0,
	CV_DECO_TITLE,
	CV_DECO_MINIMIZE,
	CV_DECO_MAXIMIZE,
	CV_DECO_CLOSE,
	CV_DECO_RESIZE,
};

/* Cached title bar texture plus the state it was drawn for. */
struct cv_deco {
	struct wlr_texture *texture;
	int width;
	float scale;
	bool active, maximized;
	enum cv_deco_part hover, pressed;
	char *title;
};

struct cv_view {
	struct wl_list link; /* cv_server.views, head is topmost */
	struct cv_server *server;
	struct wlr_xdg_toplevel *toplevel;
	struct wlr_xdg_toplevel_decoration_v1 *xdg_decoration;
	struct wlr_foreign_toplevel_handle_v1 *foreign;

	/* Canvas position of the window geometry's top-left corner. */
	int x, y;
	bool mapped, minimized, maximized, fullscreen;
	bool ssd; /* client accepted server-side decorations */
	struct wlr_box saved; /* content box to restore after maximize/fullscreen */
	uint32_t resize_edges; /* edges being dragged; the opposite ones stay put */
	/* Configure that ended the resize: anchoring continues until the client
	 * acks it, since its final resized commit arrives after the release. */
	uint32_t resize_end_serial;
	struct wlr_box resize_start;
	uint32_t output_mask; /* outputs the view is visible on */
	uint64_t minimize_seq;

	/* Title bar state: what the pointer hovers/presses and the cached texture. */
	enum cv_deco_part hover, pressed;
	struct cv_deco deco;

	struct wl_listener map, unmap, commit, destroy;
	struct wl_listener request_move, request_resize, request_maximize;
	struct wl_listener request_minimize, request_fullscreen, set_title, set_app_id;
	struct wl_listener deco_request_mode, deco_destroy;
	struct wl_listener foreign_activate, foreign_maximize, foreign_minimize;
	struct wl_listener foreign_fullscreen, foreign_close;
};

struct cv_popup {
	struct wlr_xdg_popup *popup;
	struct wl_listener commit, destroy;
};

/* ---- layer shell ---- */

struct cv_layer {
	struct wl_list link; /* cv_output.layers[layer] */
	struct cv_server *server;
	struct cv_output *output;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_box geo; /* output-local logical */
	bool mapped;
	struct wl_listener map, unmap, commit, destroy;
};

/* ---- virtual desktops ---- */

/* A screen-sized rectangle on the canvas. Windows "belong" to the desktop
 * their centre lies in; moving the frame never moves windows. */
struct cv_desktop {
	struct wl_list link; /* cv_server.desktops, head is topmost */
	struct cv_server *server;
	int id;
	struct wlr_box box; /* canvas coordinates */
	double swipe_offset; /* overview: sideways offset during the abandon gesture */
	struct wlr_texture *label;
	int label_id, label_w, label_h;
	float label_scale;
};

/* ---- outputs ---- */

enum cv_anim_kind {
	CV_ANIM_NONE,
	CV_ANIM_LERP, /* a -> b */
	CV_ANIM_ZOOM, /* camera_zoom_path(a = near, b = far, p0 -> p1) */
};

struct cv_anim {
	enum cv_anim_kind kind;
	struct camera a, b;
	double p0, p1;
	double start, duration; /* ms */
};

enum cv_mode {
	CV_MODE_NORMAL,
	CV_MODE_OVERVIEW,
};

struct cv_output {
	struct wl_list link;
	struct cv_server *server;
	struct wlr_output *wlr_output;
	uint32_t bit;
	int lx, ly, width, height; /* layout position and logical size */

	struct camera cam;
	struct cv_anim anim;
	double vel_x, vel_y; /* momentum, canvas units per ms */
	bool coasting;
	double last_tick;
	double frames_alpha; /* desktop outlines faded in while panning */

	enum cv_mode mode;
	struct camera home; /* camera before entering the overview */
	double overview_zoom;
	/* Overview target: where entering will take you. A desktop, or a free
	 * screen-sized spot on the canvas when target_desktop is NULL. */
	struct wlr_box target;
	struct cv_desktop *target_desktop;

	struct wl_list layers[4]; /* cv_layer.link, by zwlr_layer_shell_v1_layer */
	struct wlr_box usable; /* output-local area not covered by exclusive zones */

	bool dirty;
	struct wl_listener frame, needs_frame, request_state, destroy;
};

/* ---- input ---- */

enum cv_cursor_mode {
	CV_CURSOR_PASSTHROUGH,
	CV_CURSOR_MOVE, /* dragging a window */
	CV_CURSOR_RESIZE,
	CV_CURSOR_PAN, /* dragging empty canvas */
	CV_CURSOR_DECO_BUTTON, /* title bar button pressed, acts on release */
	CV_CURSOR_OVERVIEW_PRESS, /* overview press, not yet a drag */
	CV_CURSOR_DESKTOP_DRAG, /* overview: moving a desktop frame */
};

/* What an overview press landed on. */
enum cv_overview_hit {
	CV_HIT_EMPTY,
	CV_HIT_VIEW,
	CV_HIT_DESKTOP,
	CV_HIT_TAB_TILE,
	CV_HIT_TAB_ABANDON,
	CV_HIT_SHELF,
};

struct cv_keyboard {
	struct wl_list link;
	struct cv_server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers, key, destroy;
};

enum cv_gesture_kind {
	CV_GESTURE_NONE,
	CV_GESTURE_FORWARD, /* belongs to the client under the pointer */
	CV_GESTURE_PENDING, /* compositor-owned, direction not decided yet */
	CV_GESTURE_PAN, /* normal mode: move the canvas */
	CV_GESTURE_ZOOM_OUT, /* normal mode: 4 fingers up into the overview */
	CV_GESTURE_ZOOM_IN, /* overview: 4 fingers down into the target */
	CV_GESTURE_ZOOM_FURTHER, /* overview: 4 fingers up, zoom out more */
	CV_GESTURE_HOP, /* overview: 3 fingers, jump to a desktop */
	CV_GESTURE_ABANDON, /* overview: 4 fingers sideways */
	CV_GESTURE_PINCH, /* overview: 3-finger pinch, tile or combine */
	CV_GESTURE_OVERVIEW_ZOOM, /* overview: 2-finger pinch */
	CV_GESTURE_IGNORED,
};

struct cv_gesture {
	enum cv_gesture_kind kind;
	bool pinch; /* began as a pinch (matters for forwarding) */
	uint32_t fingers;
	struct cv_output *output;
	double ax, ay; /* accumulated travel */
	double scale; /* pinch scale relative to begin */
	double vx, vy; /* velocity estimate, units per ms */
	uint32_t last_time;
	double progress;
	double start_zoom;
	struct camera near; /* zoom gestures: close-up end of the path */
	struct camera far; /* zoom gestures: zoomed-out end */
	struct cv_desktop *desktop; /* acted on by abandon / pinch */
	struct cv_desktop *merge_from; /* pinch-in preview: absorbed desktop */
};

/* Transient banner drawn at the bottom of an overview output. */
struct cv_hint {
	struct wlr_texture *texture;
	char *text;
	int w, h;
	float scale;
	struct wl_event_source *timer; /* clears a transient hint */
};

struct cv_options {
	const char *startup_cmd;
	double scale; /* 0: pick per output */
};

struct cv_server {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend;
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;
	struct wlr_output_layout *output_layout;
	struct wlr_xdg_shell *xdg_shell;
	struct wlr_layer_shell_v1 *layer_shell;
	struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
	struct wlr_server_decoration_manager *kde_decoration_mgr;
	struct wlr_foreign_toplevel_manager_v1 *foreign_mgr;
	struct wlr_pointer_gestures_v1 *pointer_gestures;
	struct wlr_presentation *presentation;
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit;
	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor_mgr;
	struct cv_options options;

	struct wl_list outputs; /* cv_output.link */
	struct wl_list views; /* cv_view.link, stacking order, head is topmost */
	struct wl_list desktops; /* cv_desktop.link, head is topmost */
	struct wl_list keyboards;

	struct cv_view *focused_view;
	struct cv_layer *focused_layer; /* keyboard-interactive layer surface */
	uint64_t minimize_counter;
	bool visibility_dirty;

	/* Pointer grab state. */
	enum cv_cursor_mode cursor_mode;
	struct cv_view *grab_view;
	struct cv_desktop *grab_desktop;
	struct cv_output *grab_output;
	enum cv_overview_hit grab_hit;
	double grab_lx, grab_ly; /* layout coords where the grab started */
	double grab_cx, grab_cy; /* canvas coords where the grab started */
	struct wlr_box grab_box; /* view content box / desktop box at grab start */
	struct camera grab_cam;
	uint32_t grab_edges;
	enum cv_deco_part grab_part;
	bool grab_moved;
	struct cv_desktop *drop_desktop; /* combine target under a dragged frame */
	uint32_t last_title_click;
	struct cv_view *last_title_view;
	const char *cursor_image;

	struct cv_gesture gesture;
	struct cv_hint hint; /* transient message, see hint_set() */
	struct cv_launcher *launcher; /* owned by launcher.c, NULL until first opened */
	bool rebase_cursor; /* canvas moved under the pointer: refresh pointer focus next frame */

	struct wlr_drag_icon *drag_icon;
	double drag_icon_dx, drag_icon_dy;

	struct wl_listener new_output, layout_change;
	struct wl_listener new_xdg_toplevel, new_xdg_popup;
	struct wl_listener new_layer_surface;
	struct wl_listener new_xdg_decoration, new_kde_decoration;
	struct wl_listener new_surface;
	struct wl_listener new_input, new_virtual_keyboard;
	struct wl_listener cursor_motion, cursor_motion_absolute, cursor_button;
	struct wl_listener cursor_axis, cursor_frame;
	struct wl_listener swipe_begin, swipe_update, swipe_end;
	struct wl_listener pinch_begin, pinch_update, pinch_end;
	struct wl_listener hold_begin, hold_end;
	struct wl_listener request_cursor, request_cursor_shape;
	struct wl_listener request_set_selection, request_set_primary_selection;
	struct wl_listener request_start_drag, start_drag;
	struct wl_listener drag_icon_commit, drag_icon_destroy;
	struct wl_listener request_activate;
	struct wl_listener new_inhibitor;
};

/* server.c */
bool server_init(struct cv_server *server, const struct cv_options *options);
bool server_start(struct cv_server *server);
void server_finish(struct cv_server *server);
void server_spawn(const char *cmd);
double now_ms(void);

/* output.c */
void output_init(struct cv_server *server);
void output_finish(struct cv_server *server);
void output_damage(struct cv_output *output);
void server_damage_all(struct cv_server *server);
struct cv_output *output_at(struct cv_server *server, double lx, double ly);
struct cv_output *output_under_cursor(struct cv_server *server);
struct rect output_viewport(struct cv_output *output);
struct camera output_camera(struct cv_output *output);
void output_local_to_canvas(struct cv_output *output, double ox, double oy, double *cx, double *cy);
void output_canvas_to_local(struct cv_output *output, double cx, double cy, double *ox, double *oy);
void server_update_visibility(struct cv_server *server);
struct wlr_box output_usable_canvas_box(struct cv_output *output, struct wlr_box area);

/* canvas.c: camera motion, desktops, overview */
void camera_set(struct cv_output *output, struct camera cam);
void camera_animate(struct cv_output *output, struct camera to);
void camera_animate_zoom(struct cv_output *output, struct camera near, struct camera far,
	double p0, double p1);
void camera_stop(struct cv_output *output);
bool output_tick(struct cv_output *output, double now);
struct camera camera_for_box(struct wlr_box box);
bool output_is_panning(struct cv_output *output);
bool output_interactive(struct cv_output *output);

struct cv_desktop *desktop_create(struct cv_server *server, struct wlr_box box);
void desktop_destroy(struct cv_desktop *desktop);
struct cv_desktop *desktop_at(struct cv_server *server, double cx, double cy);
struct cv_desktop *desktop_of_view(struct cv_server *server, struct cv_view *view);
struct cv_desktop *desktop_by_id(struct cv_server *server, int id);
struct cv_desktop *desktop_showing(struct cv_output *output);
struct cv_desktop *desktop_best_overlap(struct cv_server *server, struct rect area);
struct cv_desktop *desktop_nearest(struct cv_server *server, struct cv_desktop *to);
void desktop_raise(struct cv_desktop *desktop);
void canvas_tile(struct cv_server *server, struct wlr_box area, struct cv_output *output);
void canvas_combine(struct cv_server *server, struct cv_desktop *into,
	struct cv_desktop *from, struct wlr_box from_box);
size_t views_in_box(struct cv_server *server, struct wlr_box box,
	struct cv_view **out, size_t max);

void canvas_goto_box(struct cv_output *output, struct wlr_box box);
void canvas_goto_direction(struct cv_output *output, double dx, double dy);
void canvas_pan(struct cv_output *output, double dx, double dy);
void canvas_reveal_view(struct cv_server *server, struct cv_view *view);
void canvas_pin_desktop(struct cv_output *output);

struct camera overview_camera(struct cv_output *output, struct camera home);
void overview_enter(struct cv_output *output);
/* Records home (current zoom-1 camera) and the initial target; returns the
 * overview camera to zoom out to. Does not change the mode. */
struct camera overview_prepare(struct cv_output *output);
/* Gesture release: switch to overview and animate path parameter t -> 1. */
void overview_finish_enter(struct cv_output *output, struct camera far, double t);
/* Gesture release: switch to normal and animate path parameter t -> 0,
 * landing on `near` (camera_for_box(target)). */
void overview_finish_exit(struct cv_output *output, struct camera near,
	struct camera far, double t);
void overview_exit(struct cv_output *output);
void overview_cancel(struct cv_output *output);
void overview_hop(struct cv_output *output, double dx, double dy);
void overview_set_target(struct cv_output *output, struct wlr_box box,
	struct cv_desktop *desktop, bool center);
void overview_target_point(struct cv_output *output, double cx, double cy);
void overview_zoom_by(struct cv_output *output, double factor, double ox, double oy);
void overview_abandon(struct cv_output *output, struct cv_desktop *desktop);
void overview_tile(struct cv_output *output, struct cv_desktop *desktop);
void overview_combine(struct cv_output *output, struct cv_desktop *into);
void overview_new_desktop(struct cv_output *output, double cx, double cy);
struct wlr_box target_box_at(struct cv_output *output, double cx, double cy);

/* view.c */
void view_init(struct cv_server *server);
void view_finish(struct cv_server *server);
struct cv_view *view_at(struct cv_server *server, double cx, double cy, double zoom,
	struct wlr_surface **surface, double *sx, double *sy,
	enum cv_deco_part *part, uint32_t *edges);
struct cv_view *view_from_surface(struct wlr_surface *surface);
struct wlr_box view_content_box(struct cv_view *view);
struct wlr_box view_frame_box(struct cv_view *view);
struct rect view_frame_rect(struct cv_view *view);
void view_focus(struct cv_view *view);
void view_close(struct cv_view *view);
void view_set_minimized(struct cv_view *view, bool minimized);
void view_set_maximized(struct cv_view *view, bool maximized);
void view_set_fullscreen(struct cv_view *view, bool fullscreen);
void view_set_frame(struct cv_view *view, struct wlr_box frame, bool tiled);
void view_move(struct cv_view *view, int x, int y);
void view_activate(struct cv_view *view);
void view_damage(struct cv_view *view);
void view_for_each_surface(struct cv_view *view, wlr_surface_iterator_func_t fn, void *data);
struct cv_view *view_top_visible(struct cv_server *server, struct cv_output *output);
struct cv_view *view_last_minimized(struct cv_server *server);
void focus_surface_keyboard(struct cv_server *server, struct wlr_surface *surface);

/* decoration.c */
void decoration_init(struct cv_server *server);
void decoration_finish(struct cv_server *server);
int deco_titlebar(struct cv_view *view);
int deco_border(struct cv_view *view);
enum cv_deco_part deco_hit(struct cv_view *view, double cx, double cy, double zoom,
	uint32_t *edges);
void deco_update(struct cv_view *view, float scale);
void deco_finish(struct cv_view *view);
/* View initial commit: send the SERVER_SIDE mode once the toplevel is
 * initialized (called from view.c's commit handler). */
void deco_handle_initial_commit(struct cv_view *view);
/* Recompute view->ssd from the negotiated decoration modes; damages when it
 * flips. Called on map and whenever a mode changes. */
void deco_refresh_mode(struct cv_view *view);

/* layer.c */
void layer_init(struct cv_server *server);
void layer_finish(struct cv_server *server);
void output_arrange_layers(struct cv_output *output);
struct wlr_surface *layer_surface_at(struct cv_output *output, int layer,
	double ox, double oy, double *sx, double *sy, struct cv_layer **out);
void layer_refocus(struct cv_server *server);
bool output_has_wallpaper(struct cv_output *output);

/* input.c */
void input_init(struct cv_server *server);
void input_finish(struct cv_server *server);
void cursor_rebase(struct cv_server *server);
bool cursor_canvas_pos(struct cv_server *server, struct cv_output **output,
	double *cx, double *cy);
void cursor_set_image(struct cv_server *server, const char *name);
void input_cancel_grab(struct cv_server *server);

/* gesture.c */
void gesture_swipe_begin(struct cv_server *server, uint32_t time, uint32_t fingers);
void gesture_swipe_update(struct cv_server *server, uint32_t time, double dx, double dy);
void gesture_swipe_end(struct cv_server *server, uint32_t time, bool cancelled);
void gesture_pinch_begin(struct cv_server *server, uint32_t time, uint32_t fingers);
void gesture_pinch_update(struct cv_server *server, uint32_t time, double dx, double dy,
	double scale, double rotation);
void gesture_pinch_end(struct cv_server *server, uint32_t time, bool cancelled);
void gesture_hold_begin(struct cv_server *server, uint32_t time, uint32_t fingers);
void gesture_hold_end(struct cv_server *server, uint32_t time, bool cancelled);

/* render.c */
void render_output(struct cv_output *output);
void render_finish(struct cv_server *server);
/* Shows `text` as a banner at the bottom of every output for ~2 s (NULL clears). */
void hint_set(struct cv_server *server, const char *text);
/*
 * Overview UI geometry, output-local logical coordinates; input.c hit-tests
 * exactly what render.c draws. The tab sits on top of the desktop's
 * top-left corner and carries the name plus Tile and Abandon buttons.
 * Returns false when the desktop's tab is not on this output.
 */
bool overview_tab_layout(struct cv_output *output, struct cv_desktop *desktop,
	struct wlr_box *tab, struct wlr_box *tile_button, struct wlr_box *abandon_button);
/* Minimized windows shelf along the bottom of an overview output. Fills up
 * to `max` views with their thumbnail boxes; returns the count. */
size_t overview_shelf_layout(struct cv_output *output, struct cv_view **views,
	struct wlr_box *boxes, size_t max);

/* draw.c: cairo/pango to texture */
struct wlr_texture *draw_text(struct wlr_renderer *renderer, const char *text,
	const char *font, uint32_t fg, uint32_t bg, float scale, int pad_x, int pad_y,
	int *width, int *height);
struct wlr_render_color color_rgba(uint32_t rgba, float alpha);
/* Uploads a CAIRO_FORMAT_ARGB32 image surface (premultiplied) as a texture. */
struct wlr_texture *texture_from_cairo(struct wlr_renderer *renderer, cairo_surface_t *surface);

/* apps.c: installed applications */
struct cv_apps {
	struct cv_app *items;
	size_t count;
};
/* Scans $XDG_DATA_HOME/applications then each $XDG_DATA_DIRS/applications
 * (defaults ~/.local/share, /usr/local/share:/usr/share); the first file with
 * a given desktop id wins, subdirectories map to "dir-name.desktop" ids, and
 * entries whose TryExec binary isn't on $PATH are dropped. Sorted by name,
 * case-insensitively. */
void apps_load(struct cv_apps *apps);
void apps_free(struct cv_apps *apps);
/* Indices of matching apps, best match first (ties alphabetical); an empty
 * query lists all. Returns the count written (<= max). */
size_t apps_search(const struct cv_apps *apps, const char *query, size_t *out, size_t max);
/* Runs the app via server_spawn; Terminal=true apps run inside $TERMINAL -e. */
void app_launch(const struct cv_app *app);

/* icons.c: freedesktop icon theme lookup */
/* `icon` is an absolute path or a theme icon name; `size` in device pixels.
 * Looks in the user's icon theme (gsettings/GTK settings), its Inherits chain,
 * hicolor, then pixmaps; loads PNG (cairo) or SVG (librsvg) scaled to fit
 * size x size. Cached per (icon, size): the cache owns the surface. NULL if
 * not found. */
cairo_surface_t *icon_lookup(const char *icon, int size);
void icons_finish(void);

/* launcher.c: the Start-menu-style app launcher overlay */
void launcher_toggle(struct cv_server *server, struct cv_output *output);
void launcher_close(struct cv_server *server);
bool launcher_is_open(struct cv_server *server);
/* While open the launcher owns the keyboard: every press is consumed.
 * `utf32` is the character the key produces (0 for none), `mods` the
 * WLR_MODIFIER_* mask. */
void launcher_key(struct cv_server *server, xkb_keysym_t sym, uint32_t utf32, uint32_t mods);
/* Pointer at output-local logical (ox, oy). Return true when the event is
 * consumed: over the panel, or (button) a click outside that closes it. */
bool launcher_pointer_motion(struct cv_server *server, struct cv_output *output,
	double ox, double oy);
bool launcher_pointer_button(struct cv_server *server, struct cv_output *output,
	double ox, double oy, uint32_t button, bool pressed);
bool launcher_pointer_axis(struct cv_server *server, struct cv_output *output,
	double ox, double oy, double delta);
/* Panel image for this output (rebuilt when stale) and its output-local
 * logical box; NULL unless the launcher is open on `output`. */
struct wlr_texture *launcher_texture(struct cv_server *server, struct cv_output *output,
	struct wlr_box *box);
/* Called by output.c before `output` is freed: closes the launcher if it is open there. */
void launcher_output_destroyed(struct cv_server *server, struct cv_output *output);
void launcher_finish(struct cv_server *server);

#endif
