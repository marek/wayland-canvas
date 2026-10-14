#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/util/transform.h>
#include "config.h"
#include "server.h"

/* ---------------------------------------------------------------- context */

struct cv_render_ctx {
	struct cv_output *output;
	struct wlr_render_pass *pass;
	struct camera cam; /* output_camera(): quantized, same as hit testing */
	struct rect viewport; /* visible canvas rectangle */
	double left, top; /* canvas coords shown at output-local (0, 0) */
	float scale;
	enum wl_output_transform transform; /* inverse of output->transform */
	int tw, th; /* transformed (buffer) resolution */
	bool overview;
};

/* Surfaces drawn this frame: they need enter/presentation feedback, and
 * frame_done when this output owns them. */
struct cv_drawn_surface {
	struct wlr_surface *surface;
	bool frame_done;
};

static struct cv_drawn_surface *drawn_surfaces;
static size_t drawn_len, drawn_cap;

static void record_surface(struct wlr_surface *surface, bool frame_done) {
	if (drawn_len == drawn_cap) {
		size_t cap = drawn_cap > 0 ? drawn_cap * 2 : 64;
		struct cv_drawn_surface *grown = realloc(drawn_surfaces, cap * sizeof(*grown));
		if (grown == NULL) {
			return;
		}
		drawn_surfaces = grown;
		drawn_cap = cap;
	}
	drawn_surfaces[drawn_len].surface = surface;
	drawn_surfaces[drawn_len].frame_done = frame_done;
	drawn_len++;
}

/* ------------------------------------------------------ box/rect drawing */

/* Output-local logical box -> buffer pixels. Edges are scaled and rounded
 * individually so adjacent boxes never leave a seam. */
static struct wlr_box logical_box(const struct cv_render_ctx *ctx,
		double x, double y, double w, double h) {
	int x0 = (int)round(x * ctx->scale);
	int y0 = (int)round(y * ctx->scale);
	int x1 = (int)round((x + w) * ctx->scale);
	int y1 = (int)round((y + h) * ctx->scale);
	struct wlr_box box = {x0, y0, x1 - x0, y1 - y0};
	if (!wlr_box_empty(&box)) {
		wlr_box_transform(&box, &box, ctx->transform, ctx->tw, ctx->th);
	}
	return box;
}

static double canvas_x(const struct cv_render_ctx *ctx, double cx) {
	return (cx - ctx->left) * ctx->cam.zoom;
}

static double canvas_y(const struct cv_render_ctx *ctx, double cy) {
	return (cy - ctx->top) * ctx->cam.zoom;
}

/* Screen-space rect, clipped to the output; empty rects are skipped. */
static void draw_logical_rect(const struct cv_render_ctx *ctx,
		double x, double y, double w, double h, struct wlr_render_color color) {
	double x0 = fmax(x, 0.0), y0 = fmax(y, 0.0);
	double x1 = fmin(x + w, ctx->output->width), y1 = fmin(y + h, ctx->output->height);
	if (x1 <= x0 || y1 <= y0) {
		return;
	}
	struct wlr_box box = logical_box(ctx, x0, y0, x1 - x0, y1 - y0);
	if (wlr_box_empty(&box)) {
		return;
	}
	wlr_render_pass_add_rect(ctx->pass, &(struct wlr_render_rect_options){
		.box = box,
		.color = color,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	});
}

static void draw_canvas_rect(const struct cv_render_ctx *ctx,
		double x, double y, double w, double h, struct wlr_render_color color) {
	draw_logical_rect(ctx, canvas_x(ctx, x), canvas_y(ctx, y),
		w * ctx->cam.zoom, h * ctx->cam.zoom, color);
}

/* Our own (cairo) textures, authored upright: same source transform as a
 * client surface with WL_OUTPUT_TRANSFORM_NORMAL. `src_frac` crops the
 * source horizontally (0..1) so text can be squeezed without distortion. */
static void draw_ui_texture(const struct cv_render_ctx *ctx, struct wlr_texture *texture,
		double x, double y, double w, double h, double src_frac) {
	struct wlr_box dst = logical_box(ctx, x, y, w, h);
	if (wlr_box_empty(&dst)) {
		return;
	}
	struct wlr_fbox src = {
		.x = 0, .y = 0,
		.width = texture->width * src_frac,
		.height = texture->height,
	};
	wlr_render_pass_add_texture(ctx->pass, &(struct wlr_render_texture_options){
		.texture = texture,
		.src_box = src,
		.dst_box = dst,
		.transform = ctx->output->wlr_output->transform,
		.filter_mode = WLR_SCALE_FILTER_BILINEAR,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	});
}

/* Dashed rectangle outline (dash 10, gap 6). The horizontal edges carry the
 * corners; the vertical edges run between them so corners aren't drawn
 * twice. draw_logical_rect() keeps only the on-screen dashes. */
