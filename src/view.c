#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/edges.h>
#include "config.h"
#include "server.h"

/* view_focus(NULL) clears the focus and still needs the server, so keep the
 * single compositor instance here (view_init records it). */
static struct cv_server *view_server;

/* ---- geometry ---- */

struct wlr_box view_content_box(struct cv_view *view) {
	struct wlr_box geo = view->toplevel->base->geometry;
	return (struct wlr_box){view->x, view->y, geo.width, geo.height};
}

struct wlr_box view_frame_box(struct cv_view *view) {
	struct wlr_box content = view_content_box(view);
	int tb = deco_titlebar(view);
	int bw = deco_border(view);
	return (struct wlr_box){
		content.x - bw,
		content.y - tb - bw,
		content.width + 2 * bw,
		content.height + tb + 2 * bw,
	};
}

struct rect view_frame_rect(struct cv_view *view) {
	struct wlr_box box = view_frame_box(view);
	return (struct rect){box.x, box.y, box.width, box.height};
}

static struct rect rect_of_box(struct wlr_box box) {
	return (struct rect){box.x, box.y, box.width, box.height};
}

static struct wlr_box box_of_viewport(struct rect r) {
	return (struct wlr_box){
		(int)round(r.x), (int)round(r.y),
		(int)ceil(r.w), (int)ceil(r.h),
	};
}

/* ---- maximize/fullscreen areas ---- */

static struct wlr_box inset_usable(struct cv_output *output, struct wlr_box area) {
	if (output == NULL) {
		return area;
	}
	struct wlr_box inset = output_usable_canvas_box(output, area);
	if (inset.width < 1 || inset.height < 1) {
		return area;
	}
	return inset;
}

/* The viewport of the output showing the view's frame centre in normal mode,
 * else its desktop box, else a screen-sized box centred on the frame. */
static struct wlr_box maximize_area(struct cv_view *view) {
	struct cv_server *server = view->server;
	struct rect frame = view_frame_rect(view);
	struct cv_output *picked = NULL;
	struct cv_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->mode != CV_MODE_NORMAL) {
			continue;
		}
		if (rect_contains(output_viewport(output),
				rect_cx(frame), rect_cy(frame))) {
			picked = output;
			break;
		}
	}
	if (picked != NULL) {
		return inset_usable(picked, box_of_viewport(output_viewport(picked)));
	}
	struct cv_desktop *desktop = desktop_of_view(server, view);
	if (desktop != NULL) {
		return inset_usable(output_under_cursor(server), desktop->box);
	}
	struct cv_output *any = output_under_cursor(server);
	int w = any != NULL ? any->width : 1280;
	int h = any != NULL ? any->height : 720;
	struct wlr_box area = {
		(int)round(rect_cx(frame)) - w / 2,
		(int)round(rect_cy(frame)) - h / 2,
		w, h,
	};
	return inset_usable(any, area);
}

/* Fullscreen covers the whole output viewport, no decorations, no inset. */
static struct wlr_box fullscreen_area(struct cv_view *view) {
	struct cv_server *server = view->server;
	struct rect frame = view_frame_rect(view);
	struct cv_output *picked = NULL;
	struct cv_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (rect_contains(output_viewport(output),
				rect_cx(frame), rect_cy(frame))) {
			picked = output;
			break;
		}
	}
	if (picked == NULL) {
		picked = output_under_cursor(server);
	}
	if (picked == NULL) {
		return (struct wlr_box){
			(int)round(rect_cx(frame)) - 640,
			(int)round(rect_cy(frame)) - 360,
			1280, 720,
		};
	}
	return box_of_viewport(output_viewport(picked));
}

/* ---- placement ---- */

