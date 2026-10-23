#pragma once

#include <vector>
#include "core/geometry.h"
#include "core/wlr.h"

class Output;

// Surfaces drawn this frame: they need enter/presentation feedback, and frame_done when
// their output owns them. Painter's reused per-frame buffer.
struct DrawnSurface {
	wlr_surface *surface = nullptr;
	bool frame_done = false;
};

// Everything one render target's frame is drawn with: an output's on-screen
// frame, or an offscreen capture of a rectangle of that output (see
// Painter::render_area).
struct RenderCtx {
	Output *output;
	wlr_render_pass *pass;
	Camera cam; // output->camera(): quantized, same as hit testing
	Rect viewport; // visible canvas rectangle
	double left, top; // canvas coords shown at output-local (0, 0)
	float scale;
	// What title bars are drawn for: the output's scale times the zoom the
	// camera rests at, so they stay sharp on a scaled desktop.
	double deco_scale;
	wl_output_transform transform; // maps target-local logical to buffer pixels
	wl_output_transform out_transform; // what textures are counter-rotated by
	int tw, th; // buffer resolution, in transform order
	// The render target: an output-local logical rectangle — the whole output
	// for an on-screen frame, the captured area for an offscreen one.
	double origin_x = 0, origin_y = 0;
	int vw = 0, vh = 0;
	// Whether this is an offscreen capture: it shows what the output shows,
	// minus the compositor's UI that asked for it (the capture UI, which a
	// shot or a shared screen never shows).
	bool offscreen = false;
	bool overview;
	std::vector<DrawnSurface> *drawn; // Painter's reused per-frame buffer
};

// Canvas coordinates to output-local logical px at the camera of `ctx`.
double canvas_x(const RenderCtx &ctx, double cx);
double canvas_y(const RenderCtx &ctx, double cy);

// Screen-space rect (output-local logical px), clipped to the render target;
// empty rects are skipped. `color` is premultiplied.
void draw_logical_rect(const RenderCtx &ctx, double x, double y, double w, double h,
	wlr_render_color color);
// The same for a rect in canvas coordinates.
void draw_canvas_rect(const RenderCtx &ctx, double x, double y, double w, double h,
	wlr_render_color color);
// Dashed rectangle outline (dash 10, gap 6), in screen space. The horizontal
// edges carry the corners; the vertical edges run between them so corners
// aren't drawn twice. Only the on-screen dashes are drawn.
void draw_dashed_logical_rect(const RenderCtx &ctx, double x, double y, double w, double h,
	double thickness, wlr_render_color color);
// Line approximated by rects: `step`-long dashes along the major axis for long
// connectors, or thickness-sized dots every pixel (step <= thickness) for small
// diagonal glyphs.
void draw_stepped_line(const RenderCtx &ctx, double x0, double y0, double x1, double y1,
	double thickness, double step, wlr_render_color color);
// A cairo texture of ours at (x, y) of size (w, h), output-local logical px;
// `src_frac` crops its source horizontally (0..1).
void draw_ui_texture(const RenderCtx &ctx, wlr_texture *texture, double x, double y,
	double w, double h, double src_frac);
// A client surface at (lx, ly) of size (lw, lh), output-local logical px; it
// is recorded for the frame's enter and presentation feedback.
void draw_surface_logical(const RenderCtx &ctx, wlr_surface *surface,
	double lx, double ly, double lw, double lh, float alpha, bool frame_done);

// The surfaces of a tree positioned in output-local screen space (layer and
// drag-icon surfaces): the callback of wlr_surface_for_each_surface and
// wlr_layer_surface_v1_for_each_surface, with a ScreenIter as its data.
struct ScreenIter {
	const RenderCtx *ctx;
	double ox, oy; // output-local position of the surface tree's origin
};

void screen_surface_iter(wlr_surface *surface, int sx, int sy, void *data);