static void draw_dashed_logical_rect(const struct cv_render_ctx *ctx,
		double x, double y, double w, double h, double thickness,
		struct wlr_render_color color) {
	const double step = 10.0 + 6.0;
	for (double p = 0; p < w; p += step) {
		double dash = fmin(10.0, w - p);
		if (dash > 0) {
			draw_logical_rect(ctx, x + p, y, dash, thickness, color);
			draw_logical_rect(ctx, x + p, y + h - thickness, dash, thickness, color);
		}
	}
	double inner = h - 2.0 * thickness;
	for (double p = 0; p < inner; p += step) {
		double dash = fmin(10.0, inner - p);
		if (dash > 0) {
			draw_logical_rect(ctx, x, y + thickness + p, thickness, dash, color);
			draw_logical_rect(ctx, x + w - thickness, y + thickness + p, thickness, dash, color);
		}
	}
}

static void draw_dashed_desktop(const struct cv_render_ctx *ctx,
		const struct cv_desktop *desktop, double screen_dx, double thickness,
		struct wlr_render_color color) {
	double zoom = ctx->cam.zoom;
	draw_dashed_logical_rect(ctx,
		canvas_x(ctx, desktop->box.x) + screen_dx,
		canvas_y(ctx, desktop->box.y),
		desktop->box.width * zoom, desktop->box.height * zoom,
		thickness, color);
}

/* Line approximated by rects: `step`-long dashes along the major axis for long
 * connectors, or thickness-sized dots every pixel (step <= thickness) for small
 * diagonal glyphs. */
static void draw_stepped_line(const struct cv_render_ctx *ctx,
		double x0, double y0, double x1, double y1, double thickness, double step,
		struct wlr_render_color color) {
	double dx = x1 - x0, dy = y1 - y0;
	double dist = hypot(dx, dy);
	if (dist < 1.0) {
		return;
	}
	int n = (int)ceil(dist / step);
	bool dots = step <= thickness;
	double pw = dots || fabs(dx) < fabs(dy) ? thickness : step;
	double ph = dots || fabs(dx) >= fabs(dy) ? thickness : step;
	for (int i = 0; i <= n; i++) {
		double t = (double)i / (double)n;
		draw_logical_rect(ctx, x0 + dx * t - pw / 2.0, y0 + dy * t - ph / 2.0,
			pw, ph, color);
	}
}

/* ---------------------------------------------------- surface drawing */

static void draw_surface_logical(const struct cv_render_ctx *ctx,
		struct wlr_surface *surface, double lx, double ly, double lw, double lh,
		float alpha, bool frame_done) {
	struct wlr_texture *texture = wlr_surface_get_texture(surface);
	if (texture == NULL) {
		return;
	}
	struct wlr_box dst = logical_box(ctx, lx, ly, lw, lh);
	if (wlr_box_empty(&dst)) {
		return;
	}
	struct wlr_fbox src;
	wlr_surface_get_buffer_source_box(surface, &src);
	enum wl_output_transform transform = wlr_output_transform_compose(
		wlr_output_transform_invert(surface->current.transform),
		ctx->output->wlr_output->transform);
	float a = alpha;
	struct wlr_render_texture_options opts = {
		.texture = texture,
		.src_box = src,
		.dst_box = dst,
		.transform = transform,
		.filter_mode = WLR_SCALE_FILTER_BILINEAR,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	};
	if (alpha < 1.0f) {
		opts.alpha = &a;
	}
	wlr_render_pass_add_texture(ctx->pass, &opts);
	record_surface(surface, frame_done);
}

/* View surfaces (incl. subsurfaces and popups), positioned via the camera. */
struct cv_view_iter {
	const struct cv_render_ctx *ctx;
	double ox, oy; /* canvas position of the base surface's origin */
	bool frame_done;
};

static void view_surface_iter(struct wlr_surface *surface, int sx, int sy, void *data) {
	struct cv_view_iter *it = data;
	const struct cv_render_ctx *ctx = it->ctx;
	double cx = it->ox + sx, cy = it->oy + sy;
	double cw = surface->current.width, ch = surface->current.height;
	struct rect r = {cx, cy, cw, ch};
	if (!rect_intersects(r, ctx->viewport)) {
		return;
	}
	draw_surface_logical(ctx, surface, canvas_x(ctx, cx), canvas_y(ctx, cy),
		cw * ctx->cam.zoom, ch * ctx->cam.zoom, 1.0f, it->frame_done);
}

/* Shelf thumbnails: the view's frame fitted into the box, aspect kept. */
struct cv_shelf_iter {
	const struct cv_render_ctx *ctx;
	struct wlr_box box; /* thumbnail box, output-local */
	struct wlr_box frame; /* view frame, canvas coords */
	double fit;
	double ox, oy;
};