static void view_place(struct cv_view *view) {
	struct cv_server *server = view->server;
	struct cv_output *output = output_under_cursor(server);
	if (output == NULL) {
		view->x = 0;
		view->y = 0;
		return;
	}
	struct wlr_box geo = view->toplevel->base->geometry;
	int tb = deco_titlebar(view);
	int bw = deco_border(view);
	int fw = geo.width + 2 * bw;
	int fh = geo.height + tb + 2 * bw;

	if (view->fullscreen) {
		struct wlr_box area = fullscreen_area(view);
		view->x = area.x;
		view->y = area.y;
		return;
	}
	if (view->maximized) {
		struct wlr_box area = maximize_area(view);
		view->x = area.x;
		view->y = area.y + deco_titlebar(view);
		return;
	}

	// Inside the screen minus bars (exclusive zones), or the overview target.
	struct wlr_box screen = output->mode == CV_MODE_OVERVIEW ? output->target
		: box_of_viewport(output_viewport(output));
	struct rect area = rect_of_box(output_usable_canvas_box(output, screen));
	double px = area.x + (area.w - fw) / 2.0;
	double py = area.y + (area.h - fh) / 2.0;
	if (fw > area.w) {
		px = area.x;
	}
	if (fh > area.h) {
		py = area.y;
	}
	px = clampd(px, area.x, fmax(area.x, area.x + area.w - fw));
	py = clampd(py, area.y, fmax(area.y, area.y + area.h - fh));
	// px/py place the frame; view->x/y is the content inside it.
	int x = (int)round(px) + bw;
	int y = (int)round(py) + tb + bw;

	/* Cascade by 32px while another view occupies the chosen position. */
	int step = 0;
	while (step < 64) {
		bool clash = false;
		struct cv_view *other;
		wl_list_for_each(other, &server->views, link) {
			if (other == view || !other->mapped) {
				continue;
			}
			if (other->x == x + 32 * step && other->y == y + 32 * step) {
				clash = true;
				break;
			}
		}
		if (!clash) {
			break;
		}
		step++;
	}
	x += 32 * step;
	y += 32 * step;
	/* Keep the title bar on screen after cascading. */
	int frame_top = y - tb - bw;
	int bottom = (int)floor(area.y + area.h);
	if (frame_top + tb > bottom) {
		frame_top = bottom - tb;
	}
	if (frame_top < (int)ceil(area.y)) {
		frame_top = (int)ceil(area.y);
	}
	view->x = x;
	view->y = frame_top + tb + bw;
}

/* ---- state helpers ---- */

static bool view_initialized(struct cv_view *view) {
	return view->toplevel->base->initialized;
}

void view_damage(struct cv_view *view) {
	struct rect frame = view_frame_rect(view);
	struct cv_output *output;
	wl_list_for_each(output, &view->server->outputs, link) {
		if ((view->output_mask & output->bit)
				|| rect_intersects(frame, output_viewport(output))) {
			output_damage(output);
		}
	}
}

void view_move(struct cv_view *view, int x, int y) {
	if (view->x == x && view->y == y) {
		return;
	}
	view_damage(view);
	view->x = x;
	view->y = y;
	view_damage(view);
	view->server->visibility_dirty = true;
}

