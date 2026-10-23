// The bar client's surfaces: finding a panel or the toolbar, waiting for one to stop changing.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

constexpr const char *toolbar_namespace = "wayland-canvas-toolbar";

std::vector<Band> panel_bands(const char *name) {
	cairo_surface_t *panel = load_png(name);
	std::vector<Band> bands = panel != nullptr ? text_bands(panel, popup_box)
						   : std::vector<Band>();
	if (panel != nullptr) {
		cairo_surface_destroy(panel);
	}
	return bands;
}

// The panel's shape: each band's extent and a coarse lit count. The caret
// blinking in the password field lights a few pixels inside one band; the
// shape stays put, and a stable shape means the rows the flows aim at are
// where the screenshot says.
std::string band_shape(cairo_surface_t *panel) {
	std::string shape;
	for (const Band &b : text_bands(panel, popup_box)) {
		shape += std::format("{}:{}:{},", b.top, b.bottom, b.lit / 16);
	}
	return shape;
}

int settle_stage = 0; // 0: idle, 1: first shot taken, 2: sampling pairs
int settle_retries = 0;
std::string settle_bytes; // the previous shot's shape

} // namespace

LayerSurface *named_layer(const char *ns) {
	for (const std::unique_ptr<Output> &output : server->outputs) {
		for (auto &vec : output->layers) {
			for (const std::unique_ptr<LayerSurface> &l : vec) {
				if (l->mapped && l->wlr->namespace_ != nullptr
						&& std::strcmp(l->wlr->namespace_, ns) == 0) {
					return l.get();
				}
			}
		}
	}
	return nullptr;
}

LayerSurface *bar_layer() {
	return named_layer(bar_namespace);
}

wl_client *bar_client() {
	LayerSurface *l = bar_layer();
	return l == nullptr ? nullptr
		: wl_resource_get_client(l->wlr->surface->resource);
}

LayerSurface *panel_layer(const char *name) {
	std::string ns = std::format("wayland-canvas-panel-{}", name);
	return named_layer(ns.c_str());
}

LayerSurface *toolbar_layer() {
	return named_layer(toolbar_namespace);
}

int panel_layers() {
	int n = 0;
	for (const std::unique_ptr<Output> &output : server->outputs) {
		for (auto &vec : output->layers) {
			for (const std::unique_ptr<LayerSurface> &l : vec) {
				if (l->mapped && l->wlr->namespace_ != nullptr
						&& std::strncmp(l->wlr->namespace_, "wayland-canvas-panel",
							20) == 0) {
					n++;
				}
			}
		}
	}
	return n;
}

wlr_box card_box(double icon_x, int width) {
	double x = icon_x - width / 2.0;
	x = std::max(8.0, std::min(x, double(screen_w - width - 8)));
	return wlr_box{static_cast<int>(std::round(x)), bar_height(), width, 0};
}

std::vector<Band> bands_in(cairo_surface_t *s, const wlr_box &rect) {
	std::vector<Band> bands;
	if (s == nullptr) {
		return bands;
	}
	unsigned char *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
	int x0 = std::max(0, rect.x), x1 = std::min(w, rect.x + rect.width);
	bool in_band = false;
	int band_top = 0, band_lit = 0;
	for (int y = std::max(0, rect.y); y < std::min(h, rect.y + rect.height); y++) {
		int lit = 0;
		for (int x = x0; x < x1; x++) {
			unsigned char *p = data + y * stride + x * 4;
			if (p[0] + p[1] + p[2] > 300) {
				lit++;
			}
		}
		if (lit > 0 && !in_band) {
			in_band = true;
			band_top = y;
			band_lit = lit;
		} else if (lit > 0) {
			band_lit += lit;
		} else if (in_band) {
			in_band = false;
			bands.push_back({band_top, y, band_lit});
		}
	}
	if (in_band) { // a band running to the scan's end
		bands.push_back({band_top, std::min(h, rect.y + rect.height), band_lit});
	}
	return bands;
}

std::vector<Band> text_bands(cairo_surface_t *s, const wlr_box &card) {
	return bands_in(s, {card.x + 8, card.y, card.width - 16, screen_h - card.y});
}

std::vector<Band> panel_rows(const char *name) {
	std::vector<Band> rows;
	for (const Band &b : panel_bands(name)) {
		if (b.lit >= 60) {
			rows.push_back(b);
		}
	}
	return rows;
}

std::vector<Band> card_rows(cairo_surface_t *s, const wlr_box &card) {
	if (s == nullptr) {
		return {};
	}
	unsigned char *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	int h = cairo_image_surface_get_height(s);
	auto sum = [&](int y) {
		unsigned char *p = data + y * stride + (card.x + 6) * 4;
		return p[0] + p[1] + p[2];
	};
	int fill = sum(card.y + 3), bottom = card.y + 3;
	while (bottom < h && std::abs(sum(bottom) - fill) < 6) {
		bottom++;
	}
	return bands_in(s, {card.x + 8, card.y, card.width - 16, bottom - card.y});
}

std::vector<Band> panel_card_rows(const char *name) {
	cairo_surface_t *s = load_png(name);
	std::vector<Band> rows;
	for (const Band &b : card_rows(s, popup_box)) {
		if (b.lit >= 60) {
			rows.push_back(b);
		}
	}
	if (s != nullptr) {
		cairo_surface_destroy(s);
	}
	return rows;
}

bool settle_advance(const char *name) {
	if (settle_stage == 0) {
		shot(name);
		settle_stage = 1;
		settle_retries = 0;
		return false;
	}
	cairo_surface_t *panel = load_png(name); // the previous call's shot
	std::string shape = panel != nullptr ? band_shape(panel) : std::string();
	if (panel != nullptr) {
		cairo_surface_destroy(panel);
	}
	bool same = shape == settle_bytes;
	settle_bytes = shape;
	if (settle_stage == 1) {
		settle_stage = 2;
		shot(name);
		return false;
	}
	if (!same && ++settle_retries < 25) { // a changing panel; keep watching
		shot(name);
		return false;
	}
	settle_stage = 0; // matched (or given up waiting): the shot is stable
	return true;
}

} // namespace bar