static void shelf_surface_iter(struct wlr_surface *surface, int sx, int sy, void *data) {
	struct cv_shelf_iter *it = data;
	double cx = it->ox + sx, cy = it->oy + sy;
	double cw = surface->current.width, ch = surface->current.height;
	/* Popups sticking out of the frame cannot be cropped (the source box
	 * would have to move too): leave them out of the thumbnail. */
	if (cx < it->frame.x || cy < it->frame.y
			|| cx + cw > it->frame.x + it->frame.width
			|| cy + ch > it->frame.y + it->frame.height) {
		return;
	}
	double thumbs_h = it->box.height - 16.0;
	double x = it->box.x + (it->box.width - it->frame.width * it->fit) / 2.0
		+ (cx - it->frame.x) * it->fit;
	double y = it->box.y + (thumbs_h - it->frame.height * it->fit) / 2.0
		+ (cy - it->frame.y) * it->fit;
	draw_surface_logical(it->ctx, surface, x, y, cw * it->fit, ch * it->fit,
		1.0f, true);
}

struct cv_screen_iter {
	const struct cv_render_ctx *ctx;
	double ox, oy; /* output-local position of the surface tree origin */
};

static void screen_surface_iter(struct wlr_surface *surface, int sx, int sy, void *data) {
	struct cv_screen_iter *it = data;
	const struct cv_output *o = it->ctx->output;
	double x = it->ox + sx, y = it->oy + sy;
	double w = surface->current.width, h = surface->current.height;
	if (x >= o->width || y >= o->height || x + w <= 0 || y + h <= 0) {
		return;
	}
	draw_surface_logical(it->ctx, surface, x, y, w, h, 1.0f, true);
}

/* ------------------------------------------------------------- layers */

static void draw_layers(const struct cv_render_ctx *ctx, int from, int to) {
	for (int i = from; i <= to; i++) {
		struct cv_layer *layer;
		wl_list_for_each(layer, &ctx->output->layers[i], link) {
			if (!layer->mapped) {
				continue;
			}
			struct cv_screen_iter it = {
				.ctx = ctx,
				.ox = layer->geo.x,
				.oy = layer->geo.y,
			};
			wlr_layer_surface_v1_for_each_surface(layer->layer_surface,
				screen_surface_iter, &it);
		}
	}
}

/* A fullscreen view hiding (nearly) the whole viewport covers the TOP layer. */
static bool fullscreen_covers(const struct cv_render_ctx *ctx) {
	double area = ctx->viewport.w * ctx->viewport.h;
	if (area <= 0) {
		return false;
	}
	struct cv_view *view;
	wl_list_for_each(view, &ctx->output->server->views, link) {
		if (!view->mapped || view->minimized || !view->fullscreen) {
			continue;
		}
		if (rect_overlap_area(view_frame_rect(view), ctx->viewport) >= 0.95 * area) {
			return true;
		}
	}
	return false;
}

/* --------------------------------------------------------------- grid */

static void draw_grid(const struct cv_render_ctx *ctx) {
	double spacing = 64.0;
	while (spacing * ctx->cam.zoom < 48.0) {
		spacing *= 4.0;
	}
	/* 1 logical px thick, i.e. 1/zoom canvas units. */
	double t = 1.0 / ctx->cam.zoom;
	struct wlr_render_color color = color_rgba(COLOR_GRID, 1.0f);
	double right = ctx->viewport.x + ctx->viewport.w;
	double bottom = ctx->viewport.y + ctx->viewport.h;
	for (double x = ceil(ctx->viewport.x / spacing) * spacing; x <= right; x += spacing) {
		draw_canvas_rect(ctx, x, ctx->viewport.y, t, ctx->viewport.h, color);
	}
	for (double y = ceil(ctx->viewport.y / spacing) * spacing; y <= bottom; y += spacing) {
		draw_canvas_rect(ctx, ctx->viewport.x, y, ctx->viewport.w, t, color);
	}
}

/* -------------------------------------------------------------- views */

