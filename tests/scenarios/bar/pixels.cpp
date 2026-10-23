// Reading screenshots: decoding, the bar's runs, flat palette fills.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

// The renderer's rounding of a flat fill stays within this of the palette.
constexpr int palette_tolerance = 8;

bool near_channel(unsigned char got, unsigned char want) {
	int d = got > want ? got - want : want - got;
	return d <= palette_tolerance;
}

std::string shot_pending; // the shot a shot_holds wait has in flight

} // namespace

cairo_surface_t *load_png(const char *name) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	cairo_surface_t *s = cairo_image_surface_create_from_png(path.c_str());
	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
		check(false, std::format("decoding {}", path));
		cairo_surface_destroy(s);
		return nullptr;
	}
	return s;
}

std::vector<Run> bar_runs(cairo_surface_t *s, const wlr_box &bar) {
	std::vector<Run> runs;
	if (s == nullptr) {
		return runs;
	}
	unsigned char *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	std::vector<bool> lit(bar.width);
	for (int x = 0; x < bar.width; x++) {
		for (int y = 0; y < bar.height; y++) {
			unsigned char *p = data + (bar.y + y) * stride + (bar.x + x) * 4;
			// ARGB32 little-endian: B, G, R, A. The bar background is
			// #1a1b26 (channel sum 91); glyph pixels are far brighter.
			if (p[0] + p[1] + p[2] > 300) {
				lit[x] = true;
				break;
			}
		}
	}
	constexpr int merge_gap = 8;
	int start = -1, gap = 0, last = -1;
	for (int x = 0; x <= bar.width; x++) {
		bool on = x < bar.width && lit[x];
		if (on) {
			if (start < 0) {
				start = x;
			}
			gap = 0;
			last = x;
		} else if (start >= 0 && (++gap > merge_gap || x == bar.width)) {
			// A run touching the box's right edge ends with the box: the
			// gap after it never grows, so it is closed here.
			if (last + 1 - start >= 3) { // drop sub-pixel noise
				runs.push_back({double(start), double(last + 1)});
			}
			start = -1;
		}
	}
	return runs;
}

int bright_count(cairo_surface_t *s, const wlr_box &rect) {
	if (s == nullptr) {
		return -1;
	}
	unsigned char *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
	int n = 0;
	for (int y = rect.y; y < rect.y + rect.height; y++) {
		if (y < 0 || y >= h) {
			continue;
		}
		for (int x = rect.x; x < rect.x + rect.width; x++) {
			if (x < 0 || x >= w) {
				continue;
			}
			unsigned char *p = data + y * stride + x * 4;
			if (p[0] + p[1] + p[2] > 400) {
				n++;
			}
		}
	}
	return n;
}

uint32_t pixel_at(cairo_surface_t *s, int x, int y) {
	unsigned char *p = cairo_image_surface_get_data(s)
		+ y * cairo_image_surface_get_stride(s) + x * 4;
	return uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | uint32_t(p[0]);
}

bool pixel_is(cairo_surface_t *s, int x, int y, uint32_t want) {
	if (s == nullptr) {
		return false;
	}
	want >>= 8;
	uint32_t got = pixel_at(s, x, y);
	return near_channel(got >> 16, want >> 16)
		&& near_channel(got >> 8, want >> 8) && near_channel(got, want);
}

bool shot_pixel_is(const char *name, int x, int y, uint32_t want) {
	cairo_surface_t *s = load_png(name);
	bool ok = pixel_is(s, x, y, want);
	if (s != nullptr) {
		cairo_surface_destroy(s);
	}
	return ok;
}

std::function<bool(cairo_surface_t *)> pixel_shows(int x, int y, uint32_t want) {
	return [=](cairo_surface_t *s) { return pixel_is(s, x, y, want); };
}

std::function<bool(cairo_surface_t *)> bar_shows(uint32_t want) {
	return [=](cairo_surface_t *s) {
		LayerSurface *bar = bar_layer();
		return bar != nullptr
			&& pixel_is(s, bar->geo.x + 4, bar->geo.y + bar->geo.height / 2, want);
	};
}

std::function<bool(cairo_surface_t *)> bar_drawn() {
	return [](cairo_surface_t *s) {
		LayerSurface *bar = bar_layer();
		return bar != nullptr && bar_runs(s, bar->geo).size() >= bar_run_count;
	};
}

std::optional<Result> shot_holds(const char *name,
		const std::function<bool(cairo_surface_t *)> &holds, Wait wait,
		std::source_location where) {
	if (shot_pending == name) { // the shot of the last poll has landed
		cairo_surface_t *s = load_png(name);
		bool ok = s != nullptr && holds(s);
		if (s != nullptr) {
			cairo_surface_destroy(s);
		}
		if (ok) {
			shot_pending.clear();
			return std::nullopt;
		}
	}
	std::optional<Result> more = wait_for(false, wait, where);
	if (!more) { // the budget is spent: `name` stays the last shot that landed
		shot_pending.clear();
		return std::nullopt;
	}
	shot(name);
	shot_pending = name;
	return more;
}

View *titled(const char *title) {
	for (View *v : server->views) {
		if (v->mapped && v->shell_title() && std::strcmp(v->shell_title(), title) == 0) {
			return v;
		}
	}
	return nullptr;
}

} // namespace bar
