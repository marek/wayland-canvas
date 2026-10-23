#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <ranges>
#include <span>

#include "core/box.h"
#include "core/config.h"
#include "render/draw.h"
#include "platform/output.h"
#include "core/server.h"
#include "input/ui_layer.h"

namespace {

// ---------------------------------------------------------------- context
// RenderCtx is in render.h: an output's on-screen frame and an offscreen
// capture of a rectangle of it are drawn through the same one.

void record_surface(const RenderCtx &ctx, wlr_surface *surface, bool frame_done) {
	ctx.drawn->push_back({.surface = surface, .frame_done = frame_done});
}

// ------------------------------------------------------ box/rect drawing

// Output-local logical box -> buffer pixels of the render target, offset by
// the target's origin. Edges are scaled and rounded individually so adjacent
// boxes never leave a seam.
wlr_box logical_box(const RenderCtx &ctx, double x, double y, double w, double h) {
	int x0 = static_cast<int>(std::round((x - ctx.origin_x) * ctx.scale));
	int y0 = static_cast<int>(std::round((y - ctx.origin_y) * ctx.scale));
	int x1 = static_cast<int>(std::round((x + w - ctx.origin_x) * ctx.scale));
	int y1 = static_cast<int>(std::round((y + h - ctx.origin_y) * ctx.scale));
	wlr_box box{x0, y0, x1 - x0, y1 - y0};
	if (!wlr_box_empty(&box)) {
		wlr_box_transform(&box, &box, ctx.transform, ctx.tw, ctx.th);
	}
	return box;
}

double canvas_x(const RenderCtx &ctx, double cx) {
	return (cx - ctx.left) * ctx.cam.zoom;
}

double canvas_y(const RenderCtx &ctx, double cy) {
	return (cy - ctx.top) * ctx.cam.zoom;
}

} // namespace

// Screen-space rect, clipped to the render target; empty rects are skipped.
void draw_logical_rect(const RenderCtx &ctx, double x, double y, double w, double h,
		wlr_render_color color) {
	double x0 = std::fmax(x, ctx.origin_x), y0 = std::fmax(y, ctx.origin_y);
	double x1 = std::fmin(x + w, ctx.origin_x + ctx.vw),
		y1 = std::fmin(y + h, ctx.origin_y + ctx.vh);
	if (x1 <= x0 || y1 <= y0) {
		return;
	}
	wlr_box box = logical_box(ctx, x0, y0, x1 - x0, y1 - y0);
	if (wlr_box_empty(&box)) {
		return;
	}
	const wlr_render_rect_options opts = {
		.box = box,
		.color = color,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	};
	wlr_render_pass_add_rect(ctx.pass, &opts);
}

// Our own (cairo) textures, authored upright: same source transform as a
// client surface with WL_OUTPUT_TRANSFORM_NORMAL. `src_frac` crops the
// source horizontally (0..1) so text can be squeezed without distortion.
void draw_ui_texture(const RenderCtx &ctx, wlr_texture *texture,
		double x, double y, double w, double h, double src_frac) {
	wlr_box dst = logical_box(ctx, x, y, w, h);
	if (wlr_box_empty(&dst)) {
		return;
	}
	wlr_fbox src = {
		.x = 0, .y = 0,
		.width = texture->width * src_frac,
		.height = static_cast<double>(texture->height),
	};
	const wlr_render_texture_options opts = {
		.texture = texture,
		.src_box = src,
		.dst_box = dst,
		.transform = ctx.out_transform,
		.filter_mode = WLR_SCALE_FILTER_BILINEAR,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	};
	wlr_render_pass_add_texture(ctx.pass, &opts);
}