static void draw_view(const struct cv_render_ctx *ctx, struct cv_view *view) {
	if (!view->mapped || view->minimized) {
		return;
	}
	struct cv_server *server = ctx->output->server;

	/* frame_done owner: the output holding the lowest bit of the mask,
	 * or (mask 0) whichever output still draws the view. */
	uint32_t mask = view->output_mask;
	bool frame_done = mask == 0 || (mask & (0u - mask)) == ctx->output->bit;

	struct rect frame = view_frame_rect(view);
	if (rect_intersects(frame, ctx->viewport)) {
		int tb = deco_titlebar(view);
		int bw = deco_border(view);
		/* Layout as hit-tested by deco_hit(): the title bar spans the
		 * full frame width from the frame's top edge; the border runs
		 * below it and around the content. */
		if (tb > 0) {
			deco_update(view, ctx->output->wlr_output->scale);
			if (view->deco.texture != NULL) {
				draw_ui_texture(ctx, view->deco.texture,
					canvas_x(ctx, frame.x), canvas_y(ctx, frame.y),
					frame.w * ctx->cam.zoom, tb * ctx->cam.zoom, 1.0);
			}
		}
		if (bw > 0) {
			struct wlr_render_color border = color_rgba(
				server->focused_view == view ? COLOR_BORDER_ACTIVE
					: COLOR_BORDER_INACTIVE, 1.0f);
			draw_canvas_rect(ctx, frame.x, frame.y + tb, frame.w, bw, border);
			draw_canvas_rect(ctx, frame.x, frame.y + frame.h - bw, frame.w, bw, border);
			draw_canvas_rect(ctx, frame.x, frame.y + tb, bw, frame.h - tb - bw, border);
			draw_canvas_rect(ctx, frame.x + frame.w - bw, frame.y + tb,
				bw, frame.h - tb - bw, border);
		}
	}

	struct wlr_box geo = view->toplevel->base->geometry;
	if (wlr_box_empty(&geo)) {
		/* No window geometry set yet: the base surface fills it. */
		geo.x = 0;
		geo.y = 0;
		geo.width = view->toplevel->base->surface->current.width;
		geo.height = view->toplevel->base->surface->current.height;
	}
	struct cv_view_iter it = {
		.ctx = ctx,
		.ox = view->x - geo.x,
		.oy = view->y - geo.y,
		.frame_done = frame_done,
	};
	view_for_each_surface(view, view_surface_iter, &it);
}

/* ------------------------------------------------------ desktop frames */

static void draw_desktop_frames(struct cv_render_ctx *ctx) {
	struct cv_output *o = ctx->output;
	struct cv_server *server = o->server;

	struct cv_desktop *hovered = NULL;
	if (ctx->overview) {
		struct cv_output *cursor_output = NULL;
		double cx, cy;
		if (cursor_canvas_pos(server, &cursor_output, &cx, &cy)
				&& cursor_output == o) {
			hovered = desktop_at(server, cx, cy);
		}
	}

	struct cv_desktop *d;
	wl_list_for_each_reverse(d, &server->desktops, link) {
		struct rect dr = {d->box.x, d->box.y, d->box.width, d->box.height};
		if (!rect_intersects(dr, ctx->viewport)) {
			continue;
		}

		uint32_t color = COLOR_FRAME;
		double alpha = 1.0;
		double thickness = 2.0;
		if (d->swipe_offset != 0) {
			/* Abandon gesture: slide + fade; red past the threshold. */
			alpha = fmax(0.0, 1.0 - fabs(d->swipe_offset) / (2.0 * ABANDON_DISTANCE));
			if (fabs(d->swipe_offset) > ABANDON_DISTANCE) {
				color = COLOR_ABANDON;
			}
		} else if (server->drop_desktop == d) {
			draw_canvas_rect(ctx, d->box.x, d->box.y, d->box.width, d->box.height,
				color_rgba(COLOR_COMBINE, 0.15f));
			color = COLOR_COMBINE;
		} else if (o->target_desktop == d) {
			color = COLOR_ACCENT;
			thickness = 3.0;
		} else if (hovered == d) {
			draw_canvas_rect(ctx, d->box.x, d->box.y, d->box.width, d->box.height,
				color_rgba(COLOR_FRAME, 0.15f));
			color = 0xffffffff; /* brighter than the faint base frames */
		}
		draw_dashed_desktop(ctx, d, d->swipe_offset, thickness,
			color_rgba(color, (float)alpha));
	}

	if (o->target_desktop == NULL && !wlr_box_empty(&o->target)) {
		struct rect tr = {o->target.x, o->target.y, o->target.width, o->target.height};
		if (rect_intersects(tr, ctx->viewport)) {
			double zoom = ctx->cam.zoom;
			draw_dashed_logical_rect(ctx, canvas_x(ctx, o->target.x),
				canvas_y(ctx, o->target.y), o->target.width * zoom,
				o->target.height * zoom, 2.0,
				color_rgba(COLOR_ACCENT, 0.7f));
		}
	}
}

static void draw_desktops_normal(const struct cv_render_ctx *ctx) {
	struct wlr_render_color color = color_rgba(COLOR_FRAME,
		0.5f * (float)ctx->output->frames_alpha);
	struct cv_desktop *d;
	wl_list_for_each_reverse(d, &ctx->output->server->desktops, link) {
		struct rect dr = {d->box.x, d->box.y, d->box.width, d->box.height};
		if (!rect_intersects(dr, ctx->viewport)) {
			continue;
		}
		draw_dashed_desktop(ctx, d, 0, 2.0, color);
	}
}

/* Pinch preview: accent fill for tile, green outline + connecting line
 * for combine. */
