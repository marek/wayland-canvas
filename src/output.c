#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>
#include "server.h"

static double pick_scale(struct cv_server *server, struct wlr_output *wlr_output,
		struct wlr_output_mode *mode) {
	if (server->options.scale > 0) {
		return server->options.scale;
	}
	int width = mode != NULL ? mode->width : wlr_output->width;
	int height = mode != NULL ? mode->height : wlr_output->height;
	if (wlr_output->phys_width <= 0 || wlr_output->phys_height <= 0) {
		return 1;
	}
	double dpi_x = width / (wlr_output->phys_width / 25.4);
	double dpi_y = height / (wlr_output->phys_height / 25.4);
	// GNOME's heuristic: 2x only for dense panels that stay large enough.
	return fmin(dpi_x, dpi_y) >= 192 && height >= 1200 ? 2 : 1;
}

void output_damage(struct cv_output *output) {
	output->dirty = true;
	wlr_output_schedule_frame(output->wlr_output);
}

void server_damage_all(struct cv_server *server) {
	struct cv_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		output_damage(output);
	}
}

struct cv_output *output_at(struct cv_server *server, double lx, double ly) {
	struct wlr_output *wlr_output = wlr_output_layout_output_at(server->output_layout, lx, ly);
	return wlr_output != NULL ? wlr_output->data : NULL;
}

struct cv_output *output_under_cursor(struct cv_server *server) {
	struct cv_output *output = output_at(server, server->cursor->x, server->cursor->y);
	if (output == NULL && !wl_list_empty(&server->outputs)) {
		output = wl_container_of(server->outputs.next, output, link);
	}
	return output;
}

struct camera output_camera(struct cv_output *output) {
	struct camera cam = output->cam;
	if (cam.zoom == 1) {
		// At 1:1 keep window pixels on the output's pixel grid: no blur.
		double scale = output->wlr_output->scale;
		double left = round((cam.cx - output->width / 2.0) * scale) / scale;
		double top = round((cam.cy - output->height / 2.0) * scale) / scale;
		cam.cx = left + output->width / 2.0;
		cam.cy = top + output->height / 2.0;
	}
	return cam;
}

struct rect output_viewport(struct cv_output *output) {
	return camera_viewport(output_camera(output), output->width, output->height);
}

void output_local_to_canvas(struct cv_output *output, double ox, double oy,
		double *cx, double *cy) {
	struct camera cam = output_camera(output);
	*cx = cam.cx + (ox - output->width / 2.0) / cam.zoom;
	*cy = cam.cy + (oy - output->height / 2.0) / cam.zoom;
}

void output_canvas_to_local(struct cv_output *output, double cx, double cy,
		double *ox, double *oy) {
	struct camera cam = output_camera(output);
	*ox = (cx - cam.cx) * cam.zoom + output->width / 2.0;
	*oy = (cy - cam.cy) * cam.zoom + output->height / 2.0;
}

struct wlr_box output_usable_canvas_box(struct cv_output *output, struct wlr_box area) {
	if (output == NULL) {
		return area;
	}
	int left = output->usable.x, top = output->usable.y;
	int right = output->width - (output->usable.x + output->usable.width);
	int bottom = output->height - (output->usable.y + output->usable.height);
	area.x += left;
	area.y += top;
	area.width -= left + right;
	area.height -= top + bottom;
	return area;
}

struct outputs_update {
	struct cv_server *server;
	uint32_t mask;
	double scale;
};

static void update_surface_outputs(struct wlr_surface *surface, int sx, int sy, void *data) {
	struct outputs_update *update = data;
	struct cv_output *output;
	wl_list_for_each(output, &update->server->outputs, link) {
		if (update->mask & output->bit) {
			wlr_surface_send_enter(surface, output->wlr_output);
		} else {
			wlr_surface_send_leave(surface, output->wlr_output);
		}
	}
	wlr_fractional_scale_v1_notify_scale(surface, update->scale);
	wlr_surface_set_preferred_buffer_scale(surface, (int32_t)ceil(update->scale));
}

static void view_update_outputs(struct cv_view *view, uint32_t mask) {
	uint32_t old = view->output_mask;
	view->output_mask = mask;
	// Like wlr_scene: a view scrolled off every output keeps its last
	// outputs, so panning away and back never makes clients re-render.
	if (mask == 0 || mask == old) {
		return;
	}
	struct outputs_update update = {.server = view->server, .mask = mask, .scale = 1};
	struct cv_output *output;
	wl_list_for_each(output, &view->server->outputs, link) {
		if ((mask & output->bit) && output->wlr_output->scale > update.scale) {
			update.scale = output->wlr_output->scale;
		}
	}
	view_for_each_surface(view, update_surface_outputs, &update);
	if (view->foreign == NULL) {
		return;
	}
	wl_list_for_each(output, &view->server->outputs, link) {
		bool was = old & output->bit, is = mask & output->bit;
		if (is && !was) {
			wlr_foreign_toplevel_handle_v1_output_enter(view->foreign, output->wlr_output);
		} else if (was && !is) {
			wlr_foreign_toplevel_handle_v1_output_leave(view->foreign, output->wlr_output);
		}
	}
}

void server_update_visibility(struct cv_server *server) {
	server->visibility_dirty = false;
	struct cv_view *view;
	wl_list_for_each(view, &server->views, link) {
		uint32_t mask = 0;
		if (view->mapped && !view->minimized) {
			struct rect frame = view_frame_rect(view);
			struct cv_output *output;
			wl_list_for_each(output, &server->outputs, link) {
				if (rect_intersects(frame, output_viewport(output))) {
					mask |= output->bit;
				}
			}
		}
		view_update_outputs(view, mask);
	}
}

