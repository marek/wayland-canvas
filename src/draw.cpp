#include <cmath>
#include <drm_fourcc.h>
#include <pango/pangocairo.h>

#include "draw.h"

namespace {

using PangoLayoutPtr = Handle<PangoLayout, g_object_unref>;

PangoLayoutPtr make_layout(cairo_t *cr, const char *text, const char *font) {
	PangoLayoutPtr layout{pango_cairo_create_layout(cr)};
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout.get(), desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout.get(), text, -1);
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

TextTexture draw_text(wlr_renderer *renderer, const char *text, const char *font, uint32_t fg,
	uint32_t bg, float scale, int pad_x, int pad_y) {
	if (text == nullptr) {
		return {};
	}

	// Measure on a scratch context: the surface needs its final size up front.
	CairoSurfacePtr scratch{cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1)};
	CairoPtr cr{cairo_create(scratch.get())};
	PangoLayoutPtr layout = make_layout(cr.get(), text, font);
	int lw = 0, lh = 0;
	pango_layout_get_pixel_size(layout.get(), &lw, &lh);
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

	layout = make_layout(cr.get(), text, font);
	set_source_rgba32(cr.get(), fg);
	cairo_move_to(cr.get(), pad_x, pad_y);
	pango_cairo_show_layout(cr.get(), layout.get());

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
