#include <math.h>
#include <drm_fourcc.h>
#include <pango/pangocairo.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_texture.h>
#include "server.h"

static void set_source_rgba32(cairo_t *cr, uint32_t rgba) {
	cairo_set_source_rgba(cr,
		((rgba >> 24) & 0xff) / 255.0,
		((rgba >> 16) & 0xff) / 255.0,
		((rgba >> 8) & 0xff) / 255.0,
		(rgba & 0xff) / 255.0);
}

static PangoLayout *make_layout(cairo_t *cr, const char *text, const char *font) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text, -1);
	pango_layout_set_single_paragraph_mode(layout, true);
	return layout;
}

struct wlr_texture *draw_text(struct wlr_renderer *renderer, const char *text,
		const char *font, uint32_t fg, uint32_t bg, float scale, int pad_x, int pad_y,
		int *width, int *height) {
	*width = 0;
	*height = 0;
	if (text == NULL) {
		return NULL;
	}

	/* Measure on a scratch context: the surface needs its final size up front. */
	cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = cairo_create(scratch);
	PangoLayout *layout = make_layout(cr, text, font);
	int lw = 0, lh = 0;
	pango_layout_get_pixel_size(layout, &lw, &lh);
	g_object_unref(layout);
	cairo_destroy(cr);
	cairo_surface_destroy(scratch);

	int w = lw + 2 * pad_x;
	int h = lh + 2 * pad_y;
	if (w <= 0 || h <= 0) {
		return NULL;
	}
	*width = w;
	*height = h;

	cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		(int)ceil(w * scale), (int)ceil(h * scale));
	cr = cairo_create(surf);
	cairo_scale(cr, scale, scale);

	if ((bg & 0xff) != 0) {
		set_source_rgba32(cr, bg);
		cairo_paint(cr);
	}

	layout = make_layout(cr, text, font);
	set_source_rgba32(cr, fg);
	cairo_move_to(cr, pad_x, pad_y);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
	cairo_destroy(cr);

	struct wlr_texture *texture = texture_from_cairo(renderer, surf);
	cairo_surface_destroy(surf);
	return texture;
}

struct wlr_texture *texture_from_cairo(struct wlr_renderer *renderer,
		cairo_surface_t *surface) {
	cairo_surface_flush(surface);
	return wlr_texture_from_pixels(renderer, DRM_FORMAT_ARGB8888,
		(uint32_t)cairo_image_surface_get_stride(surface),
		(uint32_t)cairo_image_surface_get_width(surface),
		(uint32_t)cairo_image_surface_get_height(surface),
		cairo_image_surface_get_data(surface));
}

struct wlr_render_color color_rgba(uint32_t rgba, float alpha) {
	float a = (rgba & 0xff) / 255.0f * alpha;
	return (struct wlr_render_color){
		.r = ((rgba >> 24) & 0xff) / 255.0f * a,
		.g = ((rgba >> 16) & 0xff) / 255.0f * a,
		.b = ((rgba >> 8) & 0xff) / 255.0f * a,
		.a = a,
	};
}