void focus_surface_keyboard(struct cv_server *server, struct wlr_surface *surface) {
	if (surface == NULL) {
		wlr_seat_keyboard_notify_clear_focus(server->seat);
		return;
	}
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	if (keyboard == NULL) {
		return;
	}
	wlr_seat_keyboard_notify_enter(server->seat, surface,
		keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
}

void view_focus(struct cv_view *view) {
	struct cv_server *server = view != NULL ? view->server : view_server;
	if (server == NULL) {
		return;
	}
	struct cv_view *prev = server->focused_view;
	if (prev == view) {
		return;
	}
	if (prev != NULL && prev->toplevel != NULL) {
		wlr_xdg_toplevel_set_activated(prev->toplevel, false);
		if (prev->foreign != NULL) {
			wlr_foreign_toplevel_handle_v1_set_activated(prev->foreign, false);
		}
		view_damage(prev);
	}
	server->focused_view = view;
	if (view != NULL) {
		/* Raise to the top of the stacking order. */
		wl_list_remove(&view->link);
		wl_list_insert(&server->views, &view->link);
		wlr_xdg_toplevel_set_activated(view->toplevel, true);
		if (view->foreign != NULL) {
			wlr_foreign_toplevel_handle_v1_set_activated(view->foreign, true);
		}
		view_damage(view);
	}
	if (server->focused_layer == NULL) {
		focus_surface_keyboard(server,
			view != NULL ? view->toplevel->base->surface : NULL);
	}
}

void view_close(struct cv_view *view) {
	if (view->toplevel != NULL) {
		wlr_xdg_toplevel_send_close(view->toplevel);
	}
}

void view_activate(struct cv_view *view) {
	if (!view->mapped) {
		return;
	}
	if (view->minimized) {
		view_set_minimized(view, false);
	}
	view_focus(view);
	canvas_reveal_view(view->server, view);
}

void view_set_minimized(struct cv_view *view, bool minimized) {
	struct cv_server *server = view->server;
	if (view->minimized == minimized) {
		return;
	}
	view->minimized = minimized;
	if (minimized) {
		view->minimize_seq = ++server->minimize_counter;
		if (server->focused_view == view) {
			view_focus(view_top_visible(server, output_under_cursor(server)));
		}
	}
	if (view->foreign != NULL) {
		wlr_foreign_toplevel_handle_v1_set_minimized(view->foreign, minimized);
	}
	view_damage(view);
	server->visibility_dirty = true;
}

void view_set_maximized(struct cv_view *view, bool maximized) {
	struct cv_server *server = view->server;
	struct wlr_xdg_toplevel *toplevel = view->toplevel;
	if (view->maximized == maximized) {
		return;
	}
	view->resize_edges = 0; // a pending resize anchor must not fight the new box
	view->resize_end_serial = 0;
	if (maximized) {
		if (!view->maximized && !view->fullscreen) {
			view->saved = view_content_box(view);
		}
		view->maximized = true;
		if (view->fullscreen) {
			view->fullscreen = false;
			if (view_initialized(view)) {
				wlr_xdg_toplevel_set_fullscreen(toplevel, false);
			}
			if (view->foreign != NULL) {
				wlr_foreign_toplevel_handle_v1_set_fullscreen(view->foreign, false);
			}
		}
		struct wlr_box area = maximize_area(view);
		int tb = deco_titlebar(view);
		view->x = area.x;
		view->y = area.y + tb;
		if (view_initialized(view)) {
			wlr_xdg_toplevel_set_maximized(toplevel, true);
			wlr_xdg_toplevel_set_size(toplevel,
				area.width, area.height - tb);
		}
	} else {
		view->maximized = false;
		if (view_initialized(view)) {
			wlr_xdg_toplevel_set_maximized(toplevel, false);
			if (!wlr_box_empty(&view->saved)) {
				wlr_xdg_toplevel_set_size(toplevel,
					view->saved.width, view->saved.height);
				view->x = view->saved.x;
				view->y = view->saved.y;
			} else {
				wlr_xdg_toplevel_set_size(toplevel, 0, 0);
			}
		}
	}
	if (view->foreign != NULL) {
		wlr_foreign_toplevel_handle_v1_set_maximized(view->foreign, maximized);
	}
	view_damage(view);
	server->visibility_dirty = true;
}

void view_set_fullscreen(struct cv_view *view, bool fullscreen) {
	struct cv_server *server = view->server;
	struct wlr_xdg_toplevel *toplevel = view->toplevel;
	if (view->fullscreen == fullscreen) {
		return;
	}
	view->resize_edges = 0;
	view->resize_end_serial = 0;
	if (fullscreen) {
		if (!view->maximized && !view->fullscreen) {
			view->saved = view_content_box(view);
		}
		view->fullscreen = true;
		if (view->maximized) {
			view->maximized = false;
			if (view_initialized(view)) {
				wlr_xdg_toplevel_set_maximized(toplevel, false);
			}
			if (view->foreign != NULL) {
				wlr_foreign_toplevel_handle_v1_set_maximized(view->foreign, false);
			}
		}
		struct wlr_box area = fullscreen_area(view);
		view->x = area.x;
		view->y = area.y;
		if (view_initialized(view)) {
			wlr_xdg_toplevel_set_fullscreen(toplevel, true);
			wlr_xdg_toplevel_set_size(toplevel, area.width, area.height);
		}
		/* Fullscreen windows sit above everything else. */
		if (view->mapped) {
			wl_list_remove(&view->link);
			wl_list_insert(&server->views, &view->link);
		}
	} else {
		view->fullscreen = false;
		if (view_initialized(view)) {
			wlr_xdg_toplevel_set_fullscreen(toplevel, false);
			if (!wlr_box_empty(&view->saved)) {
				wlr_xdg_toplevel_set_size(toplevel,
					view->saved.width, view->saved.height);
				view->x = view->saved.x;
				view->y = view->saved.y;
			} else {
				wlr_xdg_toplevel_set_size(toplevel, 0, 0);
			}
		}
	}
	if (view->foreign != NULL) {
		wlr_foreign_toplevel_handle_v1_set_fullscreen(view->foreign, fullscreen);
	}
	view_damage(view);
	server->visibility_dirty = true;
}

void view_set_frame(struct cv_view *view, struct wlr_box frame, bool tiled) {
	struct cv_server *server = view->server;
	struct wlr_xdg_toplevel *toplevel = view->toplevel;
	int tb = deco_titlebar(view);
	view->resize_edges = 0;
	view->resize_end_serial = 0;
	int bw = deco_border(view);
	struct wlr_box content = {
		frame.x + bw,
		frame.y + tb + bw,
		frame.width - 2 * bw,
		frame.height - tb - 2 * bw,
	};
	if (content.width < 1) {
		content.width = 1;
	}
	if (content.height < 1) {
		content.height = 1;
	}
	bool was_maximized = view->maximized;
	bool was_fullscreen = view->fullscreen;
	view->maximized = false;
	view->fullscreen = false;
	if (view_initialized(view)) {
		if (was_maximized) {
			wlr_xdg_toplevel_set_maximized(toplevel, false);
		}
		if (was_fullscreen) {
			wlr_xdg_toplevel_set_fullscreen(toplevel, false);
		}
		wlr_xdg_toplevel_set_tiled(toplevel, tiled
			? WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT
			: 0);
		wlr_xdg_toplevel_set_size(toplevel, content.width, content.height);
	}
	if (view->foreign != NULL) {
		if (was_maximized) {
			wlr_foreign_toplevel_handle_v1_set_maximized(view->foreign, false);
		}
		if (was_fullscreen) {
			wlr_foreign_toplevel_handle_v1_set_fullscreen(view->foreign, false);
		}
	}
	view->x = content.x;
	view->y = content.y;
	view_damage(view);
	server->visibility_dirty = true;
}

/* ---- lookup ---- */

struct cv_view *view_from_surface(struct wlr_surface *surface) {
	if (surface == NULL) {
		return NULL;
	}
	surface = wlr_surface_get_root_surface(surface);
	struct wlr_xdg_popup *popup;
	while ((popup = wlr_xdg_popup_try_from_wlr_surface(surface)) != NULL
			&& popup->parent != NULL) {
		surface = wlr_surface_get_root_surface(popup->parent);
	}
	struct wlr_xdg_toplevel *toplevel =
		wlr_xdg_toplevel_try_from_wlr_surface(surface);
	if (toplevel == NULL || toplevel->base == NULL) {
		return NULL;
	}
	return toplevel->base->data;
}

struct cv_view *view_at(struct cv_server *server, double cx, double cy,
		double zoom, struct wlr_surface **surface, double *sx, double *sy,
		enum cv_deco_part *part, uint32_t *edges) {
	*surface = NULL;
	*sx = 0;
	*sy = 0;
	*part = CV_DECO_NONE;
	*edges = 0;
	struct cv_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (!view->mapped || view->minimized) {
			continue;
		}
		struct wlr_box geo = view->toplevel->base->geometry;
		/* Canvas point -> toplevel surface-local coordinates. */
		double lx = cx - (view->x - geo.x);
		double ly = cy - (view->y - geo.y);
		double sub_x = 0, sub_y = 0;
		struct wlr_surface *found = wlr_xdg_surface_surface_at(
			view->toplevel->base, lx, ly, &sub_x, &sub_y);
		if (found != NULL) {
			*surface = found;
			*sx = sub_x;
			*sy = sub_y;
			return view;
		}
		enum cv_deco_part hit = deco_hit(view, cx, cy, zoom, edges);
		if (hit != CV_DECO_NONE) {
			*part = hit;
			return view;
		}
	}
	return NULL;
}