static void draw_pinch_preview(const struct cv_render_ctx *ctx) {
	struct cv_output *o = ctx->output;
	struct cv_gesture *g = &o->server->gesture;
	if (g->kind != CV_GESTURE_PINCH) {
		return;
	}
	struct wlr_box *box = g->desktop != NULL ? &g->desktop->box : &o->target;
	struct rect br = {box->x, box->y, box->width, box->height};
	bool box_visible = rect_intersects(br, ctx->viewport);

	if (g->scale > PINCH_TILE_SCALE && box_visible) {
		draw_canvas_rect(ctx, box->x, box->y, box->width, box->height,
			color_rgba(COLOR_ACCENT, 0.12f));
	}
	if (g->merge_from != NULL) {
		struct wlr_box *m = &g->merge_from->box;
		struct rect mr = {m->x, m->y, m->width, m->height};
		if (rect_intersects(mr, ctx->viewport)) {
			draw_dashed_desktop(ctx, g->merge_from, 0, 3.0,
				color_rgba(COLOR_COMBINE, 1.0f));
			if (box_visible) {
				double x0 = canvas_x(ctx, m->x + m->width / 2.0);
				double y0 = canvas_y(ctx, m->y + m->height / 2.0);
				double x1 = canvas_x(ctx, box->x + box->width / 2.0);
				double y1 = canvas_y(ctx, box->y + box->height / 2.0);
				draw_stepped_line(ctx, x0, y0, x1, y1, 3.0, 10.0,
					color_rgba(COLOR_COMBINE, 1.0f));
			}
		}
	}
}

/* ---------------------------------------------------------------- tabs */

/* Label texture cached in the desktop; regenerated on id/scale change. */
static struct wlr_texture *desktop_label(struct cv_server *server,
		struct cv_desktop *d, float scale) {
	if (d->label != NULL && d->label_id == d->id && d->label_scale == scale) {
		return d->label;
	}
	if (d->label != NULL) {
		wlr_texture_destroy(d->label);
		d->label = NULL;
	}
	char buf[32];
	int len = snprintf(buf, sizeof(buf), "Desktop %d", d->id);
	int w = 0, h = 0;
	if (len > 0) {
		d->label = draw_text(server->renderer, buf, UI_FONT_BOLD,
			COLOR_PANEL_TEXT, 0x00000000u, scale, 0, 0, &w, &h);
	}
	d->label_id = d->id;
	d->label_scale = scale;
	d->label_w = w;
	d->label_h = h;
	return d->label;
}

bool overview_tab_layout(struct cv_output *o, struct cv_desktop *desktop,
		struct wlr_box *tab, struct wlr_box *tile_button, struct wlr_box *abandon_button) {
	struct camera cam = output_camera(o);
	double left = cam.cx - o->width / (2.0 * cam.zoom);
	double top = cam.cy - o->height / (2.0 * cam.zoom);
	double frame_left = (desktop->box.x - left) * cam.zoom + desktop->swipe_offset;
	double frame_top = (desktop->box.y - top) * cam.zoom;

	char buf[32];
	int len = snprintf(buf, sizeof(buf), "Desktop %d", desktop->id);
	int label_w = desktop->label != NULL
		&& desktop->label_scale == o->wlr_output->scale
		? desktop->label_w : 9 * (len > 0 ? len : 0);

	tab->x = (int)round(frame_left);
	tab->y = (int)round(frame_top) - 28 - 4;
	if (tab->y < 4) {
		tab->y = (int)round(frame_top) + 4;
	}
	tab->width = 12 + label_w + 8 + 2 * 24 + 6;
	tab->height = 28;

	tile_button->x = tab->x + 12 + label_w + 8;
	tile_button->y = tab->y + 2;
	tile_button->width = 24;
	tile_button->height = 24;
	abandon_button->x = tile_button->x + 24 + 6;
	abandon_button->y = tab->y + 2;
	abandon_button->width = 24;
	abandon_button->height = 24;

	return tab->x < o->width && tab->x + tab->width > 0
		&& tab->y < o->height && tab->y + tab->height > 0;
}

static void draw_tile_glyph(const struct cv_render_ctx *ctx,
		const struct wlr_box *button, struct wlr_render_color color) {
	const int off[4][2] = {{4, 4}, {13, 4}, {4, 13}, {13, 13}};
	for (int i = 0; i < 4; i++) {
		draw_logical_rect(ctx, button->x + off[i][0], button->y + off[i][1], 7, 7, color);
	}
}

static void draw_abandon_glyph(const struct cv_render_ctx *ctx,
		const struct wlr_box *button, struct wlr_render_color color) {
	draw_stepped_line(ctx, button->x + 7, button->y + 7,
		button->x + 17, button->y + 17, 2.0, 1.0, color);
	draw_stepped_line(ctx, button->x + 7, button->y + 17,
		button->x + 17, button->y + 7, 2.0, 1.0, color);
}

