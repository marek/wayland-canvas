#include <cmath>
#include <drm_fourcc.h>

#include "platform/output.h"
#include "render/capture.h"
#include "core/server.h"
#include "windows/view.h"

namespace {

// The device-pixel size a logical size takes at `scale`, rounded the way the
// renderer rounds a box that starts at its target's origin.
wlr_box device_size(double width, double height, float scale) {
	return wlr_box{0, 0, static_cast<int>(std::round(width * scale)),
		static_cast<int>(std::round(height * scale))};
}

// Reads `buffer` into `data` as ARGB8888, `stride` bytes a row.
bool read_pixels(wlr_renderer *renderer, wlr_buffer *buffer, void *data,
		std::uint32_t stride) {
	TexturePtr texture{wlr_texture_from_buffer(renderer, buffer)};
	if (texture == nullptr) {
		wlr_log(WLR_ERROR, "capture: the buffer cannot be read back");
		return false;
	}
	const wlr_texture_read_pixels_options opts = {
		.data = data,
		.format = DRM_FORMAT_ARGB8888,
		.stride = stride,
	};
	return wlr_texture_read_pixels(texture.get(), &opts);
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
	CairoSurfacePtr surface{cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		buffer->width, buffer->height)};
	if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) {
		return {};
	}
	if (!read_pixels(renderer, buffer, cairo_image_surface_get_data(surface.get()),
			static_cast<std::uint32_t>(cairo_image_surface_get_stride(surface.get())))) {
		return {};
	}
	cairo_surface_mark_dirty(surface.get());
	return surface;
}

wlr_box view_device_size(const View &view, float scale) {
	wlr_box frame = view.frame_box();
	return device_size(frame.width, frame.height, scale);
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
	if (!view.server.painter.render_view(view, scale, buffer.get())) {
		return {};
	}
	return buffer_to_cairo(view.server.renderer, buffer.get());
}

wlr_box output_device_size(const Output &output, wlr_box local_area) {
	wlr_box bounds{0, 0, output.width, output.height};
	wlr_box clipped{};
	wlr_box_intersection(&clipped, &local_area, &bounds);
	if (wlr_box_empty(&clipped)) {
		return {};
	}
	// Sized from the area's own origin, the way render_area() rounds it.
	return device_size(clipped.width, clipped.height, output.wlr->scale);
}

CairoSurfacePtr capture_output(Output &output, wlr_box local_area) {
	wlr_box dev = output_device_size(output, local_area);
	if (wlr_box_empty(&dev)) {
		return {};
	}
	Handle<wlr_buffer, wlr_buffer_drop> buffer{
		make_capture_buffer(output.server, dev.width, dev.height)};
	if (buffer == nullptr) {
		return {};
	}
	if (!output.server.painter.render_area(output, local_area, buffer.get())) {
		return {};
	}
	return buffer_to_cairo(output.server.renderer, buffer.get());
}

bool capture_output(Output &output, wlr_box local_area, wlr_buffer *buffer,
		std::span<std::uint8_t> pixels) {
	std::size_t stride = static_cast<std::size_t>(buffer->width) * 4;
	if (pixels.size() < stride * static_cast<std::size_t>(buffer->height)
			|| !output.server.painter.render_area(output, local_area, buffer)) {
		return false;
	}
	return read_pixels(output.server.renderer, buffer, pixels.data(),
		static_cast<std::uint32_t>(stride));
}