struct cv_view *view_top_visible(struct cv_server *server,
		struct cv_output *output) {
	struct cv_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (!view->mapped || view->minimized) {
			continue;
		}
		if (output == NULL
				|| rect_intersects(view_frame_rect(view),
					output_viewport(output))) {
			return view;
		}
	}
	return NULL;
}

struct cv_view *view_last_minimized(struct cv_server *server) {
	struct cv_view *view, *best = NULL;
	wl_list_for_each(view, &server->views, link) {
		if (!view->minimized) {
			continue;
		}
		if (best == NULL || view->minimize_seq > best->minimize_seq) {
			best = view;
		}
	}
	return best;
}

void view_for_each_surface(struct cv_view *view, wlr_surface_iterator_func_t fn,
		void *data) {
	wlr_xdg_surface_for_each_surface(view->toplevel->base, fn, data);
}

/* ---- foreign toplevel handle ---- */

static void handle_foreign_activate(struct wl_listener *listener, void *data) {
	struct cv_view *view = wl_container_of(listener, view, foreign_activate);
	view_activate(view);
}

static void handle_foreign_maximize(struct wl_listener *listener, void *data) {
	struct cv_view *view = wl_container_of(listener, view, foreign_maximize);
	const struct wlr_foreign_toplevel_handle_v1_maximized_event *event = data;
	view_set_maximized(view, event->maximized);
}