static void draw_tabs(const struct cv_render_ctx *ctx) {
	struct cv_output *o = ctx->output;
	struct cv_server *server = o->server;
	struct cv_desktop *d;
	wl_list_for_each(d, &server->desktops, link) {
		/* Make sure the cached label matches this output's scale before
		 * the layout measures it. */
		desktop_label(server, d, ctx->scale);
		struct wlr_box tab, tile, abandon;
		if (!overview_tab_layout(o, d, &tab, &tile, &abandon)) {
			continue;
		}
		uint32_t bg = o->target_desktop == d ? COLOR_ACCENT : COLOR_PANEL;
		draw_logical_rect(ctx, tab.x, tab.y, tab.width, tab.height,
			color_rgba(bg, 1.0f));
		if (d->label != NULL && d->label_w > 0 && d->label_h > 0) {
			draw_ui_texture(ctx, d->label, tab.x + 12,
				tab.y + (tab.height - d->label_h) / 2,
				d->label_w, d->label_h, 1.0);
		}
		struct wlr_render_color fg = color_rgba(COLOR_PANEL_TEXT, 1.0f);
		draw_tile_glyph(ctx, &tile, fg);
		draw_abandon_glyph(ctx, &abandon, fg);
	}
}

/* --------------------------------------------------------------- shelf */

size_t overview_shelf_layout(struct cv_output *o, struct cv_view **views,
		struct wlr_box *boxes, size_t max) {
	size_t n = 0;
	if (max == 0) {
		return 0;
	}
	struct cv_view *view;
	wl_list_for_each(view, &o->server->views, link) {
		if (!view->mapped || !view->minimized || n == max) {
			continue;
		}
		/* Oldest minimized first: minimize_seq ascending. */
		size_t i = n;
		while (i > 0 && views[i - 1]->minimize_seq > view->minimize_seq) {
			views[i] = views[i - 1];
			i--;
		}
		views[i] = view;
		n++;
	}
	int bw = 160, bh = 100, gap = 12;
	int bottom = o->height - 12 - 28 - 16;
	int total = (int)n * bw + ((int)n - 1) * gap;
	int x = (o->width - total) / 2;
	for (size_t i = 0; i < n; i++) {
		boxes[i].x = x + (int)i * (bw + gap);
		boxes[i].y = bottom - bh;
		boxes[i].width = bw;
		boxes[i].height = bh;
	}
	return n;
}

#define SHELF_MAX 64

static void draw_shelf(const struct cv_render_ctx *ctx) {
	static struct cv_view *views[SHELF_MAX];
	static struct wlr_box boxes[SHELF_MAX];
	size_t n = overview_shelf_layout(ctx->output, views, boxes, SHELF_MAX);
	if (n == 0) {
		return;
	}
	int left = boxes[0].x, right = boxes[n - 1].x + boxes[n - 1].width;
	int top = boxes[0].y, bottom = boxes[0].y + boxes[0].height;
	draw_logical_rect(ctx, left - 10, top - 10, right - left + 20, bottom - top + 20,
		color_rgba(COLOR_PANEL, 1.0f));

	for (size_t i = 0; i < n; i++) {
		struct cv_view *view = views[i];
		struct wlr_box frame = view_frame_box(view);
		double fit = 0.0;
		if (frame.width > 0 && frame.height > 0) {
			fit = fmin(boxes[i].width / (double)frame.width,
				(boxes[i].height - 16.0) / frame.height);
		}
		struct wlr_box geo = view->toplevel->base->geometry;
		if (wlr_box_empty(&geo)) {
			geo.x = 0;
			geo.y = 0;
			geo.width = view->toplevel->base->surface->current.width;
			geo.height = view->toplevel->base->surface->current.height;
		}
		struct cv_shelf_iter it = {
			.ctx = ctx,
			.box = boxes[i],
			.frame = frame,
			.fit = fit,
			.ox = view->x - geo.x,
			.oy = view->y - geo.y,
		};
		view_for_each_surface(view, shelf_surface_iter, &it);

		const char *title = view->toplevel->title;
		if (title != NULL && title[0] != '\0') {
			int tw = 0, th = 0;
			struct wlr_texture *tex = draw_text(ctx->output->server->renderer,
				title, UI_FONT, COLOR_PANEL_TEXT, 0x00000000u, ctx->scale,
				0, 0, &tw, &th);
			if (tex != NULL && tw > 0 && th > 0) {
				double w = fmin(tw, (double)boxes[i].width);
				draw_ui_texture(ctx, tex,
					boxes[i].x + (boxes[i].width - w) / 2.0,
					boxes[i].y + boxes[i].height - th, w, th, w / tw);
				wlr_texture_destroy(tex);
			}
		}
	}
}

/* ----------------------------------------------------- help line/hint */

static const char HELP_TEXT[] =
	"3 fingers: jump to desktop  ·  4 down: go there  ·  4 sideways: abandon  ·  "
	"3-finger pinch out / in: tile / combine  ·  drag frames over windows  ·  "
	"right-click: new desktop  ·  Esc: back";

#define HELP_SLOTS 4

struct cv_help_cache {
	struct wlr_texture *texture;
	int w, h;
	float scale;
};

static struct cv_help_cache help_cache[HELP_SLOTS];
static unsigned help_clock;

