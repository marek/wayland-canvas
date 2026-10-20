#pragma once

#include <cstdint>
#include <cairo.h>
#include <pango/pangocairo.h>
#include "core/util.h"

struct TextTexture {
	TexturePtr texture; // nullptr for a null text or an empty result
	int width = 0, height = 0; // logical px, padding included
};

// `text` (nullptr draws nothing) in `font`, fg on bg (0xRRGGBBAA; a bg with zero alpha
// stays transparent), `pad_x`/`pad_y` logical px around it, rendered at `scale`.
TextTexture draw_text(wlr_renderer *renderer, const char *text, const char *font, uint32_t fg,
	uint32_t bg, float scale, int pad_x, int pad_y);

// An item card `width` x `height` logical px: `color` fill, a darker strip
// along the top, `text` (nullptr or empty draws only the card) in dark ink
// wrapped to the width and clipped to the card, rendered at `scale`.
TextTexture draw_item_card(wlr_renderer *renderer, const char *text, uint32_t color,
	int width, int height, float scale);

// Uploads a CAIRO_FORMAT_ARGB32 image surface (premultiplied) as a texture.
TexturePtr texture_from_cairo(wlr_renderer *renderer, cairo_surface_t *surface);

void set_source_rgba32(cairo_t *cr, uint32_t rgba);

// A rounded rectangle as the current path; `r` is clamped to half the
// shorter side.
void rounded_path(cairo_t *cr, double x, double y, double w, double h, double r);

using PangoLayoutPtr = Handle<PangoLayout, g_object_unref>;
using FontDescPtr = Handle<PangoFontDescription, pango_font_description_free>;

// A layout of `text` in `font`, ellipsized at `avail` logical px when that
// is positive; every other property is the caller's to set.
PangoLayoutPtr make_layout(cairo_t *cr, const char *text, const char *font, int avail = 0);

// The pixel width `text` takes in `font` within `avail` (see make_layout).
int text_width(cairo_t *cr, const char *text, const char *font, int avail = 0);

// Which point of a painted text sits at the given position: its top left
// corner, the middle of its left edge, or its centre.
enum class TextAnchor { top_left, middle_left, centre };

// Paints `layout` in `rgba` anchored at (x, y); returns its pixel width.
int paint_layout(cairo_t *cr, PangoLayout *layout, uint32_t rgba, double x, double y,
	TextAnchor anchor);

// Paints `text` in `font` and `rgba` anchored at (x, y), ellipsized at
// `avail` logical px when that is positive; returns its pixel width.
int paint_text(cairo_t *cr, const char *text, const char *font, uint32_t rgba, double x,
	double y, TextAnchor anchor, int avail = 0);

// 0xRRGGBBAA with `alpha` multiplied in, premultiplied as wlr_render_color expects.
constexpr wlr_render_color color_rgba(uint32_t rgba, float alpha = 1.0f) {
	const float a = static_cast<float>(rgba & 0xff) / 255.0f * alpha;
	return {
		.r = static_cast<float>((rgba >> 24) & 0xff) / 255.0f * a,
		.g = static_cast<float>((rgba >> 16) & 0xff) / 255.0f * a,
		.b = static_cast<float>((rgba >> 8) & 0xff) / 255.0f * a,
		.a = a,
	};
}