static void handle_foreign_minimize(struct wl_listener *listener, void *data) {
	struct cv_view *view = wl_container_of(listener, view, foreign_minimize);
	const struct wlr_foreign_toplevel_handle_v1_minimized_event *event = data;
	view_set_minimized(view, event->minimized);
}

static void handle_foreign_fullscreen(struct wl_listener *listener, void *data) {
	struct cv_view *view = wl_container_of(listener, view, foreign_fullscreen);
	const struct wlr_foreign_toplevel_handle_v1_fullscreen_event *event = data;
	view_set_fullscreen(view, event->fullscreen);
}

static void handle_foreign_close(struct wl_listener *listener, void *data) {
	struct cv_view *view = wl_container_of(listener, view, foreign_close);
	view_close(view);
}

static void view_destroy_foreign(struct cv_view *view) {
	if (view->foreign == NULL) {
		return;
	}
	wl_list_remove(&view->foreign_activate.link);
	wl_list_remove(&view->foreign_maximize.link);
	wl_list_remove(&view->foreign_minimize.link);
	wl_list_remove(&view->foreign_fullscreen.link);
	wl_list_remove(&view->foreign_close.link);
	wlr_foreign_toplevel_handle_v1_destroy(view->foreign);
	view->foreign = NULL;
}

static void view_create_foreign(struct cv_view *view) {
	struct cv_server *server = view->server;
	view->foreign = wlr_foreign_toplevel_handle_v1_create(server->foreign_mgr);
	if (view->foreign == NULL) {
		return;
	}
	view->foreign->data = view;
	if (view->toplevel->title != NULL) {
		wlr_foreign_toplevel_handle_v1_set_title(view->foreign,
			view->toplevel->title);
	}
	if (view->toplevel->app_id != NULL) {
		wlr_foreign_toplevel_handle_v1_set_app_id(view->foreign,
			view->toplevel->app_id);
	}
	if (view->maximized) {
		wlr_foreign_toplevel_handle_v1_set_maximized(view->foreign, true);
	}
	if (view->minimized) {
		wlr_foreign_toplevel_handle_v1_set_minimized(view->foreign, true);
	}
	if (view->fullscreen) {
		wlr_foreign_toplevel_handle_v1_set_fullscreen(view->foreign, true);
	}
	view->foreign_activate.notify = handle_foreign_activate;
	wl_signal_add(&view->foreign->events.request_activate,
		&view->foreign_activate);
	view->foreign_maximize.notify = handle_foreign_maximize;
	wl_signal_add(&view->foreign->events.request_maximize,
		&view->foreign_maximize);
	view->foreign_minimize.notify = handle_foreign_minimize;
	wl_signal_add(&view->foreign->events.request_minimize,
		&view->foreign_minimize);
	view->foreign_fullscreen.notify = handle_foreign_fullscreen;
	wl_signal_add(&view->foreign->events.request_fullscreen,
		&view->foreign_fullscreen);
	view->foreign_close.notify = handle_foreign_close;
	wl_signal_add(&view->foreign->events.request_close,
		&view->foreign_close);
}

/* ---- toplevel events ---- */

