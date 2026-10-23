#include <cmath>
#include <ctime>
#include <optional>
#include <ranges>

#include "canvas/overview_chrome.h"
#include "core/box.h"
#include "core/config.h"
#include "render/draw.h"
#include "platform/output.h"
#include "core/server.h"
#include "input/ui_layer.h"

namespace {

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
	for (const LayerSurface *layer : ctx.output->server.canvas_surfaces.bottom_up()) {
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
				server.focus.focused_view() == &view ? pal.border_active : pal.border_inactive);
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

// ------------------------------------------------------------ drag icon

void draw_drag_icon(const RenderCtx &ctx) {
	Server &server = ctx.output->server;
	std::optional<DragAndDrop::Icon> icon = server.input.dnd.icon();
	if (!icon.has_value()) {
		return;
	}
	ScreenIter it = {
		.ctx = &ctx,
		.ox = server.cursor->x - ctx.output->lx + icon->dx,
		.oy = server.cursor->y - ctx.output->ly + icon->dy,
	};
	wlr_surface_for_each_surface(icon->surface, screen_surface_iter, &it);
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
			// Like View::update_outputs: the sharpest output the surface is on wins.
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

// ------------------------------------------------------------- public

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
		draw_overview_chrome(ctx, *bottom);
	}
	server_.hint.draw(ctx, bottom.transform([](const OverviewBottom &b) { return b.hint; }));
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
