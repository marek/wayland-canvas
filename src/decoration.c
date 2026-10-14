#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <pango/pangocairo.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/util/edges.h>
#include "config.h"
#include "server.h"

/* KDE server-decoration wrappers: deco_refresh_mode() looks the negotiated
 * mode up by surface. One static list: this compositor has a single server. */
struct cv_kde_deco {
	struct wl_list link;
	struct wlr_server_decoration *decoration;
	struct wl_listener mode, destroy;
};

static struct wl_list kde_decos;

static void set_source_rgba32(cairo_t *cr, uint32_t rgba) {
	cairo_set_source_rgba(cr,
		((rgba >> 24) & 0xff) / 255.0,
		((rgba >> 16) & 0xff) / 255.0,
		((rgba >> 8) & 0xff) / 255.0,
		(rgba & 0xff) / 255.0);
}

static void fill_rect(cairo_t *cr, double x, double y, double w, double h,
		uint32_t rgba) {
	set_source_rgba32(cr, rgba);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

/* ---- title bar drawing (Windows 11 dark look) ---- */

/* 10x10 logical glyph, centred in the button; 1px lines. Straight lines sit on
 * half-logical-pixel coordinates so they land on device pixel rows/columns at
 * every integer scale. */
static void draw_glyph(cairo_t *cr, enum cv_deco_part part, double gx, double gy) {
	cairo_set_line_width(cr, 1.0);
	cairo_new_path(cr);
	if (part == CV_DECO_MINIMIZE) {
		cairo_move_to(cr, gx + 0.5, gy + 9.5);
		cairo_line_to(cr, gx + 9.5, gy + 9.5);
		cairo_stroke(cr);
	} else if (part == CV_DECO_MAXIMIZE) {
		cairo_rectangle(cr, gx + 0.5, gy + 0.5, 9, 9);
		cairo_stroke(cr);
	} else if (part == CV_DECO_CLOSE) {
		cairo_move_to(cr, gx + 0.5, gy + 0.5);
		cairo_line_to(cr, gx + 9.5, gy + 9.5);
		cairo_move_to(cr, gx + 9.5, gy + 0.5);
		cairo_line_to(cr, gx + 0.5, gy + 9.5);
		cairo_stroke(cr);
	}
}

/* Restore glyph: back square offset up-right, front square filled with the
 * button's composed background so it occludes the back square. */
static void draw_restore_glyph(cairo_t *cr, double gx, double gy,
		uint32_t base, uint32_t overlay) {
	cairo_set_line_width(cr, 1.0);
	cairo_new_path(cr);
	cairo_rectangle(cr, gx + 3.5, gy + 0.5, 6, 6);
	cairo_stroke(cr);
	fill_rect(cr, gx + 1.5, gy + 4.5, 5, 5, base);
	if (overlay != 0) {
		fill_rect(cr, gx + 1.5, gy + 4.5, 5, 5, overlay);
	}
	cairo_rectangle(cr, gx + 0.5, gy + 3.5, 6, 6);
	cairo_stroke(cr);
}

static void draw_button(cairo_t *cr, struct cv_view *view, enum cv_deco_part part,
		int width, bool active) {
	double x = width - (CV_DECO_CLOSE - part + 1) * BUTTON_WIDTH;
	double bw = BUTTON_WIDTH;
	double bh = TITLEBAR_HEIGHT;

	bool hover = view->hover == part;
	bool pressed = view->pressed == part;

	uint32_t base = active ? COLOR_TITLE_ACTIVE : COLOR_TITLE_INACTIVE;
	uint32_t overlay = 0;
	if (part == CV_DECO_CLOSE) {
		if (pressed) {
			fill_rect(cr, x, 0, bw, bh, COLOR_CLOSE_PRESS);
		} else if (hover) {
			fill_rect(cr, x, 0, bw, bh, COLOR_CLOSE_HOVER);
		}
	} else if (pressed) {
		overlay = COLOR_BUTTON_PRESS;
		fill_rect(cr, x, 0, bw, bh, overlay);
	} else if (hover) {
		overlay = COLOR_BUTTON_HOVER;
		fill_rect(cr, x, 0, bw, bh, overlay);
	}

	uint32_t fg = active ? COLOR_TEXT_ACTIVE : COLOR_TEXT_INACTIVE;
	if (part == CV_DECO_CLOSE && (hover || pressed)) {
		fg = COLOR_TEXT_ACTIVE;
	}
	set_source_rgba32(cr, fg);

	double gx = x + (bw - 10) / 2.0;
	double gy = (bh - 10) / 2.0;
	if (part == CV_DECO_MAXIMIZE && view->maximized) {
		draw_restore_glyph(cr, gx, gy, base, overlay);
	} else {
		draw_glyph(cr, part, gx, gy);
	}
}

void deco_update(struct cv_view *view, float scale) {
	struct cv_server *server = view->server;
	struct cv_deco *deco = &view->deco;
	const char *title = view->toplevel->title;
	bool active = server->focused_view == view;

	if (deco_titlebar(view) == 0) {
		if (deco->texture != NULL) {
			wlr_texture_destroy(deco->texture);
			deco->texture = NULL;
		}
		deco->width = 0;
		return;
	}

	struct wlr_box content = view_content_box(view);
	int width = content.width + 2 * deco_border(view);
	if (width < 1) {
		width = 1;
	}

	bool changed = deco->texture == NULL || deco->width != width
		|| deco->scale != scale || deco->active != active
		|| deco->maximized != view->maximized
		|| deco->hover != view->hover || deco->pressed != view->pressed
		|| (title == NULL) != (deco->title == NULL)
		|| (title != NULL && deco->title != NULL && strcmp(title, deco->title) != 0);
	if (!changed) {
		return;
	}

	free(deco->title);
	deco->title = title != NULL ? strdup(title) : NULL;

	cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		(int)ceil(width * scale), (int)ceil(TITLEBAR_HEIGHT * scale));
	cairo_t *cr = cairo_create(surf);
	cairo_scale(cr, scale, scale);

	uint32_t base = active ? COLOR_TITLE_ACTIVE : COLOR_TITLE_INACTIVE;
	fill_rect(cr, 0, 0, width, TITLEBAR_HEIGHT, base);

	draw_button(cr, view, CV_DECO_MINIMIZE, width, active);
	draw_button(cr, view, CV_DECO_MAXIMIZE, width, active);
	draw_button(cr, view, CV_DECO_CLOSE, width, active);

	if (deco->title != NULL) {
		PangoLayout *layout = pango_cairo_create_layout(cr);
		PangoFontDescription *desc =
			pango_font_description_from_string(TITLE_FONT);
		pango_layout_set_font_description(layout, desc);
		pango_font_description_free(desc);
		pango_layout_set_text(layout, deco->title, -1);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
		int avail = width - 2 * TITLE_PADDING - 3 * BUTTON_WIDTH;
		if (avail > 0) {
			pango_layout_set_width(layout, avail * PANGO_SCALE);
		}
		int lw = 0, lh = 0;
		pango_layout_get_pixel_size(layout, &lw, &lh);
		set_source_rgba32(cr, active ? COLOR_TEXT_ACTIVE : COLOR_TEXT_INACTIVE);
		cairo_move_to(cr, TITLE_PADDING, (TITLEBAR_HEIGHT - lh) / 2.0);
		pango_cairo_show_layout(cr, layout);
		g_object_unref(layout);
	}

	cairo_destroy(cr);

	if (deco->texture != NULL) {
		wlr_texture_destroy(deco->texture);
	}
	deco->texture = texture_from_cairo(server->renderer, surf);
	cairo_surface_destroy(surf);

	deco->width = width;
	deco->scale = scale;
	deco->active = active;
	deco->maximized = view->maximized;
	deco->hover = view->hover;
	deco->pressed = view->pressed;
}

