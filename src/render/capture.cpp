#include <cmath>
#include <drm_fourcc.h>

#include "core/config.h"
#include "platform/output.h"
#include "render/capture.h"
#include "render/draw.h"
#include "core/server.h"
#include "windows/view.h"

namespace {

// A logical box on the untransformed capture target, edges scaled and rounded
// individually like the renderer's logical_box().
wlr_box box_at(double x, double y, double w, double h, float scale) {
	int x0 = static_cast<int>(std::round(x * scale));
	int y0 = static_cast<int>(std::round(y * scale));
	int x1 = static_cast<int>(std::round((x + w) * scale));
	int y1 = static_cast<int>(std::round((y + h) * scale));
	return wlr_box{x0, y0, x1 - x0, y1 - y0};
}

// The device-pixel size a logical box occupies, rounded the same way.
wlr_box device_box(const wlr_box &logical, float scale) {
	return box_at(logical.x, logical.y, logical.width, logical.height, scale);
}

void draw_rect(wlr_render_pass *pass, double x, double y, double w, double h,
		float scale, wlr_render_color color) {
	wlr_box box = box_at(x, y, w, h, scale);
	if (wlr_box_empty(&box)) {
		return;
	}
	const wlr_render_rect_options opts = {
		.box = box,
		.color = color,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	};
	wlr_render_pass_add_rect(pass, &opts);
}

// An upright-authored texture (a title bar) drawn at logical (x, y).
void draw_texture(wlr_render_pass *pass, wlr_texture *texture,
		double x, double y, double w, double h, float scale) {
	wlr_box dst = box_at(x, y, w, h, scale);
	if (wlr_box_empty(&dst)) {
		return;
	}
	const wlr_fbox src = {
		.x = 0, .y = 0,
		.width = static_cast<double>(texture->width),
		.height = static_cast<double>(texture->height),
	};
	const wlr_render_texture_options opts = {
		.texture = texture,
		.src_box = src,
		.dst_box = dst,
		.filter_mode = WLR_SCALE_FILTER_BILINEAR,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	};
	wlr_render_pass_add_texture(pass, &opts);
}

// A client surface at logical (x, y) sized (lw, lh), like the renderer's
// draw_surface_logical() on an untransformed target.
void draw_surface(wlr_render_pass *pass, wlr_surface *surface,
		double x, double y, double lw, double lh, float scale) {
	wlr_texture *texture = wlr_surface_get_texture(surface);
	if (texture == nullptr) {
		return;
	}
	wlr_box dst = box_at(x, y, lw, lh, scale);
	if (wlr_box_empty(&dst)) {
		return;
	}
	wlr_fbox src;
	wlr_surface_get_buffer_source_box(surface, &src);
	wl_output_transform transform = wlr_output_transform_invert(surface->current.transform);
	const wlr_render_texture_options opts = {
		.texture = texture,
		.src_box = src,
		.dst_box = dst,
		.transform = transform,
		.filter_mode = WLR_SCALE_FILTER_BILINEAR,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	};
	wlr_render_pass_add_texture(pass, &opts);
}

} // namespace

wlr_buffer *make_capture_buffer(Server &server, int width, int height) {
	if (width <= 0 || height <= 0) {
		return nullptr;
	}
	const wlr_drm_format *format = nullptr;
	// What the renderer renders into on this machine: any output's primary
	// buffer format — the outputs share the allocator and renderer.
	for (const auto &output : server.outputs) {
		if (output->wlr->swapchain != nullptr) {
			format = &output->wlr->swapchain->format;
			break;
		}
	}
	if (format == nullptr) {
		// Nothing has rendered yet: the renderer's ARGB8888 format for the
		// allocator's kind of buffer.
		const wlr_drm_format_set *formats = wlr_renderer_get_texture_formats(
			server.renderer, server.allocator->buffer_caps);
		if (formats != nullptr) {
			format = wlr_drm_format_set_get(formats, DRM_FORMAT_ARGB8888);
		}
	}
	if (format == nullptr) {
		return nullptr;
	}
	return wlr_allocator_create_buffer(server.allocator, width, height, format);
}