namespace {

void draw_canvas_rect(const RenderCtx &ctx, double x, double y, double w, double h,
		wlr_render_color color) {
	draw_logical_rect(ctx, canvas_x(ctx, x), canvas_y(ctx, y),
		w * ctx.cam.zoom, h * ctx.cam.zoom, color);
}

// Dashed rectangle outline (dash 10, gap 6). The horizontal edges carry the
// corners; the vertical edges run between them so corners aren't drawn
// twice. draw_logical_rect() keeps only the on-screen dashes.
void draw_dashed_logical_rect(const RenderCtx &ctx, double x, double y, double w, double h,
		double thickness, wlr_render_color color) {
	const double step = 10.0 + 6.0;
	for (double p = 0; p < w; p += step) {
		double dash = std::fmin(10.0, w - p);
		if (dash > 0) {
			draw_logical_rect(ctx, x + p, y, dash, thickness, color);
			draw_logical_rect(ctx, x + p, y + h - thickness, dash, thickness, color);
		}
	}
	double inner = h - 2.0 * thickness;
	for (double p = 0; p < inner; p += step) {
		double dash = std::fmin(10.0, inner - p);
		if (dash > 0) {
			draw_logical_rect(ctx, x, y + thickness + p, thickness, dash, color);
			draw_logical_rect(ctx, x + w - thickness, y + thickness + p, thickness, dash, color);
		}
	}
}

void draw_dashed_desktop(const RenderCtx &ctx, const Desktop &desktop, double screen_dx,
		double thickness, wlr_render_color color) {
	double zoom = ctx.cam.zoom;
	const wlr_box &frame = desktop.frame();
	draw_dashed_logical_rect(ctx,
		canvas_x(ctx, frame.x) + screen_dx,
		canvas_y(ctx, frame.y),
		frame.width * zoom, frame.height * zoom,
		thickness, color);
}

// Line approximated by rects: `step`-long dashes along the major axis for long
// connectors, or thickness-sized dots every pixel (step <= thickness) for small
// diagonal glyphs.
void draw_stepped_line(const RenderCtx &ctx, double x0, double y0, double x1, double y1,
		double thickness, double step, wlr_render_color color) {
	double dx = x1 - x0, dy = y1 - y0;
	double dist = std::hypot(dx, dy);
	if (dist < 1.0) {
		return;
	}
	int n = static_cast<int>(std::ceil(dist / step));
	bool dots = step <= thickness;
	double pw = dots || std::fabs(dx) < std::fabs(dy) ? thickness : step;
	double ph = dots || std::fabs(dx) >= std::fabs(dy) ? thickness : step;
	for (int i = 0; i <= n; i++) {
		double t = static_cast<double>(i) / static_cast<double>(n);
		draw_logical_rect(ctx, x0 + dx * t - pw / 2.0, y0 + dy * t - ph / 2.0,
			pw, ph, color);
	}
}

// ---------------------------------------------------- surface drawing

void draw_surface_logical(const RenderCtx &ctx, wlr_surface *surface,
		double lx, double ly, double lw, double lh, float alpha, bool frame_done) {
	wlr_texture *texture = wlr_surface_get_texture(surface);
	if (texture == nullptr) {
		return;
	}
	wlr_box dst = logical_box(ctx, lx, ly, lw, lh);
	if (wlr_box_empty(&dst)) {
		return;
	}
	wlr_fbox src;
	wlr_surface_get_buffer_source_box(surface, &src);
	wl_output_transform transform = wlr_output_transform_compose(
		wlr_output_transform_invert(surface->current.transform),
		ctx.out_transform);
	float a = alpha;
	wlr_render_texture_options opts = {
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
	wlr_render_pass_add_texture(ctx.pass, &opts);
	record_surface(ctx, surface, frame_done);
}

// Layer and drag-icon surfaces, positioned in output-local screen space.
struct ScreenIter {
	const RenderCtx *ctx;
	double ox, oy; // output-local position of the surface tree origin
};

void screen_surface_iter(wlr_surface *surface, int sx, int sy, void *data) {
	ScreenIter &it = *static_cast<ScreenIter *>(data);
	const RenderCtx &ctx = *it.ctx;
	double x = it.ox + sx, y = it.oy + sy;
	double w = surface->current.width, h = surface->current.height;
	if (x >= ctx.origin_x + ctx.vw || y >= ctx.origin_y + ctx.vh
			|| x + w <= ctx.origin_x || y + h <= ctx.origin_y) {
		return;
	}
	draw_surface_logical(*it.ctx, surface, x, y, w, h, 1.0f, true);
}

// ------------------------------------------------------------- layers

// A surface a plugin put on the plane: drawn in canvas space, so it pans,
// zooms and scales with everything else there, by every output whose view
// it is in (docs/intents/platform/ipc.md). Each surface of its tree is drawn
// at the size it committed: a size in flight is never stretched.
struct CanvasIter {
	const RenderCtx *ctx;
	int cx, cy; // canvas position of the surface tree's origin
};

void canvas_surface_iter(wlr_surface *surface, int sx, int sy, void *data) {
	CanvasIter &it = *static_cast<CanvasIter *>(data);
	const RenderCtx &ctx = *it.ctx;
	Rect on_plane{double(it.cx + sx), double(it.cy + sy),
		double(surface->current.width), double(surface->current.height)};
	if (!on_plane.intersects(ctx.viewport)) {
		return;
	}
	draw_surface_logical(ctx, surface, canvas_x(ctx, on_plane.x),
		canvas_y(ctx, on_plane.y), on_plane.w * ctx.cam.zoom,
		on_plane.h * ctx.cam.zoom, 1.0f, true);
}

void draw_canvas_surfaces(const RenderCtx &ctx) {
	for (const LayerSurface *layer : ctx.output->server.canvas_stack) {
		if (!layer->mapped || !layer->canvas_box.has_value()) {
			continue;
		}
		CanvasIter it{.ctx = &ctx, .cx = layer->canvas_box->x,
			.cy = layer->canvas_box->y};
		wlr_surface_for_each_surface(layer->wlr->surface, canvas_surface_iter, &it);
	}
}

void draw_layers(const RenderCtx &ctx, int from, int to) {
	for (int i = from; i <= to; i++) {
		for (const auto &layer : ctx.output->layers[static_cast<std::size_t>(i)]) {
			if (!layer->mapped || layer->canvas_box.has_value()
					|| layer->belongs_on_canvas()) {
				continue; // the plane's, drawn with the canvas or not at all
			}
			ScreenIter it = {
				.ctx = &ctx,
				.ox = static_cast<double>(layer->geo.x),
				.oy = static_cast<double>(layer->geo.y),
			};
			wlr_layer_surface_v1_for_each_surface(layer->wlr, screen_surface_iter, &it);
		}
	}
}

// A fullscreen view hiding (nearly) the whole viewport covers the TOP layer.
bool fullscreen_covers(const RenderCtx &ctx) {
	double area = ctx.viewport.w * ctx.viewport.h;
	if (area <= 0) {
		return false;
	}
	for (const View *view : ctx.output->server.views) {
		if (!view->mapped || view->minimized || view->mode != WindowMode::fullscreen) {
			continue;
		}
		if (view->frame_rect().overlap_area(ctx.viewport) >= 0.95 * area) {
			return true;
		}
	}
	return false;
}

// --------------------------------------------------------------- grid

void draw_grid(const RenderCtx &ctx) {
	double spacing = 64.0;
	while (spacing * ctx.cam.zoom < 48.0) {
		spacing *= 4.0;
	}
	// 1 logical px thick, i.e. 1/zoom canvas units.
	double t = 1.0 / ctx.cam.zoom;
	wlr_render_color color = color_rgba(ctx.output->server.theme.colors().grid);
	double right = ctx.viewport.x + ctx.viewport.w;
	double bottom = ctx.viewport.y + ctx.viewport.h;
	for (double x = std::ceil(ctx.viewport.x / spacing) * spacing; x <= right; x += spacing) {
		draw_canvas_rect(ctx, x, ctx.viewport.y, t, ctx.viewport.h, color);
	}
	for (double y = std::ceil(ctx.viewport.y / spacing) * spacing; y <= bottom; y += spacing) {
		draw_canvas_rect(ctx, ctx.viewport.x, y, ctx.viewport.w, t, color);
	}
}

// -------------------------------------------------------------- views

// One window at the camera of `ctx`. Minimized or not is the caller's to
// decide: the scene skips minimized windows, a capture of one window draws it.
void draw_view(const RenderCtx &ctx, View &view) {
	if (!view.mapped) {
		return;
	}
	Server &server = ctx.output->server;

	// frame_done owner: the output holding the lowest bit of the mask,
	// or (mask 0) whichever output still draws the view.
	uint32_t mask = view.output_mask;
	bool frame_done = mask == 0 || (mask & (0u - mask)) == ctx.output->bit;

	Rect frame = view.frame_rect();
	if (frame.intersects(ctx.viewport)) {
		int tb = view.deco.titlebar();
		int bw = view.deco.border();
		// Layout as hit-tested by deco_hit(): the title bar spans the
		// full frame width from the frame's top edge; the border runs
		// below it and around the content.
		if (tb > 0) {
			wlr_texture *bar = view.deco.title_bar(ctx.deco_scale);
			if (bar != nullptr) {
				draw_ui_texture(ctx, bar, canvas_x(ctx, frame.x), canvas_y(ctx, frame.y),
					frame.w * ctx.cam.zoom, tb * ctx.cam.zoom, 1.0);
			}
		}
		if (bw > 0) {
			const config::Palette &pal = server.theme.colors();
			wlr_render_color border = color_rgba(
				server.focused_view == &view ? pal.border_active : pal.border_inactive);
			draw_canvas_rect(ctx, frame.x, frame.y + tb, frame.w, bw, border);
			draw_canvas_rect(ctx, frame.x, frame.y + frame.h - bw, frame.w, bw, border);
			draw_canvas_rect(ctx, frame.x, frame.y + tb, bw, frame.h - tb - bw, border);
			draw_canvas_rect(ctx, frame.x + frame.w - bw, frame.y + tb,
				bw, frame.h - tb - bw, border);
		}
	}

	Point origin = view.surface_origin();
	double ox = origin.x, oy = origin.y;
	// View surfaces (incl. subsurfaces and popups), positioned via the camera.
	view.for_each_surface([&](wlr_surface *surface, int sx, int sy) {
		double cx = ox + sx, cy = oy + sy;
		double cw = surface->current.width, ch = surface->current.height;
		Rect r{cx, cy, cw, ch};
		if (!r.intersects(ctx.viewport)) {
			return;
		}
		draw_surface_logical(ctx, surface, canvas_x(ctx, cx), canvas_y(ctx, cy),
			cw * ctx.cam.zoom, ch * ctx.cam.zoom, 1.0f, frame_done);
	});
}

// ------------------------------------------------------ desktop frames

void draw_desktop_frames(const RenderCtx &ctx) {
	Output &o = *ctx.output;
	Server &server = o.server;
	const config::Palette &pal = server.theme.colors();

	Desktop *hovered = ctx.overview ? server.input.desktop_under_cursor(o) : nullptr;

	for (const Desktop &d : std::views::reverse(server.canvas.desktops.all())) {
		const wlr_box &frame = d.frame();
		Rect dr = d.rect();
		if (!dr.intersects(ctx.viewport)) {
			continue;
		}

		double offset = server.gestures.abandon_offset(d.id());
		uint32_t color = pal.frame;
		double alpha = 1.0;
		double thickness = 2.0;
		if (offset != 0) {
			// Abandon gesture: slide + fade; red past the threshold.
			alpha = std::fmax(0.0,
				1.0 - std::fabs(offset) / (2.0 * config::abandon_distance));
			if (std::fabs(offset) > config::abandon_distance) {
				color = pal.abandon;
			}
		} else if (server.input.grab.drop_target() == d.id()) {
			draw_canvas_rect(ctx, frame.x, frame.y, frame.width, frame.height,
				color_rgba(pal.combine, 0.15f));
			color = pal.combine;
		} else if (o.overview.target().desktop == d.id()) {
			color = pal.accent;
			thickness = 3.0;
		} else if (hovered == &d) {
			draw_canvas_rect(ctx, frame.x, frame.y, frame.width, frame.height,
				color_rgba(pal.frame, 0.15f));
			color = pal.frame_hover; // stronger than the faint base frames
		}
		draw_dashed_desktop(ctx, d, offset, thickness,
			color_rgba(color, static_cast<float>(alpha)));
	}

	if (server.canvas.desktops.find(o.overview.target().desktop) == nullptr
			&& !wlr_box_empty(&o.overview.target().box)) {
		const wlr_box &t = o.overview.target().box;
		Rect tr = rect_of_box(t);
		if (tr.intersects(ctx.viewport)) {
			double zoom = ctx.cam.zoom;
			draw_dashed_logical_rect(ctx, canvas_x(ctx, t.x),
				canvas_y(ctx, t.y), t.width * zoom,
				t.height * zoom, 2.0,
				color_rgba(pal.accent, 0.7f));
		}
	}
}

void draw_desktops_normal(const RenderCtx &ctx) {
	wlr_render_color color = color_rgba(ctx.output->server.theme.colors().frame,
		0.5f * static_cast<float>(ctx.output->frames_alpha));
	for (const Desktop &d :
			std::views::reverse(ctx.output->server.canvas.desktops.all())) {
		Rect dr = d.rect();
		if (!dr.intersects(ctx.viewport)) {
			continue;
		}
		draw_dashed_desktop(ctx, d, 0, 2.0, color);
	}
}

// Pinch preview: accent fill for tile, green outline + connecting line
// for combine.
void draw_pinch_preview(const RenderCtx &ctx) {
	Output &o = *ctx.output;
	const Gestures::Pinch *g = o.server.gestures.pinch();
	if (g == nullptr) {
		return;
	}
	Desktop *desktop = o.server.canvas.desktops.find(g->desktop);
	Desktop *merge_from = o.server.canvas.desktops.find(g->merge_from);
	const wlr_box &box = desktop != nullptr ? desktop->frame() : o.overview.target().box;
	Rect br = rect_of_box(box);
	bool box_visible = br.intersects(ctx.viewport);

	if (g->scale > config::pinch_tile_scale && box_visible) {
		draw_canvas_rect(ctx, box.x, box.y, box.width, box.height,
			color_rgba(o.server.theme.colors().accent, 0.12f));
	}
	if (merge_from != nullptr) {
		const wlr_box &m = merge_from->frame();
		Rect mr = rect_of_box(m);
		if (mr.intersects(ctx.viewport)) {
			draw_dashed_desktop(ctx, *merge_from, 0, 3.0,
				color_rgba(o.server.theme.colors().combine));
			if (box_visible) {
				double x0 = canvas_x(ctx, m.x + m.width / 2.0);
				double y0 = canvas_y(ctx, m.y + m.height / 2.0);
				double x1 = canvas_x(ctx, box.x + box.width / 2.0);
				double y1 = canvas_y(ctx, box.y + box.height / 2.0);
				draw_stepped_line(ctx, x0, y0, x1, y1, 3.0, 10.0,
					color_rgba(o.server.theme.colors().combine));
			}
		}
	}
}

// ---------------------------------------------------------------- tabs

using LabelBuffer = std::array<char, 48>;

// "Desktop 3", with the percentage of a scaled desktop.
std::string_view desktop_label_text(const Desktop &desktop, LabelBuffer &buf) {
	int len;
	if (desktop.scale() == 1.0) {
		len = std::snprintf(buf.data(), buf.size(), "Desktop %d", desktop.number());
	} else {
		len = std::snprintf(buf.data(), buf.size(), "Desktop %d · %d %%",
			desktop.number(), static_cast<int>(std::lround(desktop.scale() * 100)));
	}
	return {buf.data(), std::min(static_cast<std::size_t>(std::max(len, 0)), buf.size() - 1)};
}

// The tab's label: the text cache's request for its text, in `fg`.
TextSpec desktop_label_spec(std::string_view text, float scale, uint32_t fg) {
	return {.text = text, .font = config::ui_font_bold, .fg = fg, .scale = scale};
}

// The Tile switch's glyph: four squares centred in the button.
void draw_tile_glyph(const RenderCtx &ctx, const wlr_box &button, wlr_render_color color) {
	constexpr int square = 7, gap = 2;
	int x0 = button.x + (button.width - (2 * square + gap)) / 2;
	int y0 = button.y + (button.height - (2 * square + gap)) / 2;
	for (int row = 0; row < 2; row++) {
		for (int col = 0; col < 2; col++) {
			draw_logical_rect(ctx, x0 + col * (square + gap), y0 + row * (square + gap),
				square, square, color);
		}
	}
}

// The Abandon button's glyph: a cross centred in the button.
void draw_abandon_glyph(const RenderCtx &ctx, const wlr_box &button, wlr_render_color color) {
	constexpr int size = 10;
	int x0 = button.x + (button.width - size) / 2;
	int y0 = button.y + (button.height - size) / 2;
	draw_stepped_line(ctx, x0, y0, x0 + size, y0 + size, 2.0, 1.0, color);
	draw_stepped_line(ctx, x0, y0 + size, x0 + size, y0, 2.0, 1.0, color);
}

void draw_tabs(const RenderCtx &ctx) {
	Output &o = *ctx.output;
	Server &server = o.server;
	for (const Desktop &d : server.canvas.desktops.all()) {
		// The target's tab is the accent fill, so its label is drawn in
		// the accent-text variant: a contrasting colour in both modes.
		bool target = o.overview.target().desktop == d.id();
		CachedText label = desktop_label(server, d, ctx.scale, target);
		std::optional<TabLayout> layout = server.painter.overview_tab_layout(o, d);
		if (!layout) {
			continue;
		}
		uint32_t bg = target ? server.theme.colors().accent : server.theme.colors().panel;
		draw_logical_rect(ctx, layout->tab.x, layout->tab.y, layout->tab.width,
			layout->tab.height, color_rgba(bg));
		if (label.texture != nullptr && label.w > 0 && label.h > 0) {
			draw_ui_texture(ctx, label.texture, layout->tab.x + config::tab_pad,
				layout->tab.y + (layout->tab.height - label.h) / 2,
				label.w, label.h, 1.0);
		}
		// The tab's glyphs follow the same rule as the label: on the target
		// tab they sit on the accent fill, so they take the accent-text
		// colour — a contrast in both modes — including the lit Tile
		// switch's wash and glyph. Non-target tabs keep the panel-text
		// glyphs and the accent-lit switch.
		uint32_t ink = target ? server.theme.colors().accent_text
			: server.theme.colors().panel_text;
		uint32_t lit = target ? server.theme.colors().accent_text
			: server.theme.colors().accent;
		wlr_render_color fg = color_rgba(ink);
		if (d.tiling()) {
			// The Tile switch, lit while tiling is on.
			draw_logical_rect(ctx, layout->tile_button.x, layout->tile_button.y,
				layout->tile_button.width, layout->tile_button.height,
				color_rgba(lit, 0.25f));
			fg = color_rgba(lit);
		}
		draw_tile_glyph(ctx, layout->tile_button, fg);
		draw_abandon_glyph(ctx, layout->abandon_button, color_rgba(ink));
	}
}

// --------------------------------------------------------------- shelf

// The y the overview's bottom UI — the help line, the hint's row, the shelf —
// stays above: the output's bottom edge, or the top of the highest shell
// strip (the toolbar, the app bar) anchored to it.
int strips_top(const Output &o) {
	int bottom = o.height;
	for (const auto &vec : o.layers) {
		for (const std::unique_ptr<LayerSurface> &layer : vec) {
			if (layer->mapped && layer->is_overview_strip()
					&& (layer->wlr->current.anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) != 0) {
				bottom = std::min(bottom, layer->geo.y);
			}
		}
	}
	return bottom;
}

// Shelf thumbnails: the view's frame fitted into the box, aspect kept.
void draw_shelf(const RenderCtx &ctx) {
	Shelf shelf = overview_shelf_layout(*ctx.output);
	if (shelf.empty()) {
		return;
	}
	int left = shelf.front().box.x, right = shelf.back().box.x + shelf.back().box.width;
	int top = shelf.front().box.y, bottom = shelf.front().box.y + shelf.front().box.height;
	draw_logical_rect(ctx, left - config::shelf_backing, top - config::shelf_backing,
		right - left + 2 * config::shelf_backing, bottom - top + 2 * config::shelf_backing,
		color_rgba(ctx.output->server.theme.colors().panel));

	for (std::size_t i = 0; i < shelf.size(); i++) {
		View *view = shelf[i].view;
		const wlr_box &box = shelf[i].box;
		wlr_box frame = view->frame_box();
		double fit = 0.0;
		if (frame.width > 0 && frame.height > 0) {
			fit = std::fmin(box.width / static_cast<double>(frame.width),
				(box.height - config::shelf_title_strip) / frame.height);
		}
		Point origin = view->surface_origin();
		double ox = origin.x, oy = origin.y;
		view->for_each_surface([&](wlr_surface *surface, int sx, int sy) {
			double cx = ox + sx, cy = oy + sy;
			double cw = surface->current.width, ch = surface->current.height;
			// Popups sticking out of the frame cannot be cropped (the source box
			// would have to move too): leave them out of the thumbnail.
			if (cx < frame.x || cy < frame.y
					|| cx + cw > frame.x + frame.width
					|| cy + ch > frame.y + frame.height) {
				return;
			}
			double thumbs_h = box.height - config::shelf_title_strip;
			double x = box.x + (box.width - frame.width * fit) / 2.0
				+ (cx - frame.x) * fit;
			double y = box.y + (thumbs_h - frame.height * fit) / 2.0
				+ (cy - frame.y) * fit;
			draw_surface_logical(ctx, surface, x, y, cw * fit, ch * fit, 1.0f, true);
		});

		const char *title = view->shell_title();
		if (title != nullptr && title[0] != '\0') {
			Server &server = ctx.output->server;
			CachedText text = server.text_cache.get({.text = title, .font = config::ui_font,
				.fg = server.theme.colors().panel_text, .scale = ctx.scale});
			if (text.texture != nullptr && text.w > 0 && text.h > 0) {
				double w = std::fmin(text.w, static_cast<double>(box.width));
				draw_ui_texture(ctx, text.texture, box.x + (box.width - w) / 2.0,
					box.y + box.height - text.h, w, text.h, w / text.w);
			}
		}
	}
}

// ----------------------------------------------------- help line/hint

void draw_help_line(const RenderCtx &ctx, const OverviewBottom &bottom) {
	Server &server = ctx.output->server;
	CachedText help = server.text_cache.get(panel_text_spec(
		overview_help_text(server.gestures.options()), ctx.scale, server.theme.colors()));
	if (help.texture == nullptr || wlr_box_empty(&bottom.help)) {
		return;
	}
	draw_ui_texture(ctx, help.texture, bottom.help.x, bottom.help.y,
		bottom.help.width, bottom.help.height, 1.0);
}

// The hint sits in the overview's hint row, or above the bottom edge
// without the overview.
void draw_hint(const RenderCtx &ctx, const std::optional<OverviewBottom> &bottom) {
	Output &o = *ctx.output;
	const std::string &text = o.server.hint.text;
	if (text.empty()) {
		return;
	}
	CachedText hint = o.server.text_cache.get(
		panel_text_spec(text, ctx.scale, o.server.theme.colors()));
	if (hint.texture == nullptr || hint.w <= 0 || hint.h <= 0) {
		return;
	}
	int y = bottom.has_value()
		? bottom->hint.y + bottom->hint.height - hint.h
		: o.height - 48 - hint.h;
	draw_ui_texture(ctx, hint.texture,
		(o.width - hint.w) / 2.0, y, hint.w, hint.h, 1.0);
}

// ------------------------------------------------------------ drag icon

void draw_drag_icon(const RenderCtx &ctx) {
	Server &server = ctx.output->server;
	if (server.input.drag_icon == nullptr || server.input.drag_icon->surface == nullptr) {
		return;
	}
	ScreenIter it = {
		.ctx = &ctx,
		.ox = server.cursor->x - ctx.output->lx + server.input.drag_icon_dx,
		.oy = server.cursor->y - ctx.output->ly + server.input.drag_icon_dy,
	};
	wlr_surface_for_each_surface(server.input.drag_icon->surface,
		screen_surface_iter, &it);
}

// ------------------------------------------------- frame done/enter/present

void send_surface_feedback(const RenderCtx &ctx) {
	timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_output *wlr_output = ctx.output->wlr;
	for (const DrawnSurface &drawn : *ctx.drawn) {
		wlr_surface *surface = drawn.surface;
		bool entered = false;
		wlr_surface_output *so = nullptr;
		wl_list_for_each(so, &surface->current_outputs, link) {
			if (so->output == wlr_output) {
				entered = true;
				break;
			}
		}
		if (!entered) {
			wlr_surface_send_enter(surface, wlr_output);
			// Like view_update_outputs: the sharpest output the surface is on wins.
			double scale = 0;
			wl_list_for_each(so, &surface->current_outputs, link) {
				scale = Output::sharper_scale(scale, *so->output);
			}
			Output::tell_scale(surface, scale);
		}
		wlr_presentation_surface_textured_on_output(surface, wlr_output);
		if (drawn.frame_done) {
			wlr_surface_send_frame_done(surface, &now);
		}
	}
	ctx.drawn->clear();
}

} // namespace

// The overview's help line, one variant per swipe preset: the preset's
// gestures and the pointer and key hints, or only those under `off`.
const char *overview_help_text(const GestureOptions &options) {
	switch (options.swipes) {
	case settings::SwipePreset::three_finger_hops:
		return "3 fingers: jump to desktop  ·  4 down: go there  ·  "
			"4 sideways: abandon  ·  3-finger pinch out / in: tile / combine  ·  "
			"drag frames over windows  ·  right-click: new desktop  ·  Esc: back";
	case settings::SwipePreset::four_finger_hops:
		return "4 fingers: jump to desktop  ·  3 down: go there  ·  "
			"3 sideways: abandon  ·  4-finger pinch out / in: tile / combine  ·  "
			"drag frames over windows  ·  right-click: new desktop  ·  Esc: back";
	case settings::SwipePreset::off:
		return "drag frames over windows  ·  right-click: new desktop  ·  Esc: back";
	}
	return "";
}

// ------------------------------------------------------------- public

CachedText desktop_label(Server &server, const Desktop &desktop, float scale, bool on_accent) {
	LabelBuffer buf;
	const config::Palette &pal = server.theme.colors();
	return server.text_cache.get(desktop_label_spec(desktop_label_text(desktop, buf), scale,
		on_accent ? pal.accent_text : pal.panel_text));
}

std::optional<TabLayout> Painter::overview_tab_layout(const Output &o,
		const Desktop &desktop) const {
	Camera cam = o.camera.drawn();
	double left = cam.cx - o.width / (2.0 * cam.zoom);
	double top = cam.cy - o.height / (2.0 * cam.zoom);
	double offset = server_.gestures.abandon_offset(desktop.id());
	double frame_left = (desktop.frame().x - left) * cam.zoom + offset;
	double frame_top = (desktop.frame().y - top) * cam.zoom;

	// The text's width once the first frame has drawn it, a guess of nine
	// pixels a character before.
	LabelBuffer buf;
	std::string_view text = desktop_label_text(desktop, buf);
	std::optional<CachedText> label = server_.text_cache.find_size(
		desktop_label_spec(text, o.wlr->scale, 0));
	int text_w = label.has_value() ? label->w : config::tab_label_guess * static_cast<int>(text.size());

	TabLayout layout;
	layout.tab.x = static_cast<int>(std::round(frame_left));
	int frame_y = static_cast<int>(std::round(frame_top));
	layout.tab.y = frame_y - config::tab_height - config::tab_lift;
	if (layout.tab.y < config::tab_lift) {
		layout.tab.y = frame_y + config::tab_lift;
	}
	layout.tab.width = config::tab_pad + text_w + config::tab_gap
		+ 2 * config::tab_button + config::tab_button_gap;
	layout.tab.height = config::tab_height;

	layout.tile_button = {layout.tab.x + config::tab_pad + text_w + config::tab_gap,
		layout.tab.y + config::tab_button_inset, config::tab_button, config::tab_button};
	layout.abandon_button = {layout.tile_button.x + config::tab_button + config::tab_button_gap,
		layout.tile_button.y, config::tab_button, config::tab_button};

	if (layout.tab.x < o.width && layout.tab.x + layout.tab.width > 0
			&& layout.tab.y < o.height && layout.tab.y + layout.tab.height > 0) {
		return layout;
	}
	return std::nullopt;
}

OverviewBottom overview_bottom_layout(const Output &o, float scale) {
	Server &server = o.server;
	CachedText help = server.text_cache.get(panel_text_spec(
		overview_help_text(server.gestures.options()), scale, server.theme.colors()));
	bool has_help = help.texture != nullptr && help.w > 0 && help.h > 0;
	OverviewBottom stack;
	int help_top = strips_top(o) - config::overview_help_margin;
	if (has_help) {
		// Narrow outputs: shrink the line rather than clip it.
		double fit = std::fmin(1.0, (o.width - 2.0 * config::overview_help_margin) / help.w);
		int w = static_cast<int>(std::ceil(help.w * fit));
		int h = static_cast<int>(std::ceil(help.h * fit));
		help_top -= h;
		stack.help = {(o.width - w) / 2, help_top, w, h};
	}
	// A hint's texture is as tall as the help line's: one font, one padding.
	int line_h = has_help ? help.h : config::overview_help_guess_h;
	stack.hint = {0, help_top - config::overview_hint_gap - line_h, o.width, line_h};
	stack.shelf_bottom = stack.hint.y - config::overview_shelf_gap;
	return stack;
}

Shelf overview_shelf_layout(const Output &o) {
	Shelf shelf;
	for (View *view : o.server.views) {
		if (!view->mapped || !view->minimized || shelf.size() == shelf_max) {
			continue;
		}
		// Oldest minimized first: minimize_seq ascending.
		shelf.push_back({});
		std::size_t i = shelf.size() - 1;
		while (i > 0 && shelf[i - 1].view->minimize_seq > view->minimize_seq) {
			shelf[i] = shelf[i - 1];
			i--;
		}
		shelf[i] = {.view = view, .box = {}};
	}
	int count = static_cast<int>(shelf.size());
	int total = count * config::shelf_thumb_w + (count - 1) * config::shelf_gap;
	int x = (o.width - total) / 2;
	int bottom = overview_bottom_layout(o, o.wlr->scale).shelf_bottom;
	for (int i = 0; i < count; i++) {
		shelf[static_cast<std::size_t>(i)].box = {
			.x = x + i * (config::shelf_thumb_w + config::shelf_gap),
			.y = bottom - config::shelf_thumb_h,
			.width = config::shelf_thumb_w,
			.height = config::shelf_thumb_h};
	}
	return shelf;
}

Hint::Hint(Server &server) : server_(server) {}

Hint::~Hint() = default;

void Hint::set(std::string_view text) {
	if (text.empty()) {
		clear();
		server_.damage_all();
		return;
	}
	if (text == this->text) {
		keep();
		return;
	}
	this->text.assign(text);
	restart_timer();
	server_.damage_all();
}

void Hint::keep() {
	if (!text.empty()) {
		restart_timer();
	}
}

void Hint::restart_timer() {
	if (!timer_) {
		timer_.reset(wl_event_loop_add_timer(server_.loop,
			[](void *data) -> int {
				auto *hint = static_cast<Hint *>(data);
				// set({}) may have cleared it already; the timer just no-ops.
				if (!hint->text.empty()) {
					hint->clear();
					hint->server_.damage_all();
				}
				return 0;
			}, this));
	}
	if (timer_) {
		wl_event_source_timer_update(timer_.get(), 2000);
	}
}

void Hint::clear() {
	text.clear();
}

void Hint::finish() {
	timer_.reset();
	text.clear();
}

Painter::Painter(Server &server) : server_(server) {}

Painter::~Painter() = default;

RenderCtx Painter::make_ctx(Output &output, wlr_render_pass *pass) {
	RenderCtx ctx{};
	ctx.output = &output;
	ctx.pass = pass;
	ctx.cam = output.camera.drawn();
	ctx.viewport = ctx.cam.viewport(output.width, output.height);
	ctx.left = ctx.cam.cx - output.width / (2.0 * ctx.cam.zoom);
	ctx.top = ctx.cam.cy - output.height / (2.0 * ctx.cam.zoom);
	ctx.scale = output.wlr->scale;
	ctx.deco_scale = output.wlr->scale * output.resting_zoom();
	ctx.transform = wlr_output_transform_invert(output.wlr->transform);
	ctx.out_transform = output.wlr->transform;
	wlr_output_transformed_resolution(output.wlr, &ctx.tw, &ctx.th);
	ctx.vw = output.width;
	ctx.vh = output.height;
	ctx.overview = output.overview.active();
	ctx.drawn = &drawn_;
	return ctx;
}

// While the session is locked nothing behind the lock is drawn — on screen
// or in a capture, which takes the same path: the locker's surface for this
// output, or an opaque fill where it has none yet.
void Painter::draw_lock(const RenderCtx &ctx) {
	Output &output = *ctx.output;
	ctx.drawn->clear();
	draw_logical_rect(ctx, 0, 0, output.width, output.height,
		color_rgba(config::lock_fill));
	wlr_surface *surface = server_.lock.surface_for(output);
	if (surface == nullptr) {
		return;
	}
	ScreenIter it = {.ctx = &ctx, .ox = 0, .oy = 0};
	wlr_surface_for_each_surface(surface, screen_surface_iter, &it);
}

void Painter::draw_scene(const RenderCtx &ctx) {
	Output &output = *ctx.output;
	if (server_.lock.locked()) {
		draw_lock(ctx);
		return;
	}
	ctx.drawn->clear(); // no-op for a capture's scratch buffer

	// clear
	draw_logical_rect(ctx, 0, 0, output.width, output.height,
		color_rgba(server_.theme.colors().canvas));

	// background + bottom layers, screen space
	draw_layers(ctx, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
		ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM);

	// A camera resting at its zoom (a scaled desktop, a free view at any
	// zoom) is a normal view; a zoom on its way to another zoom, or the
	// 4-finger zoom out before it commits, shows the free-canvas chrome.
	bool zoomed = ctx.cam.zoom != ctx.output->resting_zoom()
		|| server_.gestures.opening_overview(output);
	bool wallpaper = output.has_wallpaper();
	if (zoomed && wallpaper) {
		draw_logical_rect(ctx, 0, 0, output.width, output.height,
			color_rgba(server_.theme.colors().dim));
	}
	if (ctx.overview || zoomed || !wallpaper) {
		draw_grid(ctx);
	}

	// views bottom -> top
	for (View *view : std::views::reverse(server_.views)) {
		if (!view->minimized) {
			draw_view(ctx, *view);
		}
	}

	// desktop frames
	if (ctx.overview || zoomed) {
		draw_desktop_frames(ctx);
	} else if (output.frames_alpha > 0) {
		draw_desktops_normal(ctx);
	}

	// and the surfaces plugins put on the plane, above both
	draw_canvas_surfaces(ctx);

	// top layer, unless a fullscreen view covers the output
	if (!fullscreen_covers(ctx)) {
		draw_layers(ctx, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
			ZWLR_LAYER_SHELL_V1_LAYER_TOP);
	}

	// The compositor's own UI: overview chrome, the hint, client overlay
	// surfaces, then the layers over everything, the lowest first.
	std::optional<OverviewBottom> bottom;
	if (ctx.overview) {
		bottom = overview_bottom_layout(*ctx.output, ctx.scale);
		draw_pinch_preview(ctx);
		draw_tabs(ctx);
		draw_shelf(ctx);
		draw_help_line(ctx, *bottom);
	}
	draw_hint(ctx, bottom);
	draw_layers(ctx, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
	for (UiLayer *layer : std::views::reverse(server_.input.layers())) {
		layer->draw(ctx);
	}
	draw_drag_icon(ctx);

}

void Painter::render(Output &output) {
	wlr_output *wlr_output = output.wlr;

	wlr_output_state state;
	wlr_output_state_init(&state);
	// The night light rides the output's own gamma ramp, so a capture stays
	// in true colours; an output with no ramp shows none.
	server_.night_light.apply_to_output(output, state);
	wlr_render_pass *pass = wlr_output_begin_render_pass(wlr_output, &state, nullptr);
	if (pass == nullptr) {
		wlr_output_state_finish(&state);
		return;
	}

	RenderCtx ctx = make_ctx(output, pass);
	drawn_.clear();
	draw_scene(ctx);

	wlr_output_add_software_cursors_to_render_pass(wlr_output, pass, nullptr);
	wlr_render_pass_submit(pass);
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	server_.text_cache.trim();

	send_surface_feedback(ctx);
}

bool Painter::render_area(Output &output, wlr_box local_area, wlr_buffer *target) {
	if (target == nullptr) {
		return false;
	}
	// Clip the area to the output; an empty one is nothing to draw.
	wlr_box bounds{0, 0, output.width, output.height};
	wlr_box clipped{};
	wlr_box_intersection(&clipped, &local_area, &bounds);
	if (wlr_box_empty(&clipped)) {
		return false;
	}
	wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(server_.renderer, target, nullptr);
	if (pass == nullptr) {
		wlr_log(WLR_ERROR, "capture: failed to begin the offscreen render pass");
		return false;
	}
	RenderCtx ctx = make_capture_ctx(output, pass, target);
	// The capture target is the clipped area at the output's scale.
	ctx.origin_x = clipped.x;
	ctx.origin_y = clipped.y;
	ctx.vw = clipped.width;
	ctx.vh = clipped.height;
	draw_scene(ctx);
	return wlr_render_pass_submit(pass);
}

bool Painter::render_view(View &view, float scale, wlr_buffer *target) {
	// Nothing behind the lock is captured.
	if (target == nullptr || !view.mapped || server_.lock.locked()
			|| server_.outputs.empty()) {
		return false;
	}
	wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(server_.renderer, target, nullptr);
	if (pass == nullptr) {
		wlr_log(WLR_ERROR, "capture: failed to begin the window's render pass");
		return false;
	}
	RenderCtx ctx = make_capture_ctx(*server_.outputs.front(), pass, target);
	// The frame's corner is the target's origin, at zoom 1.
	wlr_box frame = view.frame_box();
	ctx.cam = camera_for_box(frame);
	ctx.viewport = ctx.cam.viewport(frame.width, frame.height);
	ctx.left = frame.x;
	ctx.top = frame.y;
	ctx.scale = scale;
	ctx.deco_scale = scale;
	ctx.vw = frame.width;
	ctx.vh = frame.height;
	// Window sources reuse their buffer frame after frame, and the renderer
	// keeps a target's old contents: start from transparent.
	const wlr_render_rect_options clear = {
		.box = {0, 0, target->width, target->height},
		.color = {0, 0, 0, 0},
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	};
	wlr_render_pass_add_rect(pass, &clear);
	capture_drawn_.clear();
	draw_view(ctx, view);
	return wlr_render_pass_submit(pass);
}

// The capture target is untransformed — an upright image whatever the
// output's own transform. Cursors stay out: an on-screen frame owns them, and
// paint-cursor capture sessions get them through that frame. A capture never
// shows the capture UI that asked for it and sends no surface feedback.
RenderCtx Painter::make_capture_ctx(Output &output, wlr_render_pass *pass,
		wlr_buffer *target) {
	RenderCtx ctx = make_ctx(output, pass);
	ctx.transform = WL_OUTPUT_TRANSFORM_NORMAL;
	ctx.out_transform = WL_OUTPUT_TRANSFORM_NORMAL;
	ctx.tw = target->width;
	ctx.th = target->height;
	ctx.drawn = &capture_drawn_;
	ctx.offscreen = true;
	return ctx;
}