void deco_finish(struct cv_view *view) {
	if (view->deco.texture != NULL) {
		wlr_texture_destroy(view->deco.texture);
		view->deco.texture = NULL;
	}
	free(view->deco.title);
	view->deco.title = NULL;
}

/* ---- metrics ---- */

int deco_titlebar(struct cv_view *view) {
	return view->ssd && !view->fullscreen ? TITLEBAR_HEIGHT : 0;
}

int deco_border(struct cv_view *view) {
	return view->ssd && !view->fullscreen && !view->maximized ? BORDER_WIDTH : 0;
}

/* ---- hit testing (canvas coordinates vs the frame) ---- */

enum cv_deco_part deco_hit(struct cv_view *view, double cx, double cy,
		double zoom, uint32_t *edges) {
	*edges = 0;
	if (!view->mapped) {
		return CV_DECO_NONE;
	}
	struct wlr_box frame = view_frame_box(view);
	if (wlr_box_empty(&frame)) {
		return CV_DECO_NONE;
	}
	double fx = frame.x, fy = frame.y;
	double fw = frame.width, fh = frame.height;
	int tb = deco_titlebar(view);
	int bw = deco_border(view);
	bool can_resize = !view->maximized && !view->fullscreen;
	double margin = RESIZE_MARGIN / zoom;
	double corner = 16.0 / zoom;

	/* Title bar: buttons right-aligned, resize strip along its top. */
	if (tb > 0 && cx >= fx && cx < fx + fw && cy >= fy && cy < fy + tb) {
		if (can_resize && cy < fy + 4) {
			*edges = WLR_EDGE_TOP;
			if (cx < fx + corner) {
				*edges |= WLR_EDGE_LEFT;
			}
			if (cx >= fx + fw - corner) {
				*edges |= WLR_EDGE_RIGHT;
			}
			return CV_DECO_RESIZE;
		}
		double right = fx + fw;
		if (cx >= right - BUTTON_WIDTH) {
			return CV_DECO_CLOSE;
		}
		if (cx >= right - 2 * BUTTON_WIDTH) {
			return CV_DECO_MAXIMIZE;
		}
		if (cx >= right - 3 * BUTTON_WIDTH) {
			return CV_DECO_MINIMIZE;
		}
		return CV_DECO_TITLE;
	}

	if (can_resize) {
		uint32_t e = 0;
		/* Left and right: the border strip inside the frame plus the
		 * margin outside it. */
		if (cy >= fy - margin && cy <= fy + fh + margin) {
			if (cx >= fx - margin && cx < fx + bw) {
				e |= WLR_EDGE_LEFT;
			}
			if (cx >= fx + fw - bw && cx < fx + fw + margin) {
				e |= WLR_EDGE_RIGHT;
			}
		}
		/* Top: the margin above the frame (plus the border strip below the
		 * title bar); bottom likewise. The title bar itself was handled above. */
		bool top_zone = (cy >= fy - margin && cy < fy)
			|| (tb > 0 ? (cy >= fy + tb && cy < fy + tb + bw)
				: (cy >= fy && cy < fy + bw));
		if (top_zone && cx >= fx - margin && cx <= fx + fw + margin) {
			e |= WLR_EDGE_TOP;
		}
		if (cy >= fy + fh - bw && cy < fy + fh + margin
				&& cx >= fx - margin && cx <= fx + fw + margin) {
			e |= WLR_EDGE_BOTTOM;
		}
		/* Outside corners get a wider diagonal grab area. */
		if (cx < fx && cy < fy && cx > fx - corner && cy > fy - corner) {
			e |= WLR_EDGE_TOP | WLR_EDGE_LEFT;
		}
		if (cx >= fx + fw && cy < fy && cx < fx + fw + corner && cy > fy - corner) {
			e |= WLR_EDGE_TOP | WLR_EDGE_RIGHT;
		}
		if (cx < fx && cy >= fy + fh && cx > fx - corner && cy < fy + fh + corner) {
			e |= WLR_EDGE_BOTTOM | WLR_EDGE_LEFT;
		}
		if (cx >= fx + fw && cy >= fy + fh && cx < fx + fw + corner
				&& cy < fy + fh + corner) {
			e |= WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT;
		}
		if (e != 0) {
			*edges = e;
			return CV_DECO_RESIZE;
		}
	}

	return CV_DECO_NONE;
}