CairoSurfacePtr buffer_to_cairo(wlr_renderer *renderer, wlr_buffer *buffer) {
	if (buffer == nullptr) {
		return {};
	}
	TexturePtr texture{wlr_texture_from_buffer(renderer, buffer)};
	if (texture == nullptr) {
		wlr_log(WLR_ERROR, "capture: the buffer cannot be read back");
		return {};
	}
	CairoSurfacePtr surface{cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		buffer->width, buffer->height)};
	if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) {
		return {};
	}
	const wlr_texture_read_pixels_options opts = {
		.data = cairo_image_surface_get_data(surface.get()),
		.format = DRM_FORMAT_ARGB8888,
		.stride = static_cast<uint32_t>(cairo_image_surface_get_stride(surface.get())),
	};
	if (!wlr_texture_read_pixels(texture.get(), &opts)) {
		return {};
	}
	cairo_surface_mark_dirty(surface.get());
	return surface;
}

wlr_box view_device_size(const View &view, float scale) {
	wlr_box frame = view.frame_box();
	return device_box({0, 0, frame.width, frame.height}, scale);
}

bool render_view_offscreen(View &view, float scale, wlr_buffer *buffer) {
	if (!view.mapped || buffer == nullptr) {
		return false;
	}
	Server &server = view.server;
	if (server.lock.locked()) {
		return false; // nothing behind the lock is captured
	}
	wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(server.renderer, buffer, nullptr);
	if (pass == nullptr) {
		wlr_log(WLR_ERROR, "capture: failed to begin the window's render pass");
		return false;
	}
	// Window sources reuse their buffer frame after frame, and the renderer
	// keeps a target's old contents: start from transparent.
	const wlr_render_rect_options clear = {
		.box = {0, 0, buffer->width, buffer->height},
		.color = {0, 0, 0, 0},
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	};
	wlr_render_pass_add_rect(pass, &clear);

	// The frame's top-left corner is the capture's origin; the layout is the
	// one the renderer's draw_view() draws and deco_hit() hit-tests: the
	// title bar spans the frame width from its top edge, the border runs
	// below it and around the content.
	Rect frame = view.frame_rect();
	int tb = view.deco.titlebar();
	int bw = view.deco.border();
	if (tb > 0) {
		view.deco.update(scale);
		if (view.deco.texture() != nullptr) {
			draw_texture(pass, view.deco.texture(), 0, 0, frame.w, tb, scale);
		}
	}
	if (bw > 0) {
		const config::Palette &pal = server.theme.colors();
		wlr_render_color border = color_rgba(
			server.focused_view == &view ? pal.border_active : pal.border_inactive);
		draw_rect(pass, 0, tb, frame.w, bw, scale, border);
		draw_rect(pass, 0, frame.h - bw, frame.w, bw, scale, border);
		draw_rect(pass, 0, tb, bw, frame.h - tb - bw, scale, border);
		draw_rect(pass, frame.w - bw, tb, bw, frame.h - tb - bw, scale, border);
	}

	wlr_box geo = view.wlr->base->geometry;
	if (wlr_box_empty(&geo)) {
		// No window geometry set yet: the base surface fills it.
		geo = {.x = 0, .y = 0,
			.width = view.wlr->base->surface->current.width,
			.height = view.wlr->base->surface->current.height};
	}
	double vx = view.x - geo.x, vy = view.y - geo.y; // canvas coords, tree origin
	view.for_each_surface([&](wlr_surface *surface, int sx, int sy) {
		draw_surface(pass, surface, vx + sx - frame.x, vy + sy - frame.y,
			surface->current.width, surface->current.height, scale);
	});
	return wlr_render_pass_submit(pass);
}

CairoSurfacePtr capture_view(View &view, float scale) {
	if (!view.mapped) {
		return {};
	}
	wlr_box dev = view_device_size(view, scale);
	Handle<wlr_buffer, wlr_buffer_drop> buffer{
		make_capture_buffer(view.server, dev.width, dev.height)};
	if (buffer == nullptr) {
		return {};
	}
	if (!render_view_offscreen(view, scale, buffer.get())) {
		return {};
	}
	return buffer_to_cairo(view.server.renderer, buffer.get());
}

CairoSurfacePtr capture_output(Output &output, wlr_box local_area) {
	wlr_box bounds{0, 0, output.width, output.height};
	wlr_box clipped{};
	wlr_box_intersection(&clipped, &local_area, &bounds);
	if (wlr_box_empty(&clipped)) {
		return {};
	}
	// Sized from the area's own origin, the way render_area() rounds it.
	wlr_box dev = device_box({0, 0, clipped.width, clipped.height}, output.wlr->scale);
	Handle<wlr_buffer, wlr_buffer_drop> buffer{
		make_capture_buffer(output.server, dev.width, dev.height)};
	if (buffer == nullptr) {
		return {};
	}
	if (!output.server.painter.render_area(output, clipped, buffer.get())) {
		return {};
	}
	return buffer_to_cairo(output.server.renderer, buffer.get());
}
