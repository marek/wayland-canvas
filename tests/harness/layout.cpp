#include "harness/layout.h"

#include <cmath>
#include <cstring>
#include "harness/run.h"
#include "platform/layer.h"
#include "platform/output.h"
#include "windows/view.h"

namespace harness {

namespace {

constexpr const char *bar_namespace = "wayland-canvas-bar";
constexpr int default_bar_height = 26;

} // namespace

Point deco_point(const View &view, DecoPart part) {
	Point canvas = button_centre(view.frame_box(), part);
	return output().canvas_to_local(canvas.x, canvas.y);
}

Point title_point(const View &view, double from_left) {
	wlr_box frame = view.frame_box();
	return output().canvas_to_local(frame.x + from_left, frame.y + config::titlebar_height / 2.0);
}

View *titled(std::string_view title) {
	for (View *view : server->views) {
		if (view->mapped && view->shell_title() != nullptr && view->shell_title() == title) {
			return view;
		}
	}
	return nullptr;
}

Point on_screen(const View &view, double sx, double sy) {
	return output().canvas_to_local(view.x + sx, view.y + sy);
}

wlr_box frame_on_screen(const View &view) {
	wlr_box frame = view.frame_box();
	Point corner = output().canvas_to_local(frame.x, frame.y);
	double zoom = output().camera.drawn().zoom;
	return wlr_box{
		.x = static_cast<int>(std::lround(corner.x)),
		.y = static_cast<int>(std::lround(corner.y)),
		.width = static_cast<int>(std::lround(frame.width * zoom)),
		.height = static_cast<int>(std::lround(frame.height * zoom)),
	};
}

LayerSurface *layer_named(const char *ns) {
	for (const std::unique_ptr<Output> &each : server->outputs) {
		for (const auto &layer : each->layers) {
			for (const std::unique_ptr<LayerSurface> &surface : layer) {
				if (surface->mapped && surface->wlr->namespace_ != nullptr
						&& std::strcmp(surface->wlr->namespace_, ns) == 0) {
					return surface.get();
				}
			}
		}
	}
	return nullptr;
}

int bar_height() {
	LayerSurface *bar = layer_named(bar_namespace);
	return bar != nullptr ? bar->geo.height : default_bar_height;
}

double bar_mid_y() {
	return bar_height() / 2.0;
}

} // namespace harness