static void update_output_geometry(struct cv_output *output) {
	struct wlr_box box;
	wlr_output_layout_get_box(output->server->output_layout, output->wlr_output, &box);
	if (wlr_box_empty(&box)) {
		return;
	}
	bool resized = box.width != output->width || box.height != output->height;
	output->lx = box.x;
	output->ly = box.y;
	output->width = box.width;
	output->height = box.height;
	if (resized) {
		output->usable = (struct wlr_box){0, 0, box.width, box.height};
		output_arrange_layers(output);
	}
	output_damage(output);
}

static void handle_layout_change(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, layout_change);
	struct cv_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		update_output_geometry(output);
	}
	server->visibility_dirty = true;
}

static void handle_frame(struct wl_listener *listener, void *data) {
	struct cv_output *output = wl_container_of(listener, output, frame);
	bool animating = output_tick(output, now_ms());
	struct cv_server *server = output->server;
	if (server->visibility_dirty) {
		server_update_visibility(server);
	}
	if (server->rebase_cursor) {
		server->rebase_cursor = false;
		cursor_rebase(server);
	}
	// needs_frame: screencopy clients and software cursors wait for a commit.
	if (!output->dirty && !animating && !output->wlr_output->needs_frame) {
		return;
	}
	output->dirty = false;
	render_output(output);
	if (animating) {
		wlr_output_schedule_frame(output->wlr_output);
	}
}

static void handle_needs_frame(struct wl_listener *listener, void *data) {
	struct cv_output *output = wl_container_of(listener, output, needs_frame);
	wlr_output_schedule_frame(output->wlr_output);
}

static void handle_request_state(struct wl_listener *listener, void *data) {
	// Nested backends: the host resized our window.
	struct cv_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
	output_damage(output);
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct cv_output *output = wl_container_of(listener, output, destroy);
	struct cv_server *server = output->server;
	for (int i = 0; i < 4; i++) {
		struct cv_layer *layer, *tmp;
		wl_list_for_each_safe(layer, tmp, &output->layers[i], link) {
			layer->output = NULL;
			wl_list_remove(&layer->link);
			wl_list_init(&layer->link);
			wlr_layer_surface_v1_destroy(layer->layer_surface);
		}
	}
	launcher_output_destroyed(server, output);
	if (server->grab_output == output) {
		input_cancel_grab(server);
	}
	if (server->gesture.output == output) {
		server->gesture.output = NULL;
		// A forwarded gesture still owes its client the end event.
		if (server->gesture.kind != CV_GESTURE_FORWARD) {
			if (server->gesture.kind == CV_GESTURE_ABANDON &&
					server->gesture.desktop != NULL) {
				server->gesture.desktop->swipe_offset = 0;
			}
			server->gesture.kind = CV_GESTURE_IGNORED;
		}
	}
	struct cv_view *view;
	wl_list_for_each(view, &server->views, link) {
		view->output_mask &= ~output->bit;
	}
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->needs_frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
	server->visibility_dirty = true;
}

static void handle_new_output(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;
	if (wlr_output->non_desktop) {
		return;
	}
	uint32_t used = 0;
	struct cv_output *existing;
	wl_list_for_each(existing, &server->outputs, link) {
		used |= existing->bit;
	}
	if (used == UINT32_MAX) {
		wlr_log(WLR_ERROR, "too many outputs, ignoring %s", wlr_output->name);
		return;
	}
	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}
	wlr_output_state_set_scale(&state, pick_scale(server, wlr_output, mode));
	if (!wlr_output_commit_state(wlr_output, &state)) {
		wlr_log(WLR_ERROR, "failed to enable %s with its preferred mode", wlr_output->name);
		wlr_output_state_finish(&state);
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, true);
		wlr_output_commit_state(wlr_output, &state);
	}
	wlr_output_state_finish(&state);

	struct cv_output *output = calloc(1, sizeof(*output));
	if (output == NULL) {
		return;
	}
	output->server = server;
	output->wlr_output = wlr_output;
	wlr_output->data = output;
	output->bit = 1;
	while (used & output->bit) {
		output->bit <<= 1;
	}
	for (int i = 0; i < 4; i++) {
		wl_list_init(&output->layers[i]);
	}
	output->cam.zoom = 1;
	output->frame.notify = handle_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->needs_frame.notify = handle_needs_frame;
	wl_signal_add(&wlr_output->events.needs_frame, &output->needs_frame);
	output->request_state.notify = handle_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = handle_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	wl_list_insert(server->outputs.prev, &output->link);

	wlr_output_layout_add_auto(server->output_layout, wlr_output);
	update_output_geometry(output);
	// The canvas starts out aligned with the output layout.
	output->cam = (struct camera){
		.cx = output->lx + output->width / 2.0,
		.cy = output->ly + output->height / 2.0,
		.zoom = 1,
	};
	if (wl_list_empty(&server->desktops)) {
		desktop_create(server, (struct wlr_box){
			output->lx, output->ly, output->width, output->height});
	}
	wlr_log(WLR_INFO, "output %s: %dx%d logical at scale %.2f", wlr_output->name,
		output->width, output->height, wlr_output->scale);
	server->visibility_dirty = true;
	output_damage(output);
}

void output_init(struct cv_server *server) {
	server->new_output.notify = handle_new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);
	server->layout_change.notify = handle_layout_change;
	wl_signal_add(&server->output_layout->events.change, &server->layout_change);
}

void output_finish(struct cv_server *server) {
	wl_list_remove(&server->new_output.link);
	wl_list_remove(&server->layout_change.link);
}
