#include <cmath>
#include <drm_fourcc.h>
#include <numbers>

#include "render/draw.h"
namespace {

// A label is one line: a newline in a window's title must not grow it.
PangoLayoutPtr label_layout(cairo_t *cr, const char *text, const char *font) {
	PangoLayoutPtr layout = make_layout(cr, text, font);
	pango_layout_set_single_paragraph_mode(layout.get(), true);
	return layout;
}

} // namespace

void set_source_rgba32(cairo_t *cr, uint32_t rgba) {
	cairo_set_source_rgba(cr,
		((rgba >> 24) & 0xff) / 255.0,
		((rgba >> 16) & 0xff) / 255.0,
		((rgba >> 8) & 0xff) / 255.0,
		(rgba & 0xff) / 255.0);
}

void rounded_path(cairo_t *cr, double x, double y, double w, double h, double r) {
	r = std::fmin(r, std::fmin(w, h) / 2.0);
	cairo_new_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -std::numbers::pi / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, std::numbers::pi / 2);
	cairo_arc(cr, x + r, y + h - r, r, std::numbers::pi / 2, std::numbers::pi);
	cairo_arc(cr, x + r, y + r, r, std::numbers::pi, 3 * std::numbers::pi / 2);
	cairo_close_path(cr);
}

PangoLayoutPtr make_layout(cairo_t *cr, const char *text, const char *font, int avail) {
	PangoLayoutPtr layout{pango_cairo_create_layout(cr)};
	FontDescPtr desc{pango_font_description_from_string(font)};
	pango_layout_set_font_description(layout.get(), desc.get());
	pango_layout_set_text(layout.get(), text, -1);
	if (avail > 0) {
		pango_layout_set_ellipsize(layout.get(), PANGO_ELLIPSIZE_END);
		pango_layout_set_width(layout.get(), avail * PANGO_SCALE);
	}
	return layout;
}

int text_width(cairo_t *cr, const char *text, const char *font, int avail) {
	PangoLayoutPtr layout = make_layout(cr, text, font, avail);
	int w = 0;
	pango_layout_get_pixel_size(layout.get(), &w, nullptr);
	return w;
}

int paint_layout(cairo_t *cr, PangoLayout *layout, uint32_t rgba, double x, double y,
	TextAnchor anchor) {
	int w = 0, h = 0;
	pango_layout_get_pixel_size(layout, &w, &h);
	if (anchor != TextAnchor::top_left) {
		y -= h / 2.0;
	}
	if (anchor == TextAnchor::centre) {
		x -= w / 2.0;
	}
	set_source_rgba32(cr, rgba);
	cairo_move_to(cr, x, y);
	pango_cairo_show_layout(cr, layout);
	return w;
}

int paint_text(cairo_t *cr, const char *text, const char *font, uint32_t rgba, double x,
	double y, TextAnchor anchor, int avail) {
	PangoLayoutPtr layout = make_layout(cr, text, font, avail);
	return paint_layout(cr, layout.get(), rgba, x, y, anchor);
}

TextTexture draw_text(wlr_renderer *renderer, const char *text, const char *font, uint32_t fg,
	uint32_t bg, float scale, int pad_x, int pad_y) {
	if (text == nullptr) {
		return {};
	}

	// Measure at the scale it is drawn at, rounding up: hinting lays text
	// out wider at some scales than at others, and a box measured at
	// another scale cuts off the end of the text.
	CairoSurfacePtr scratch{cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1)};
	CairoPtr cr{cairo_create(scratch.get())};
	cairo_scale(cr.get(), scale, scale);
	PangoLayoutPtr layout = label_layout(cr.get(), text, font);
	PangoRectangle logical{};
	pango_layout_get_extents(layout.get(), nullptr, &logical);
	int lw = static_cast<int>(std::ceil(static_cast<double>(logical.width) / PANGO_SCALE));
	int lh = static_cast<int>(std::ceil(static_cast<double>(logical.height) / PANGO_SCALE));
	layout.reset();
	cr.reset();
	scratch.reset();

	int w = lw + 2 * pad_x;
	int h = lh + 2 * pad_y;
	if (w <= 0 || h <= 0) {
		return {};
	}

	CairoSurfacePtr surf{cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		static_cast<int>(std::ceil(w * scale)), static_cast<int>(std::ceil(h * scale)))};
	cr = CairoPtr{cairo_create(surf.get())};
	cairo_scale(cr.get(), scale, scale);

	if ((bg & 0xff) != 0) {
		set_source_rgba32(cr.get(), bg);
		cairo_paint(cr.get());
	}

	layout = label_layout(cr.get(), text, font);
	paint_layout(cr.get(), layout.get(), fg, pad_x, pad_y, TextAnchor::top_left);

	return {texture_from_cairo(renderer, surf.get()), w, h};
}

TexturePtr texture_from_cairo(wlr_renderer *renderer, cairo_surface_t *surface) {
	cairo_surface_flush(surface);
	return TexturePtr{wlr_texture_from_pixels(renderer, DRM_FORMAT_ARGB8888,
		static_cast<uint32_t>(cairo_image_surface_get_stride(surface)),
		static_cast<uint32_t>(cairo_image_surface_get_width(surface)),
		static_cast<uint32_t>(cairo_image_surface_get_height(surface)),
		cairo_image_surface_get_data(surface))};
}
