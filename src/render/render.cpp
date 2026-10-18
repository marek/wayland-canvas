#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <ranges>
#include <span>

#include "core/config.h"
#include "render/draw.h"
#include "platform/output.h"
#include "core/server.h"

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

// wlr_box (ints) as a canvas Rect.
Rect box_rect(const wlr_box &box) {
	Rect r;
	r.x = box.x;
	r.y = box.y;
	r.w = box.width;
	r.h = box.height;
	return r;
}

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

void draw_canvas_rect(const RenderCtx &ctx, double x, double y, double w, double h,
		wlr_render_color color) {
	draw_logical_rect(ctx, canvas_x(ctx, x), canvas_y(ctx, y),
		w * ctx.cam.zoom, h * ctx.cam.zoom, color);
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

// One hollow rectangle of the capture overlay: four thin fills. The
// horizontal edges carry the corners.
void draw_capture_outline(const RenderCtx &ctx, const CaptureOutline &outline) {
	wlr_render_color color = color_rgba(outline.rgba);
	double t = outline.thickness;
	const wlr_box &b = outline.box;
	draw_logical_rect(ctx, b.x, b.y, b.width, t, color);
	draw_logical_rect(ctx, b.x, b.y + b.height - t, b.width, t, color);
	draw_logical_rect(ctx, b.x, b.y + t, t, b.height - 2 * t, color);
	draw_logical_rect(ctx, b.x + b.width - t, b.y + t, t, b.height - 2 * t, color);
}

// The capture UI's own overlay: the picker, the toolbar, the thumbnail.
void draw_capture_overlay(const RenderCtx &ctx) {
	const CaptureOverlay &capture =
		ctx.output->server.capture_ui.overlay(*ctx.output, ctx.scale);
	for (const CaptureFill &fill : capture.fills) {
		draw_logical_rect(ctx, fill.box.x, fill.box.y, fill.box.width,
			fill.box.height, color_rgba(fill.rgba));
	}
	for (const CaptureOutline &outline : capture.outlines) {
		draw_capture_outline(ctx, outline);
	}
	for (const CaptureImage &image : capture.images) {
		if (image.texture != nullptr) {
			draw_ui_texture(ctx, image.texture, image.box.x, image.box.y,
				image.box.width, image.box.height, 1.0);
		}
	}
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
// zooms and scales with everything else there
// (docs/intents/platform/ipc.md).
void draw_canvas_surfaces(const RenderCtx &ctx) {
	for (const auto &surfaces : ctx.output->layers) {
		for (const auto &layer : surfaces) {
			if (!layer->mapped || !layer->canvas_box.has_value()) {
				continue;
			}
			const wlr_box &box = *layer->canvas_box;
			Rect on_plane{double(box.x), double(box.y), double(box.width),
				double(box.height)};
			if (!on_plane.intersects(ctx.viewport)) {
				continue;
			}
			wlr_surface *surface = layer->wlr->surface;
			draw_surface_logical(ctx, surface, canvas_x(ctx, box.x),
				canvas_y(ctx, box.y), box.width * ctx.cam.zoom,
				box.height * ctx.cam.zoom, 1.0f, true);
		}
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
		if (!view->mapped || view->minimized || !view->fullscreen) {
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

void draw_view(const RenderCtx &ctx, View &view) {
	if (!view.mapped || view.minimized) {
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
			view.deco.update(ctx.deco_scale);
			if (view.deco.texture() != nullptr) {
				draw_ui_texture(ctx, view.deco.texture(),
					canvas_x(ctx, frame.x), canvas_y(ctx, frame.y),
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

	wlr_box geo = view.shell_geometry();
	if (wlr_box_empty(&geo)) {
		// No window geometry set yet: the base surface fills it.
		geo = {.x = 0, .y = 0,
			.width = view.surface() != nullptr
				? view.surface()->current.width : geo.width,
			.height = view.surface() != nullptr
				? view.surface()->current.height : geo.height};
	}
	double ox = view.x - geo.x, oy = view.y - geo.y;
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

	Desktop *hovered = nullptr;
	if (ctx.overview) {
		if (std::optional<CanvasPos> pos = server.input.cursor_canvas_pos();
				pos && pos->output == &o) {
			hovered = server.canvas.desktops.at(pos->cx, pos->cy);
		}
	}

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
		Rect tr = box_rect(t);
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
	Rect br = box_rect(box);
	bool box_visible = br.intersects(ctx.viewport);

	if (g->scale > config::pinch_tile_scale && box_visible) {
		draw_canvas_rect(ctx, box.x, box.y, box.width, box.height,
			color_rgba(o.server.theme.colors().accent, 0.12f));
	}
	if (merge_from != nullptr) {
		const wlr_box &m = merge_from->frame();
		Rect mr = box_rect(m);
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

void draw_tile_glyph(const RenderCtx &ctx, const wlr_box &button, wlr_render_color color) {
	constexpr int off[4][2] = {{4, 4}, {13, 4}, {4, 13}, {13, 13}};
	for (int i = 0; i < 4; i++) {
		draw_logical_rect(ctx, button.x + off[i][0], button.y + off[i][1], 7, 7, color);
	}
}

void draw_abandon_glyph(const RenderCtx &ctx, const wlr_box &button, wlr_render_color color) {
	draw_stepped_line(ctx, button.x + 7, button.y + 7,
		button.x + 17, button.y + 17, 2.0, 1.0, color);
	draw_stepped_line(ctx, button.x + 7, button.y + 17,
		button.x + 17, button.y + 7, 2.0, 1.0, color);
}

void draw_tabs(const RenderCtx &ctx) {
	Output &o = *ctx.output;
	Server &server = o.server;
	for (const Desktop &d : server.canvas.desktops.all()) {
		// The target's tab is the accent fill, so its label is drawn in
		// the accent-text variant: a contrasting colour in both modes.
		bool target = o.overview.target().desktop == d.id();
		// Make sure the cached label matches this output's scale before
		// the layout measures it.
		DesktopLabel label = server.painter.desktop_label(d, ctx.scale, target);
		std::optional<TabLayout> layout = server.painter.overview_tab_layout(o, d);
		if (!layout) {
			continue;
		}
		uint32_t bg = target ? server.theme.colors().accent : server.theme.colors().panel;
		draw_logical_rect(ctx, layout->tab.x, layout->tab.y, layout->tab.width,
			layout->tab.height, color_rgba(bg));
		if (label.texture != nullptr && label.w > 0 && label.h > 0) {
			draw_ui_texture(ctx, label.texture, layout->tab.x + 12,
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

// The shell's overview toolbar on this output, while it is mapped: the
// strip surface the overview's bottom UI keeps clear of when it sits on
// the bottom edge. Keep the namespace in sync with plugins/canvas.toolbar
// (docs/intents/shell/toolbar.md).
constexpr const char *toolbar_namespace = "wayland-canvas-toolbar";

const LayerSurface *output_toolbar(const Output &o) {
	for (const auto &vec : o.layers) {
		for (const std::unique_ptr<LayerSurface> &layer : vec) {
			if (layer->mapped && layer->wlr->namespace_ != nullptr
					&& std::strcmp(layer->wlr->namespace_, toolbar_namespace) == 0) {
				return layer.get();
			}
		}
	}
	return nullptr;
}

// The y the overview's bottom UI — the help line, the hints, the shelf —
// stays above: the output's bottom edge, or the top of a toolbar anchored
// to it. The shelf's hit-testing reads the same layout
// (input.cpp:overview_press_target), so what is drawn and what takes the
// pointer are one box.
int overview_bottom(const Output &o) {
	const LayerSurface *toolbar = output_toolbar(o);
	if (toolbar != nullptr
			&& (toolbar->wlr->current.anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) != 0) {
		return toolbar->geo.y;
	}
	return o.height;
}

// Shelf thumbnails: the view's frame fitted into the box, aspect kept.
void draw_shelf(const RenderCtx &ctx) {
	Shelf shelf = overview_shelf_layout(*ctx.output);
	if (shelf.empty()) {
		return;
	}
	int left = shelf.front().box.x, right = shelf.back().box.x + shelf.back().box.width;
	int top = shelf.front().box.y, bottom = shelf.front().box.y + shelf.front().box.height;
	draw_logical_rect(ctx, left - 10, top - 10, right - left + 20, bottom - top + 20,
		color_rgba(ctx.output->server.theme.colors().panel));

	for (std::size_t i = 0; i < shelf.size(); i++) {
		View *view = shelf[i].view;
		const wlr_box &box = shelf[i].box;
		wlr_box frame = view->frame_box();
		double fit = 0.0;
		if (frame.width > 0 && frame.height > 0) {
			fit = std::fmin(box.width / static_cast<double>(frame.width),
				(box.height - 16.0) / frame.height);
		}
		wlr_box geo = view->shell_geometry();
		if (wlr_box_empty(&geo)) {
			geo = {.x = 0, .y = 0,
				.width = view->surface() != nullptr
					? view->surface()->current.width : geo.width,
				.height = view->surface() != nullptr
					? view->surface()->current.height : geo.height};
		}
		double ox = view->x - geo.x, oy = view->y - geo.y;
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
			double thumbs_h = box.height - 16.0;
			double x = box.x + (box.width - frame.width * fit) / 2.0
				+ (cx - frame.x) * fit;
			double y = box.y + (thumbs_h - frame.height * fit) / 2.0
				+ (cy - frame.y) * fit;
			draw_surface_logical(ctx, surface, x, y, cw * fit, ch * fit, 1.0f, true);
		});

		const char *title = view->shell_title();
		if (title != nullptr && title[0] != '\0') {
			TextTexture t = draw_text(ctx.output->server.renderer, title, config::ui_font,
				ctx.output->server.theme.colors().panel_text, 0x00000000u, ctx.scale, 0, 0);
			if (t.texture != nullptr && t.width > 0 && t.height > 0) {
				double w = std::fmin(t.width, static_cast<double>(box.width));
				draw_ui_texture(ctx, t.texture.get(),
					box.x + (box.width - w) / 2.0,
					box.y + box.height - t.height, w, t.height, w / t.width);
			}
		}
	}
}

// ----------------------------------------------------- help line/hint

HelpSlot &help_texture(const RenderCtx &ctx) {
	const GestureOptions &options = ctx.output->server.gestures.options();
	for (HelpSlot &slot : ctx.help_cache) {
		if (slot.texture != nullptr && slot.scale == ctx.scale
				&& slot.preset == options.swipes) {
			return slot;
		}
	}
	HelpSlot &slot = ctx.help_cache[(*ctx.help_clock)++ % ctx.help_cache.size()];
	slot.texture.reset();
	TextTexture t = draw_text(ctx.output->server.renderer,
		overview_help_text(options), config::ui_font,
		ctx.output->server.theme.colors().panel_text,
		ctx.output->server.theme.colors().panel, ctx.scale, 10, 5);
	slot.texture = std::move(t.texture);
	slot.w = t.width;
	slot.h = t.height;
	slot.scale = ctx.scale;
	slot.preset = options.swipes;
	return slot;
}

int draw_help_line(const RenderCtx &ctx) {
	HelpSlot &help = help_texture(ctx);
	if (help.texture == nullptr || help.w <= 0) {
		return 0;
	}
	// Narrow outputs: shrink the line rather than clip it.
	double fit = std::fmin(1.0, (ctx.output->width - 24.0) / help.w);
	double w = help.w * fit, h = help.h * fit;
	draw_ui_texture(ctx, help.texture.get(), (ctx.output->width - w) / 2.0,
		overview_bottom(*ctx.output) - 12 - h, w, h, 1.0);
	return static_cast<int>(std::ceil(h));
}

void draw_hint(const RenderCtx &ctx, int help_h) {
	Output &o = *ctx.output;
	Hint &hint = o.server.hint;
	if (hint.text.empty()) {
		return;
	}
	if (hint.texture == nullptr || hint.scale != ctx.scale) {
		hint.texture.reset();
		TextTexture t = draw_text(o.server.renderer, hint.text.c_str(), config::ui_font,
			o.server.theme.colors().panel_text, o.server.theme.colors().panel,
			ctx.scale, 10, 5);
		hint.texture = std::move(t.texture);
		hint.w = t.width;
		hint.h = t.height;
		hint.scale = ctx.scale;
	}
	if (hint.texture == nullptr || hint.w <= 0 || hint.h <= 0) {
		return;
	}
	int bottom = overview_bottom(o);
	int y = ctx.overview
		? bottom - 12 - help_h - 8 - hint.h
		: o.height - 48 - hint.h;
	draw_ui_texture(ctx, hint.texture.get(),
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

// ------------------------------------------------------- canvas items

// Plugin-owned cards above the windows and their title bars, in canvas
// space, below the top and overlay layers.

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
			double scale = 1;
			wl_list_for_each(so, &surface->current_outputs, link) {
				scale = std::fmax(scale, so->output->scale);
			}
			wlr_fractional_scale_v1_notify_scale(surface, scale);
			wlr_surface_set_preferred_buffer_scale(surface,
				static_cast<int32_t>(std::ceil(scale)));
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
	case SwipePreset::three_finger_hops:
		return "3 fingers: jump to desktop  ·  4 down: go there  ·  "
			"4 sideways: abandon  ·  3-finger pinch out / in: tile / combine  ·  "
			"drag frames over windows  ·  right-click: new desktop  ·  Esc: back";
	case SwipePreset::four_finger_hops:
		return "4 fingers: jump to desktop  ·  3 down: go there  ·  "
			"3 sideways: abandon  ·  4-finger pinch out / in: tile / combine  ·  "
			"drag frames over windows  ·  right-click: new desktop  ·  Esc: back";
	case SwipePreset::off:
		return "drag frames over windows  ·  right-click: new desktop  ·  Esc: back";
	}
	return "";
}

// ------------------------------------------------------------- public

Painter::LabelSlot *Painter::label_slot(DesktopId id) {
	for (LabelSlot &slot : label_cache_) {
		if (slot.id == id) {
			return &slot;
		}
	}
	return nullptr;
}

const Painter::LabelSlot *Painter::label_slot(DesktopId id) const {
	for (const LabelSlot &slot : label_cache_) {
		if (slot.id == id) {
			return &slot;
		}
	}
	return nullptr;
}

DesktopLabel Painter::desktop_label(const Desktop &desktop, float scale, bool on_accent) {
	LabelSlot *slot = label_slot(desktop.id());
	if (slot == nullptr) {
		label_cache_.push_back(LabelSlot{.id = desktop.id()});
		slot = label_slot(desktop.id());
	}
	TexturePtr &texture = on_accent ? slot->accent_texture : slot->texture;
	if (texture != nullptr && slot->number == desktop.number()
			&& slot->desktop_scale == desktop.scale() && slot->scale == scale) {
		return {texture.get(), slot->w, slot->h};
	}
	texture.reset();
	char buf[48];
	int len;
	if (desktop.scale() == 1.0) {
		len = std::snprintf(buf, sizeof(buf), "Desktop %d", desktop.number());
	} else {
		len = std::snprintf(buf, sizeof(buf), "Desktop %d · %d %%",
			desktop.number(), static_cast<int>(std::lround(desktop.scale() * 100)));
	}
	// The same text in the colour its fill asks for: the panel text of an
	// ordinary tab, the contrasting accent text of the target's accent.
	int w = 0, h = 0;
	if (len > 0) {
		TextTexture t = draw_text(server_.renderer, buf, config::ui_font_bold,
			on_accent ? server_.theme.colors().accent_text
				  : server_.theme.colors().panel_text,
			0x00000000u, scale, 0, 0);
		texture = std::move(t.texture);
		w = t.width;
		h = t.height;
	}
	slot->number = desktop.number();
	slot->desktop_scale = desktop.scale();
	slot->scale = scale;
	slot->w = w;
	slot->h = h;
	return {texture.get(), slot->w, slot->h};
}


std::optional<TabLayout> Painter::overview_tab_layout(const Output &o,
		const Desktop &desktop) const {
	Camera cam = o.camera.drawn();
	double left = cam.cx - o.width / (2.0 * cam.zoom);
	double top = cam.cy - o.height / (2.0 * cam.zoom);
	double offset = server_.gestures.abandon_offset(desktop.id());
	double frame_left = (desktop.frame().x - left) * cam.zoom + offset;
	double frame_top = (desktop.frame().y - top) * cam.zoom;

	char buf[32];
	int len = std::snprintf(buf, sizeof(buf), "Desktop %d", desktop.number());
	const LabelSlot *slot = label_slot(desktop.id());
	int label_w = slot != nullptr && slot->scale == o.wlr->scale
		&& slot->desktop_scale == desktop.scale()
		? slot->w : 9 * (len > 0 ? len : 0);

	TabLayout layout;
	layout.tab.x = static_cast<int>(std::round(frame_left));
	layout.tab.y = static_cast<int>(std::round(frame_top)) - 28 - 4;
	if (layout.tab.y < 4) {
		layout.tab.y = static_cast<int>(std::round(frame_top)) + 4;
	}
	layout.tab.width = 12 + label_w + 8 + 2 * 24 + 6;
	layout.tab.height = 28;

	layout.tile_button.x = layout.tab.x + 12 + label_w + 8;
	layout.tile_button.y = layout.tab.y + 2;
	layout.tile_button.width = 24;
	layout.tile_button.height = 24;
	layout.abandon_button.x = layout.tile_button.x + 24 + 6;
	layout.abandon_button.y = layout.tab.y + 2;
	layout.abandon_button.width = 24;
	layout.abandon_button.height = 24;

	if (layout.tab.x < o.width && layout.tab.x + layout.tab.width > 0
			&& layout.tab.y < o.height && layout.tab.y + layout.tab.height > 0) {
		return layout;
	}
	return std::nullopt;
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
	int bw = 160, bh = 100, gap = 12;
	// The help line's margin and height, 16 px above it; the bottom is
	// the toolbar's top when one sits on the bottom edge.
	int bottom = overview_bottom(o) - 12 - 28 - 16;
	int total = static_cast<int>(shelf.size()) * bw
		+ (static_cast<int>(shelf.size()) - 1) * gap;
	int x = (o.width - total) / 2;
	for (std::size_t i = 0; i < shelf.size(); i++) {
		shelf[i].box = {.x = x + static_cast<int>(i) * (bw + gap),
			.y = bottom - bh, .width = bw, .height = bh};
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
	this->text.assign(text);
	texture.reset();
	scale = 0; // regenerate at the output's scale
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
	server_.damage_all();
}

void Hint::clear() {
	text.clear();
	texture.reset();
	w = 0;
	h = 0;
	scale = 0;
}

void Hint::theme_changed() {
	texture.reset();
	scale = 0; // redraw at the output's scale, in the new palette
}

void Hint::finish() {
	timer_.reset();
	texture.reset();
	text.clear();
	w = 0;
	h = 0;
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
	ctx.help_cache = help_cache_;
	ctx.help_clock = &help_clock_;
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
	// Drop the labels of desktops that are gone.
	std::erase_if(label_cache_, [this](const LabelSlot &slot) {
		return server_.canvas.desktops.find(slot.id) == nullptr;
	});

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
		draw_view(ctx, *view);
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
	// surfaces, the launcher.
	int help_h = 0;
	if (ctx.overview) {
		draw_pinch_preview(ctx);
		draw_tabs(ctx);
		draw_shelf(ctx);
		help_h = draw_help_line(ctx);
	}
	draw_hint(ctx, help_h);
	draw_layers(ctx, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
	if (std::optional<Launcher::Panel> panel = server_.launcher.panel(output)) {
		draw_logical_rect(ctx, 0, 0, output.width, output.height,
			color_rgba(server_.theme.colors().launcher_backdrop));
		draw_ui_texture(ctx, panel->texture, panel->box.x, panel->box.y,
			panel->box.width, panel->box.height, 1.0);
	}
	// The picker, toolbar and thumbnail are all a capture leaves out: a shot
	// or a shared screen never shows the capture UI that took it.
	if (ctx.show_capture_ui) {
		draw_capture_overlay(ctx);
	}
	draw_drag_icon(ctx);

}

void Painter::render(Output &output) {
	wlr_output *wlr_output = output.wlr;

	wlr_output_state state;
	wlr_output_state_init(&state);
	// The night light rides the output's own gamma ramp where it can hold
	// it, so a capture stays in true colours; where it cannot, the frame is
	// rendered through the same transform instead.
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
	RenderCtx ctx = make_ctx(output, pass);
	// The capture target: the clipped area, untransformed, at the output's
	// scale — an upright image of it whatever the output's own transform.
	// Cursors stay out: an on-screen frame owns them, and paint-cursor
	// capture sessions get them through that frame.
	ctx.transform = WL_OUTPUT_TRANSFORM_NORMAL;
	ctx.out_transform = WL_OUTPUT_TRANSFORM_NORMAL;
	ctx.origin_x = clipped.x;
	ctx.origin_y = clipped.y;
	ctx.vw = clipped.width;
	ctx.vh = clipped.height;
	ctx.tw = target->width;
	ctx.th = target->height;
	std::vector<DrawnSurface> drawn; // a capture sends no surface feedback
	ctx.drawn = &drawn;
	ctx.show_capture_ui = false; // a capture never shows the capture UI that asked for it
	draw_scene(ctx);
	return wlr_render_pass_submit(pass);
}

void Painter::theme_changed() {
	for (HelpSlot &slot : help_cache_) {
		slot.texture.reset();
	}
	for (LabelSlot &slot : label_cache_) {
		slot.texture.reset();
		slot.accent_texture.reset();
	}
}

void Painter::finish() {
	for (HelpSlot &slot : help_cache_) {
		slot.texture.reset();
	}
	label_cache_.clear();
}
