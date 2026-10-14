#include <stdlib.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include "server.h"

/* Layers arranged topmost first, so exclusive zones stack sensibly. */
static const enum zwlr_layer_shell_v1_layer layer_order[] = {
	ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
	ZWLR_LAYER_SHELL_V1_LAYER_TOP,
	ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
	ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
};

/* Position and size one layer surface; same math as
 * wlr_scene_layer_surface_v1_configure. Updates `usable` when the surface
 * reserves an exclusive zone. */
static void layer_configure(struct cv_layer *layer, const struct wlr_box *full,
		struct wlr_box *usable) {
	struct wlr_layer_surface_v1 *ls = layer->layer_surface;
	struct wlr_layer_surface_v1_state *state = &ls->current;

	// Zone -1: cover the whole output, ignoring other exclusive zones.
	struct wlr_box bounds = state->exclusive_zone == -1 ? *full : *usable;

	struct wlr_box box = {
		.width = state->desired_width,
		.height = state->desired_height,
	};

	// Horizontal positioning.
	if (box.width == 0) {
		box.x = bounds.x + state->margin.left;
		box.width = bounds.width - (state->margin.left + state->margin.right);
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT &&
			state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) {
		box.x = bounds.x + bounds.width / 2 - box.width / 2;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) {
		box.x = bounds.x + state->margin.left;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) {
		box.x = bounds.x + bounds.width - box.width - state->margin.right;
	} else {
		box.x = bounds.x + bounds.width / 2 - box.width / 2;
	}

	// Vertical positioning.
	if (box.height == 0) {
		box.y = bounds.y + state->margin.top;
		box.height = bounds.height - (state->margin.top + state->margin.bottom);
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP &&
			state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) {
		box.y = bounds.y + bounds.height / 2 - box.height / 2;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) {
		box.y = bounds.y + state->margin.top;
	} else if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) {
		box.y = bounds.y + bounds.height - box.height - state->margin.bottom;
	} else {
		box.y = bounds.y + bounds.height / 2 - box.height / 2;
	}

	layer->geo = box;
	// configure() asserts on surfaces that have not done their initial commit.
	if (ls->initialized) {
		wlr_layer_surface_v1_configure(ls, box.width, box.height);
	}

	if (ls->surface->mapped && state->exclusive_zone > 0) {
		enum wlr_edges edge = wlr_layer_surface_v1_get_exclusive_edge(ls);
		switch (edge) {
		case WLR_EDGE_TOP:
			usable->y += state->exclusive_zone + state->margin.top;
			usable->height -= state->exclusive_zone + state->margin.top;
			break;
		case WLR_EDGE_BOTTOM:
			usable->height -= state->exclusive_zone + state->margin.bottom;
			break;
		case WLR_EDGE_LEFT:
			usable->x += state->exclusive_zone + state->margin.left;
			usable->width -= state->exclusive_zone + state->margin.left;
			break;
		case WLR_EDGE_RIGHT:
			usable->width -= state->exclusive_zone + state->margin.right;
			break;
		default:
			return;
		}
		if (usable->width < 0) {
			usable->width = 0;
		}
		if (usable->height < 0) {
			usable->height = 0;
		}
	}
}

void output_arrange_layers(struct cv_output *output) {
	struct wlr_box full = {0, 0, output->width, output->height};
	struct wlr_box usable = full;

	/* Surfaces reserving an exclusive zone are arranged first (overlay,
	 * top, bottom, background) so the rest fill what is left. */
	for (int pass = 0; pass < 2; pass++) {
		for (size_t i = 0; i < sizeof(layer_order) / sizeof(layer_order[0]); i++) {
			struct cv_layer *layer;
			wl_list_for_each(layer, &output->layers[layer_order[i]], link) {
				bool exclusive = layer->layer_surface->current.exclusive_zone > 0;
				if ((pass == 0) != exclusive) {
					continue;
				}
				layer_configure(layer, &full, &usable);
			}
		}
	}

	output->usable = usable;
	output_damage(output);
	output->server->visibility_dirty = true;
}

static void layer_handle_map(struct wl_listener *listener, void *data) {
	struct cv_layer *layer = wl_container_of(listener, layer, map);
	layer->mapped = true;
	if (layer->output != NULL) {
		// The surface's exclusive zone (if any) takes effect now.
		output_arrange_layers(layer->output);
	}
	layer_refocus(layer->server);
}

static void layer_handle_unmap(struct wl_listener *listener, void *data) {
	struct cv_layer *layer = wl_container_of(listener, layer, unmap);
	layer->mapped = false;
	if (layer->output != NULL) {
		// Release the exclusive zone the surface was reserving.
		output_arrange_layers(layer->output);
	}
	layer_refocus(layer->server);
}

