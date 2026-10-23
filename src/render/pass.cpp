#include "render/pass.h"

#include <cmath>

namespace {

void record_surface(const RenderCtx &ctx, wlr_surface *surface, bool frame_done) {
	ctx.drawn->push_back({.surface = surface, .frame_done = frame_done});
}

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

} // namespace

double canvas_x(const RenderCtx &ctx, double cx) {
	return (cx - ctx.left) * ctx.cam.zoom;
}

double canvas_y(const RenderCtx &ctx, double cy) {
	return (cy - ctx.top) * ctx.cam.zoom;
}

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