static struct cv_help_cache *help_texture(const struct cv_render_ctx *ctx) {
	for (size_t i = 0; i < HELP_SLOTS; i++) {
		if (help_cache[i].texture != NULL && help_cache[i].scale == ctx->scale) {
			return &help_cache[i];
		}
	}
	struct cv_help_cache *slot = &help_cache[help_clock++ % HELP_SLOTS];
	if (slot->texture != NULL) {
		wlr_texture_destroy(slot->texture);
		slot->texture = NULL;
	}
	slot->texture = draw_text(ctx->output->server->renderer, HELP_TEXT, UI_FONT,
		COLOR_PANEL_TEXT, COLOR_PANEL, ctx->scale, 10, 5, &slot->w, &slot->h);
	slot->scale = ctx->scale;
	return slot;
}

static int draw_help_line(const struct cv_render_ctx *ctx) {
	struct cv_help_cache *help = help_texture(ctx);
	if (help->texture == NULL || help->w <= 0) {
		return 0;
	}
	// Narrow outputs: shrink the line rather than clip it.
	double fit = fmin(1.0, (ctx->output->width - 24.0) / help->w);
	double w = help->w * fit, h = help->h * fit;
	draw_ui_texture(ctx, help->texture, (ctx->output->width - w) / 2.0,
		ctx->output->height - 12 - h, w, h, 1.0);
	return (int)ceil(h);
}

static void draw_hint(const struct cv_render_ctx *ctx, int help_h) {
	struct cv_output *o = ctx->output;
	struct cv_hint *hint = &o->server->hint;
	if (hint->text == NULL) {
		return;
	}
	if (hint->texture == NULL || hint->scale != ctx->scale) {
		if (hint->texture != NULL) {
			wlr_texture_destroy(hint->texture);
			hint->texture = NULL;
		}
		hint->texture = draw_text(o->server->renderer, hint->text, UI_FONT,
			COLOR_PANEL_TEXT, COLOR_PANEL, ctx->scale, 10, 5, &hint->w, &hint->h);
		hint->scale = ctx->scale;
	}
	if (hint->texture == NULL || hint->w <= 0 || hint->h <= 0) {
		return;
	}
	int y = ctx->overview
		? o->height - 12 - help_h - 8 - hint->h
		: o->height - 48 - hint->h;
	draw_ui_texture(ctx, hint->texture,
		(o->width - hint->w) / 2.0, y, hint->w, hint->h, 1.0);
}

/* ------------------------------------------------------------ drag icon */

static void draw_drag_icon(const struct cv_render_ctx *ctx) {
	struct cv_server *server = ctx->output->server;
	if (server->drag_icon == NULL || server->drag_icon->surface == NULL) {
		return;
	}
	struct cv_screen_iter it = {
		.ctx = ctx,
		.ox = server->cursor->x - ctx->output->lx + server->drag_icon_dx,
		.oy = server->cursor->y - ctx->output->ly + server->drag_icon_dy,
	};
	wlr_surface_for_each_surface(server->drag_icon->surface,
		screen_surface_iter, &it);
}

/* ------------------------------------------------- frame done/enter/present */

static void send_surface_feedback(const struct cv_render_ctx *ctx) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	struct wlr_output *wlr_output = ctx->output->wlr_output;
	for (size_t i = 0; i < drawn_len; i++) {
		struct wlr_surface *surface = drawn_surfaces[i].surface;
		bool entered = false;
		struct wlr_surface_output *so;
		wl_list_for_each(so, &surface->current_outputs, link) {
			if (so->output == wlr_output) {
				entered = true;
				break;
			}
		}
		if (!entered) {
			wlr_surface_send_enter(surface, wlr_output);
			// Like view_update_outputs: the sharpest output the surface is on wins.
			double scale = 1;
			wl_list_for_each(so, &surface->current_outputs, link) {
				scale = fmax(scale, so->output->scale);
			}
			wlr_fractional_scale_v1_notify_scale(surface, scale);
			wlr_surface_set_preferred_buffer_scale(surface, (int32_t)ceil(scale));
		}
		wlr_presentation_surface_textured_on_output(surface, wlr_output);
		if (drawn_surfaces[i].frame_done) {
			wlr_surface_send_frame_done(surface, &now);
		}
	}
	drawn_len = 0;
}

/* ------------------------------------------------------------- public */