static void layer_handle_commit(struct wl_listener *listener, void *data) {
	struct cv_layer *layer = wl_container_of(listener, layer, commit);
	struct wlr_layer_surface_v1 *ls = layer->layer_surface;
	// current.committed holds the state fields changed by this commit.
	if (!ls->initial_commit && ls->current.committed == 0) {
		return;
	}
	if (layer->output == NULL) {
		return;
	}
	if (ls->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		// Layer change: move to the list of the new layer.
		wl_list_remove(&layer->link);
		wl_list_insert(&layer->output->layers[ls->current.layer], &layer->link);
	}
	output_arrange_layers(layer->output);
}

static void layer_handle_destroy(struct wl_listener *listener, void *data) {
	struct cv_layer *layer = wl_container_of(listener, layer, destroy);
	struct cv_server *server = layer->server;

	wl_list_remove(&layer->map.link);
	wl_list_remove(&layer->unmap.link);
	wl_list_remove(&layer->commit.link);
	wl_list_remove(&layer->destroy.link);
	wl_list_remove(&layer->link);

	if (server->focused_layer == layer) {
		server->focused_layer = NULL;
	}
	if (layer->output != NULL) {
		output_arrange_layers(layer->output);
	}
	layer_refocus(server);
	free(layer);
}

static void handle_new_layer_surface(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *ls = data;

	struct cv_output *output = ls->output != NULL ? ls->output->data
		: output_under_cursor(server);
	if (output == NULL) {
		wlr_log(WLR_ERROR, "layer surface with no output to place it on");
		wlr_layer_surface_v1_destroy(ls);
		return;
	}
	ls->output = output->wlr_output;

	struct cv_layer *layer = calloc(1, sizeof(*layer));
	if (layer == NULL) {
		wlr_log(WLR_ERROR, "out of memory for a layer surface");
		wlr_layer_surface_v1_destroy(ls);
		return;
	}
	layer->server = server;
	layer->output = output;
	layer->layer_surface = ls;
	ls->data = layer;
	// Head of a layer list is the topmost (newest) surface.
	wl_list_insert(&output->layers[ls->pending.layer], &layer->link);

	layer->map.notify = layer_handle_map;
	wl_signal_add(&ls->surface->events.map, &layer->map);
	layer->unmap.notify = layer_handle_unmap;
	wl_signal_add(&ls->surface->events.unmap, &layer->unmap);
	layer->commit.notify = layer_handle_commit;
	wl_signal_add(&ls->surface->events.commit, &layer->commit);
	layer->destroy.notify = layer_handle_destroy;
	wl_signal_add(&ls->events.destroy, &layer->destroy);
}

struct wlr_surface *layer_surface_at(struct cv_output *output, int layer,
		double ox, double oy, double *sx, double *sy, struct cv_layer **out) {
	struct cv_layer *l;
	wl_list_for_each(l, &output->layers[layer], link) {
		if (!l->mapped) {
			continue;
		}
		struct wlr_surface *surface = wlr_layer_surface_v1_surface_at(
			l->layer_surface, ox - l->geo.x, oy - l->geo.y, sx, sy);
		if (surface != NULL) {
			if (out != NULL) {
				*out = l;
			}
			return surface;
		}
	}
	return NULL;
}

void layer_refocus(struct cv_server *server) {
	/* The topmost mapped exclusively keyboard-interactive surface, overlay
	 * before top, owns the keyboard. */
	for (size_t i = 0; i < 2; i++) {
		struct cv_output *output;
		wl_list_for_each(output, &server->outputs, link) {
			struct cv_layer *layer;
			wl_list_for_each(layer, &output->layers[layer_order[i]], link) {
				if (!layer->mapped || layer->layer_surface->current.keyboard_interactive !=
						ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
					continue;
				}
				server->focused_layer = layer;
				focus_surface_keyboard(server, layer->layer_surface->surface);
				return;
			}
		}
	}

	struct cv_layer *focused = server->focused_layer;
	if (focused == NULL || focused->mapped) {
		// A still-mapped ON_DEMAND surface keeps its click focus.
		return;
	}
	server->focused_layer = NULL;
	struct cv_view *view = server->focused_view;
	if (view != NULL && view->mapped) {
		focus_surface_keyboard(server, view->toplevel->base->surface);
	} else {
		focus_surface_keyboard(server, NULL);
	}
}

bool output_has_wallpaper(struct cv_output *output) {
	struct cv_layer *layer;
	wl_list_for_each(layer,
			&output->layers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND], link) {
		if (layer->mapped) {
			return true;
		}
	}
	return false;
}

void layer_init(struct cv_server *server) {
	server->layer_shell = wlr_layer_shell_v1_create(server->display, 4);
	if (server->layer_shell == NULL) {
		wlr_log(WLR_ERROR, "failed to create the layer shell");
		wl_list_init(&server->new_layer_surface.link);
		return;
	}
	server->new_layer_surface.notify = handle_new_layer_surface;
	wl_signal_add(&server->layer_shell->events.new_surface,
		&server->new_layer_surface);
}

void layer_finish(struct cv_server *server) {
	wl_list_remove(&server->new_layer_surface.link);
}