static void handle_map(struct wl_listener *listener, void *data) {
	struct cv_view *view = wl_container_of(listener, view, map);
	struct cv_server *server = view->server;
	view->mapped = true;
	wl_list_insert(&server->views, &view->link);
	deco_refresh_mode(view);
	view_place(view);
	view_create_foreign(view);
	view_focus(view);
	server->visibility_dirty = true;
	view_damage(view);
}

static void handle_unmap(struct wl_listener *listener, void *data) {
	struct cv_view *view = wl_container_of(listener, view, unmap);
	struct cv_server *server = view->server;
	if (server->grab_view == view) {
		input_cancel_grab(server);
	}
	view->mapped = false;
	wl_list_remove(&view->link);
	wl_list_init(&view->link);
	view_destroy_foreign(view);
	if (server->focused_view == view) {
		server->focused_view = NULL;
		view_focus(view_top_visible(server, output_under_cursor(server)));
	}
	/* Reset state the client renegotiates when it remaps. */
	view->maximized = false;
	view->fullscreen = false;
	view->saved = (struct wlr_box){0, 0, 0, 0};
	view->resize_edges = 0;
	view->hover = CV_DECO_NONE;
	view->pressed = CV_DECO_NONE;
	server->visibility_dirty = true;
	view_damage(view);
}

static void handle_commit(struct wl_listener *listener, void *data) {
	struct cv_view *view = wl_container_of(listener, view, commit);
	struct wlr_xdg_surface *base = view->toplevel->base;
	if (base->initial_commit) {
		wlr_xdg_toplevel_set_wm_capabilities(view->toplevel,
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE
			| WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN
			| WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
		deco_handle_initial_commit(view);
		if (view->toplevel->requested.fullscreen) {
			view_set_fullscreen(view, true);
		} else if (view->toplevel->requested.maximized) {
			view_set_maximized(view, true);
		} else {
			wlr_xdg_toplevel_set_size(view->toplevel, 0, 0);
		}
		return;
	}
	if (!view->mapped) {
		return;
	}
	if (view->resize_edges != 0) {
		/* Interactive resize: keep the opposite edge anchored. */
		struct wlr_box geo = base->geometry;
		if (view->resize_edges & WLR_EDGE_LEFT) {
			view->x = view->resize_start.x + view->resize_start.width
				- geo.width;
		}
		if (view->resize_edges & WLR_EDGE_TOP) {
			view->y = view->resize_start.y + view->resize_start.height
				- geo.height;
		}
		if (view->resize_end_serial != 0 &&
				base->current.configure_serial >= view->resize_end_serial) {
			view->resize_edges = 0;
			view->resize_end_serial = 0;
		}
	}
	view_damage(view);
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct cv_view *view = wl_container_of(listener, view, destroy);
	struct cv_server *server = view->server;
	if (server->grab_view == view) {
		input_cancel_grab(server);
	}
	if (server->focused_view == view) {
		server->focused_view = NULL;
	}
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_minimize.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->set_app_id.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->link);
	if (view->xdg_decoration != NULL) {
		wl_list_remove(&view->deco_request_mode.link);
		wl_list_remove(&view->deco_destroy.link);
		view->xdg_decoration = NULL;
	}
	view_destroy_foreign(view);
	deco_finish(view);
	free(view);
}

/* ---- interactive move/resize requests ---- */

static bool grab_allowed(struct cv_server *server, struct cv_view *view) {
	/* Only honour requests stemming from a real button press on the
	 * focused window; anything else is a client trying its luck. */
	return server->focused_view == view
		&& server->cursor_mode == CV_CURSOR_PASSTHROUGH
		&& server->seat->pointer_state.button_count > 0;
}

static void handle_request_move(struct wl_listener *listener, void *data) {
	struct cv_view *view = wl_container_of(listener, view, request_move);
	struct cv_server *server = view->server;
	if (!grab_allowed(server, view)) {
		return;
	}
	server->cursor_mode = CV_CURSOR_MOVE;
	server->grab_view = view;
	server->grab_output = output_under_cursor(server);
	server->grab_lx = server->cursor->x;
	server->grab_ly = server->cursor->y;
	server->grab_box = view_content_box(view);
}