void render_output(struct cv_output *output) {
	struct cv_server *server = output->server;
	struct wlr_output *wlr_output = output->wlr_output;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	struct wlr_render_pass *pass =
		wlr_output_begin_render_pass(wlr_output, &state, NULL);
	if (pass == NULL) {
		wlr_output_state_finish(&state);
		return;
	}

	struct cv_render_ctx ctx = {0};
	ctx.output = output;
	ctx.pass = pass;
	ctx.cam = output_camera(output);
	ctx.viewport = camera_viewport(ctx.cam, output->width, output->height);
	ctx.left = ctx.cam.cx - output->width / (2.0 * ctx.cam.zoom);
	ctx.top = ctx.cam.cy - output->height / (2.0 * ctx.cam.zoom);
	ctx.scale = wlr_output->scale;
	ctx.transform = wlr_output_transform_invert(wlr_output->transform);
	wlr_output_transformed_resolution(wlr_output, &ctx.tw, &ctx.th);
	ctx.overview = output->mode == CV_MODE_OVERVIEW;
	drawn_len = 0;

	/* clear */
	draw_logical_rect(&ctx, 0, 0, output->width, output->height,
		color_rgba(COLOR_CANVAS, 1.0f));

	/* background + bottom layers, screen space */
	draw_layers(&ctx, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
		ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM);

	bool zoomed = ctx.cam.zoom != 1.0;
	bool wallpaper = output_has_wallpaper(output);
	if (zoomed && wallpaper) {
		draw_logical_rect(&ctx, 0, 0, output->width, output->height,
			color_rgba(COLOR_DIM, 1.0f));
	}
	if (ctx.overview || zoomed || !wallpaper) {
		draw_grid(&ctx);
	}

	/* views bottom -> top */
	struct cv_view *view;
	wl_list_for_each_reverse(view, &server->views, link) {
		draw_view(&ctx, view);
	}

	/* desktop frames */
	if (ctx.overview || zoomed) {
		draw_desktop_frames(&ctx);
	} else if (output->frames_alpha > 0) {
		draw_desktops_normal(&ctx);
	}

	/* top layer, unless a fullscreen view covers the output */
	if (!fullscreen_covers(&ctx)) {
		draw_layers(&ctx, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
			ZWLR_LAYER_SHELL_V1_LAYER_TOP);
	}

	/* overview UI */
	int help_h = 0;
	if (ctx.overview) {
		draw_pinch_preview(&ctx);
		draw_tabs(&ctx);
		draw_shelf(&ctx);
		help_h = draw_help_line(&ctx);
	}
	draw_hint(&ctx, help_h);

	/* overlay layer, launcher, drag icon, cursors */
	draw_layers(&ctx, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
	struct wlr_box launcher_box;
	struct wlr_texture *launcher = launcher_texture(server, output, &launcher_box);
	if (launcher != NULL) {
		draw_logical_rect(&ctx, 0, 0, output->width, output->height,
			color_rgba(COLOR_LAUNCHER_BACKDROP, 1.0f));
		draw_ui_texture(&ctx, launcher, launcher_box.x, launcher_box.y,
			launcher_box.width, launcher_box.height, 1.0);
	}
	draw_drag_icon(&ctx);

	wlr_output_add_software_cursors_to_render_pass(wlr_output, pass, NULL);
	wlr_render_pass_submit(pass);
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	send_surface_feedback(&ctx);
}

void render_finish(struct cv_server *server) {
	for (size_t i = 0; i < HELP_SLOTS; i++) {
		if (help_cache[i].texture != NULL) {
			wlr_texture_destroy(help_cache[i].texture);
			help_cache[i].texture = NULL;
		}
	}
	if (server->hint.timer != NULL) {
		wl_event_source_remove(server->hint.timer);
		server->hint.timer = NULL;
	}
	if (server->hint.texture != NULL) {
		wlr_texture_destroy(server->hint.texture);
		server->hint.texture = NULL;
	}
	free(server->hint.text);
	server->hint.text = NULL;
	server->hint.w = 0;
	server->hint.h = 0;
	free(drawn_surfaces);
	drawn_surfaces = NULL;
	drawn_len = 0;
	drawn_cap = 0;
}

static void hint_clear(struct cv_server *server) {
	free(server->hint.text);
	server->hint.text = NULL;
	if (server->hint.texture != NULL) {
		wlr_texture_destroy(server->hint.texture);
		server->hint.texture = NULL;
	}
	server->hint.w = 0;
	server->hint.h = 0;
	server->hint.scale = 0;
}

static int hint_expired(void *data) {
	struct cv_server *server = data;
	/* hint_set(NULL) may have cleared it already; the timer just no-ops. */
	if (server->hint.text != NULL) {
		hint_clear(server);
		server_damage_all(server);
	}
	return 0;
}

void hint_set(struct cv_server *server, const char *text) {
	if (text == NULL) {
		hint_clear(server);
		server_damage_all(server);
		return;
	}
	free(server->hint.text);
	server->hint.text = strdup(text);
	if (server->hint.texture != NULL) {
		wlr_texture_destroy(server->hint.texture);
		server->hint.texture = NULL;
	}
	server->hint.scale = 0; /* regenerate at the output's scale */
	if (server->hint.timer == NULL) {
		server->hint.timer = wl_event_loop_add_timer(server->loop,
			hint_expired, server);
	}
	if (server->hint.timer != NULL) {
		wl_event_source_timer_update(server->hint.timer, 2000);
	}
	server_damage_all(server);
}
