#pragma once

#include <cstdint>
#include <cairo.h>
#include "util.h"

struct TextTexture {
	TexturePtr texture; // nullptr for a null text or an empty result
	int width = 0, height = 0; // logical px, padding included
};

// `text` (nullptr draws nothing) in `font`, fg on bg (0xRRGGBBAA; a bg with zero alpha
// stays transparent), `pad_x`/`pad_y` logical px around it, rendered at `scale`.
TextTexture draw_text(wlr_renderer *renderer, const char *text, const char *font, uint32_t fg,
	uint32_t bg, float scale, int pad_x, int pad_y);

// Uploads a CAIRO_FORMAT_ARGB32 image surface (premultiplied) as a texture.
TexturePtr texture_from_cairo(wlr_renderer *renderer, cairo_surface_t *surface);

void set_source_rgba32(cairo_t *cr, uint32_t rgba);

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