/* ---- mode negotiation ---- */

void deco_refresh_mode(struct cv_view *view) {
	// Clients that negotiate neither protocol (GTK4/libadwaita) draw their
	// own header bar; adding ours would double the title bar.
	bool ssd = view->xdg_decoration != NULL && view->xdg_decoration->current.mode
		== WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
	struct cv_kde_deco *kde;
	wl_list_for_each(kde, &kde_decos, link) {
		if (kde->decoration->surface == view->toplevel->base->surface &&
				kde->decoration->mode == WLR_SERVER_DECORATION_MANAGER_MODE_SERVER) {
			ssd = true;
		}
	}

	if (ssd == view->ssd) {
		return;
	}
	view->ssd = ssd;
	if (view->mapped) {
		view_damage(view);
		view->server->visibility_dirty = true;
	}
}

/* Called from view.c's commit handler on the initial commit, when the
 * toplevel is initialized and the mode can finally be sent. */
void deco_handle_initial_commit(struct cv_view *view) {
	if (view->xdg_decoration != NULL && view->toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(view->xdg_decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

static void handle_deco_request_mode(struct wl_listener *listener, void *data) {
	struct cv_view *view = wl_container_of(listener, view, deco_request_mode);
	struct wlr_xdg_toplevel_decoration_v1 *decoration = view->xdg_decoration;
	if (decoration == NULL) {
		return;
	}
	/* We always decide server-side; the client gets to know once the
	 * toplevel is initialized (before that the initial commit tells it). */
	if (decoration->toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	deco_refresh_mode(view);
}

static void handle_deco_destroy(struct wl_listener *listener, void *data) {
	struct cv_view *view = wl_container_of(listener, view, deco_destroy);
	wl_list_remove(&view->deco_request_mode.link);
	wl_list_remove(&view->deco_destroy.link);
	view->xdg_decoration = NULL;
	if (view->ssd) {
		view->ssd = false;
		if (view->mapped) {
			view_damage(view);
			view->server->visibility_dirty = true;
		}
	}
}

static void handle_new_xdg_decoration(struct wl_listener *listener, void *data) {
	struct cv_server *server =
		wl_container_of(listener, server, new_xdg_decoration);
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
	struct cv_view *view = decoration->toplevel->base->data;
	if (view == NULL) {
		return;
	}
	view->xdg_decoration = decoration;
	view->deco_request_mode.notify = handle_deco_request_mode;
	wl_signal_add(&decoration->events.request_mode, &view->deco_request_mode);
	view->deco_destroy.notify = handle_deco_destroy;
	wl_signal_add(&decoration->events.destroy, &view->deco_destroy);
	if (decoration->toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

static void handle_kde_mode(struct wl_listener *listener, void *data) {
	struct cv_kde_deco *kde = wl_container_of(listener, kde, mode);
	struct cv_view *view = view_from_surface(kde->decoration->surface);
	if (view != NULL) {
		deco_refresh_mode(view);
	}
}

static void handle_kde_destroy(struct wl_listener *listener, void *data) {
	struct cv_kde_deco *kde = wl_container_of(listener, kde, destroy);
	wl_list_remove(&kde->mode.link);
	wl_list_remove(&kde->destroy.link);
	wl_list_remove(&kde->link);
	free(kde);
}

static void handle_new_kde_decoration(struct wl_listener *listener, void *data) {
	struct cv_kde_deco *kde = calloc(1, sizeof(*kde));
	if (kde == NULL) {
		return;
	}
	kde->decoration = data;
	kde->mode.notify = handle_kde_mode;
	wl_signal_add(&kde->decoration->events.mode, &kde->mode);
	kde->destroy.notify = handle_kde_destroy;
	wl_signal_add(&kde->decoration->events.destroy, &kde->destroy);
	wl_list_insert(kde_decos.prev, &kde->link);
	struct cv_view *view = view_from_surface(kde->decoration->surface);
	if (view != NULL) {
		deco_refresh_mode(view);
	}
}

void decoration_init(struct cv_server *server) {
	wl_list_init(&kde_decos);
	server->xdg_decoration_mgr =
		wlr_xdg_decoration_manager_v1_create(server->display);
	server->new_xdg_decoration.notify = handle_new_xdg_decoration;
	wl_signal_add(&server->xdg_decoration_mgr->events.new_toplevel_decoration,
		&server->new_xdg_decoration);

	server->kde_decoration_mgr =
		wlr_server_decoration_manager_create(server->display);
	wlr_server_decoration_manager_set_default_mode(server->kde_decoration_mgr,
		WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	server->new_kde_decoration.notify = handle_new_kde_decoration;
	wl_signal_add(&server->kde_decoration_mgr->events.new_decoration,
		&server->new_kde_decoration);
}

void decoration_finish(struct cv_server *server) {
	wl_list_remove(&server->new_xdg_decoration.link);
	wl_list_remove(&server->new_kde_decoration.link);
	struct cv_kde_deco *kde, *tmp;
	wl_list_for_each_safe(kde, tmp, &kde_decos, link) {
		wl_list_remove(&kde->mode.link);
		wl_list_remove(&kde->destroy.link);
		wl_list_remove(&kde->link);
		free(kde);
	}
}
