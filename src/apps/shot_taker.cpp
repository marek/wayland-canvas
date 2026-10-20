#include "apps/shot_taker.h"

#include <unistd.h>

#include <glib.h>

#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "apps/capture_files.h"
#include "apps/capture_geometry.h"
#include "apps/clipboard_image.h"
#include "apps/shot_thumbnail.h"
#include "core/fs.h"
#include "core/server.h"
#include "platform/output.h"
#include "render/capture.h"

namespace {

cairo_status_t png_collect(void *closure, const unsigned char *data, unsigned length) {
	auto *out = static_cast<std::vector<std::uint8_t> *>(closure);
	out->insert(out->end(), data, data + length);
	return CAIRO_STATUS_SUCCESS;
}

} // namespace

ShotTaker::ShotTaker(Server &server, ClipboardImage &clipboard, ShotThumbnail &thumbnail)
	: server_(server), clipboard_(clipboard), thumbnail_(thumbnail) {
}

void ShotTaker::take_shot(Output &output, const std::optional<wlr_box> &area, View *view,
		const CaptureOptions &options) {
	wlr_box whole{0, 0, output.width, output.height};
	wlr_box target = area.value_or(whole);
	CairoSurfacePtr surface = view != nullptr
		? capture_view(*view, output.wlr->scale)
		: capture_output(output, target);
	if (surface == nullptr) {
		server_.hint.set("The screenshot failed");
		return;
	}
	// The cursor, composited in only when it sat inside what was captured.
	wlr_box on_screen = view != nullptr ? frame_on_output(output, *view) : target;
	if (options.show_pointer) {
		composite_cursor(output, surface.get(), on_screen);
	}
	if (options.clipboard) {
		std::vector<std::uint8_t> png;
		cairo_surface_write_to_png_stream(surface.get(), png_collect, &png);
		clipboard_.copy(std::move(png));
		server_.hint.set("Screenshot copied to the clipboard");
		return;
	}
	std::string dir = user_dir(G_USER_DIRECTORY_PICTURES, "Pictures") + "/Screenshots";
	fs::mkdir_p(dir);
	std::string path = std::format("{}/Screenshot {}.png", dir, timestamp());
	for (int n = 2; access(path.c_str(), F_OK) == 0; n++) { // never overwrite
		path = std::format("{}/Screenshot {} {}.png", dir, timestamp(), n);
	}
	if (cairo_surface_write_to_png(surface.get(), path.c_str()) != CAIRO_STATUS_SUCCESS) {
		server_.hint.set("The screenshot could not be saved");
		return;
	}
	thumbnail_.show(path, output, surface.get());
	server_.hint.set("Screenshot saved to " + dir);
}

// Draws the cursor image onto the shot, at the place the pointer had on
// screen, when the pointer was over this output and inside `on_screen`.
void ShotTaker::composite_cursor(Output &output, cairo_surface_t *shot,
		const wlr_box &on_screen) {
	double ox = server_.cursor->x - output.lx;
	double oy = server_.cursor->y - output.ly;
	if (!wlr_box_contains_point(&on_screen, ox, oy)) {
		return;
	}
	float scale = output.wlr->scale;
	wlr_xcursor *cursor = wlr_xcursor_manager_get_xcursor(server_.xcursor_mgr,
		"default", scale);
	if (cursor == nullptr || cursor->image_count == 0) {
		return;
	}
	wlr_xcursor_image *image = cursor->images[0];
	cairo_surface_t *pixels = cairo_image_surface_create_for_data(image->buffer,
		CAIRO_FORMAT_ARGB32, static_cast<int>(image->width),
		static_cast<int>(image->height), static_cast<int>(image->width) * 4);
	CairoSurfacePtr hold{pixels};
	CairoPtr cr{cairo_create(shot)};
	cairo_set_source_surface(cr.get(), pixels,
		(ox - on_screen.x) * scale - image->hotspot_x,
		(oy - on_screen.y) * scale - image->hotspot_y);
	cairo_paint(cr.get());
}