static void handle_request_resize(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct cv_view *view = wl_container_of(listener, view, request_resize);
	struct cv_server *server = view->server;
	if (!grab_allowed(server, view)) {
		return;
	}
	server->cursor_mode = CV_CURSOR_RESIZE;
	server->grab_view = view;
	server->grab_output = output_under_cursor(server);
	server->grab_lx = server->cursor->x;
	server->grab_ly = server->cursor->y;
	server->grab_box = view_content_box(view);
	server->grab_edges = event->edges;
	view->resize_edges = event->edges;
	view->resize_end_serial = 0;
	view->resize_start = view_content_box(view);
	wlr_xdg_toplevel_set_resizing(view->toplevel, true);
}

static void handle_request_maximize(struct wl_listener *listener, void *data) {
	struct cv_view *view = wl_container_of(listener, view, request_maximize);
	if (!view_initialized(view)) {
		return; /* the initial commit will honour the request */
	}
	view_set_maximized(view, view->toplevel->requested.maximized);
	wlr_xdg_surface_schedule_configure(view->toplevel->base);
}

static void handle_request_minimize(struct wl_listener *listener, void *data) {
	struct cv_view *view = wl_container_of(listener, view, request_minimize);
	if (!view_initialized(view)) {
		return;
	}
	view_set_minimized(view, view->toplevel->requested.minimized);
	wlr_xdg_surface_schedule_configure(view->toplevel->base);
}

static void handle_request_fullscreen(struct wl_listener *listener, void *data) {
	struct cv_view *view = wl_container_of(listener, view, request_fullscreen);
	if (!view_initialized(view)) {
		return;
	}
	view_set_fullscreen(view, view->toplevel->requested.fullscreen);
	wlr_xdg_surface_schedule_configure(view->toplevel->base);
}

static void handle_set_title(struct wl_listener *listener, void *data) {
	struct cv_view *view = wl_container_of(listener, view, set_title);
	if (view->foreign != NULL && view->toplevel->title != NULL) {
		wlr_foreign_toplevel_handle_v1_set_title(view->foreign,
			view->toplevel->title);
	}
	view_damage(view);
}

static void handle_set_app_id(struct wl_listener *listener, void *data) {
	struct cv_view *view = wl_container_of(listener, view, set_app_id);
	if (view->foreign != NULL && view->toplevel->app_id != NULL) {
		wlr_foreign_toplevel_handle_v1_set_app_id(view->foreign,
			view->toplevel->app_id);
	}
	view_damage(view);
}

static void handle_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct cv_server *server =
		wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;

	struct cv_view *view = calloc(1, sizeof(*view));
	if (view == NULL) {
		return;
	}
	view->server = server;
	view->toplevel = toplevel;
	toplevel->base->data = view;
	wl_list_init(&view->link);

	view->map.notify = handle_map;
	wl_signal_add(&toplevel->base->surface->events.map, &view->map);
	view->unmap.notify = handle_unmap;
	wl_signal_add(&toplevel->base->surface->events.unmap, &view->unmap);
	view->commit.notify = handle_commit;
	wl_signal_add(&toplevel->base->surface->events.commit, &view->commit);
	view->destroy.notify = handle_destroy;
	wl_signal_add(&toplevel->events.destroy, &view->destroy);
	view->request_move.notify = handle_request_move;
	wl_signal_add(&toplevel->events.request_move, &view->request_move);
	view->request_resize.notify = handle_request_resize;
	wl_signal_add(&toplevel->events.request_resize, &view->request_resize);
	view->request_maximize.notify = handle_request_maximize;
	wl_signal_add(&toplevel->events.request_maximize,
		&view->request_maximize);
	view->request_minimize.notify = handle_request_minimize;
	wl_signal_add(&toplevel->events.request_minimize,
		&view->request_minimize);
	view->request_fullscreen.notify = handle_request_fullscreen;
	wl_signal_add(&toplevel->events.request_fullscreen,
		&view->request_fullscreen);
	view->set_title.notify = handle_set_title;
	wl_signal_add(&toplevel->events.set_title, &view->set_title);
	view->set_app_id.notify = handle_set_app_id;
	wl_signal_add(&toplevel->events.set_app_id, &view->set_app_id);
}

/* ---- popups ---- */

/* Owner view of a popup: walk the popup parent chain to its root surface. */
static struct cv_view *popup_owner_view(struct wlr_xdg_popup *popup) {
	struct wlr_surface *surface = popup->parent;
	if (surface == NULL) {
		return NULL;
	}
	return view_from_surface(surface);
}

static struct cv_layer *popup_owner_layer(struct wlr_xdg_popup *popup) {
	struct wlr_surface *surface = popup->parent;
	if (surface == NULL) {
		return NULL;
	}
	struct wlr_xdg_popup *parent;
	while ((parent = wlr_xdg_popup_try_from_wlr_surface(surface)) != NULL
			&& parent->parent != NULL) {
		surface = parent->parent;
	}
	struct wlr_surface *root = wlr_surface_get_root_surface(surface);
	struct wlr_layer_surface_v1 *layer_surface =
		wlr_layer_surface_v1_try_from_wlr_surface(root);
	if (layer_surface == NULL) {
		return NULL;
	}
	return layer_surface->data;
}

/* Constrain the popup to the area its owner is visible in; the box must be
 * in the root parent surface's coordinate space. Returns false when no
 * owner was found (the caller then replies with a bare configure). */
static bool popup_unconstrain(struct cv_popup *popup) {
	struct wlr_xdg_popup *xdg_popup = popup->popup;

	struct cv_view *view = popup_owner_view(xdg_popup);
	if (view != NULL) {
		struct cv_server *server = view->server;
		/* The output whose viewport shows the frame centre, else the one
		 * under the cursor. */
		struct rect frame = view_frame_rect(view);
		struct cv_output *picked = NULL;
		struct cv_output *output;
		wl_list_for_each(output, &server->outputs, link) {
			if (rect_contains(output_viewport(output),
					rect_cx(frame), rect_cy(frame))) {
				picked = output;
				break;
			}
		}
		if (picked == NULL) {
			picked = output_under_cursor(server);
		}
		if (picked != NULL) {
			struct rect viewport = output_viewport(picked);
			struct wlr_box geo = view->toplevel->base->geometry;
			/* Canvas -> toplevel surface-local: the surface origin sits at
			 * (view->x - geo.x, view->y - geo.y) on the canvas. */
			struct wlr_box box = {
				(int)floor(viewport.x - (view->x - geo.x)),
				(int)floor(viewport.y - (view->y - geo.y)),
				(int)ceil(viewport.w),
				(int)ceil(viewport.h),
			};
			wlr_xdg_popup_unconstrain_from_box(xdg_popup, &box);
			return true;
		}
	}

	struct cv_layer *layer = popup_owner_layer(xdg_popup);
	if (layer != NULL && layer->output != NULL) {
		/* Output-local rect shifted into the layer surface's space. */
		struct wlr_box box = {
			-layer->geo.x,
			-layer->geo.y,
			layer->output->width,
			layer->output->height,
		};
		wlr_xdg_popup_unconstrain_from_box(xdg_popup, &box);
		return true;
	}
	return false;
}

static void popup_handle_commit(struct wl_listener *listener, void *data) {
	struct cv_popup *popup = wl_container_of(listener, popup, commit);
	if (popup->popup->base->initial_commit) {
		if (!popup_unconstrain(popup)) {
			wlr_xdg_surface_schedule_configure(popup->popup->base);
		}
	}
	/* Later commits damage through server.c's per-surface tracker. */
}

static void popup_handle_destroy(struct wl_listener *listener, void *data) {
	struct cv_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void handle_new_xdg_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *xdg_popup = data;
	struct cv_popup *popup = calloc(1, sizeof(*popup));
	if (popup == NULL) {
		return;
	}
	popup->popup = xdg_popup;
	popup->commit.notify = popup_handle_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = popup_handle_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* ---- setup/teardown ---- */

void view_init(struct cv_server *server) {
	view_server = server;
	server->xdg_shell = wlr_xdg_shell_create(server->display, 6);
	server->new_xdg_toplevel.notify = handle_new_xdg_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel,
		&server->new_xdg_toplevel);
	server->new_xdg_popup.notify = handle_new_xdg_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup,
		&server->new_xdg_popup);
}

void view_finish(struct cv_server *server) {
	wl_list_remove(&server->new_xdg_toplevel.link);
	wl_list_remove(&server->new_xdg_popup.link);
}
