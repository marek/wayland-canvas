#include <cairo.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "config.h"
#include "server.h"

/* Windows-11-style Start/search panel: state lives in struct cv_launcher hung
 * off server->launcher, allocated on first open and freed in launcher_finish. */

#define LAUNCHER_PI 3.14159265358979323846
#define LAUNCHER_QUERY_MAX 255

struct cv_launcher {
	bool open;
	struct cv_output *output;

	char query[LAUNCHER_QUERY_MAX + 1];
	size_t query_len;

	struct cv_apps apps;
	size_t *results; /* indices into apps.items, apps.count entries */
	size_t nresults;

	size_t selected; /* index into results */
	size_t scroll; /* first visible row */
	int hover; /* result index under the pointer, -1 none */
	int pressed_row; /* result index of the pending click, -1 none */

	struct wlr_texture *texture;
	float scale; /* scale the texture was drawn for */
	struct wlr_box box; /* layout it was drawn for */
	bool stale;
};

/* ---- geometry (output-local logical px) ---- */

static int panel_rows(int output_height) {
	int chrome = 12 + LAUNCHER_SEARCH_HEIGHT + 8 + 8 + 28;
	int rows = (output_height - 64 - chrome) / LAUNCHER_ROW_HEIGHT;
	if (rows < 1) {
		rows = 1;
	}
	if (rows > LAUNCHER_MAX_ROWS) {
		rows = LAUNCHER_MAX_ROWS;
	}
	return rows;
}

static struct wlr_box panel_box(const struct cv_output *output, int *rows) {
	int width = LAUNCHER_WIDTH;
	int avail = output->width - 32;
	if (width > avail) {
		width = avail > 1 ? avail : 1;
	}
	int r = panel_rows(output->height);
	int height = 12 + LAUNCHER_SEARCH_HEIGHT + 8 + r * LAUNCHER_ROW_HEIGHT + 8 + 28;
	struct wlr_box box = {
		.x = (output->width - width) / 2,
		.y = (output->height - height) / 2,
		.width = width,
		.height = height,
	};
	if (rows != NULL) {
		*rows = r;
	}
	return box;
}

static int row_at(const struct cv_launcher *l, const struct wlr_box *box,
	int rows, double ox, double oy) {
	double rel = oy - (box->y + 12 + LAUNCHER_SEARCH_HEIGHT + 8);
	if (rel < 0) {
		return -1;
	}
	int vis = (int)(rel / LAUNCHER_ROW_HEIGHT);
	if (vis >= rows) {
		return -1;
	}
	size_t idx = l->scroll + (size_t)vis;
	if (idx >= l->nresults) {
		return -1;
	}
	return (int)idx;
}

/* ---- cairo/pango helpers ---- */

static void set_source_rgba32(cairo_t *cr, uint32_t rgba) {
	cairo_set_source_rgba(cr,
		((rgba >> 24) & 0xff) / 255.0,
		((rgba >> 16) & 0xff) / 255.0,
		((rgba >> 8) & 0xff) / 255.0,
		(rgba & 0xff) / 255.0);
}

static void rounded_path(cairo_t *cr, double x, double y, double w, double h,
	double r) {
	if (r > w / 2.0) {
		r = w / 2.0;
	}
	if (r > h / 2.0) {
		r = h / 2.0;
	}
	cairo_new_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -LAUNCHER_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, LAUNCHER_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, LAUNCHER_PI / 2, LAUNCHER_PI);
	cairo_arc(cr, x + r, y + r, r, LAUNCHER_PI, 3 * LAUNCHER_PI / 2);
	cairo_close_path(cr);
}

static PangoLayout *make_layout(cairo_t *cr, const char *text, const char *font,
	int avail) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text, -1);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	if (avail > 0) {
		pango_layout_set_width(layout, avail * PANGO_SCALE);
	}
	return layout;
}

static int text_width(cairo_t *cr, const char *text, const char *font, int avail) {
	PangoLayout *layout = make_layout(cr, text, font, avail);
	int w = 0;
	pango_layout_get_pixel_size(layout, &w, NULL);
	g_object_unref(layout);
	return w;
}

/* Draws `text` left-aligned at x, vertically centred on cy; returns its
 * (possibly ellipsized) pixel width. */
static int paint_text(cairo_t *cr, const char *text, const char *font,
	uint32_t rgba, double x, double cy, int avail) {
	PangoLayout *layout = make_layout(cr, text, font, avail);
	int w = 0, h = 0;
	pango_layout_get_pixel_size(layout, &w, &h);
	set_source_rgba32(cr, rgba);
	cairo_move_to(cr, x, cy - h / 2.0);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
	return w;
}

static uint32_t name_hash(const char *s) {
	uint32_t h = 5381;
	for (; s != NULL && *s != '\0'; s++) {
		h = h * 33u + (uint8_t)*s;
	}
	return h;
}

static void set_source_hsl(cairo_t *cr, double hue, double sat, double light) {
	double c = (1.0 - fabs(2.0 * light - 1.0)) * sat;
	double hp = hue / 60.0;
	int sector = (int)hp;
	if (sector < 0) {
		sector = 0;
	}
	if (sector > 5) {
		sector = 5;
	}
	double x = c * (1.0 - fabs(fmod(hp, 2.0) - 1.0));
	double r = 0, g = 0, b = 0;
	switch (sector) {
	case 0: r = c; g = x; break;
	case 1: r = x; g = c; break;
	case 2: g = c; b = x; break;
	case 3: g = x; b = c; break;
	case 4: r = x; b = c; break;
	default: r = c; b = x; break;
	}
	double m = light - c / 2.0;
	cairo_set_source_rgb(cr, r + m, g + m, b + m);
}

/* Copies the first UTF-8 character of `s` ("" when empty) into out. */
static void first_letter(const char *s, char out[5]) {
	if (s == NULL || s[0] == '\0') {
		out[0] = '\0';
		return;
	}
	size_t n = 1;
	while (n < 4 && ((unsigned char)s[n] & 0xc0) == 0x80) {
		n++;
	}
	memcpy(out, s, n);
	out[n] = '\0';
}

static size_t utf8_encode(uint32_t cp, char buf[4]) {
	if (cp < 0x80) {
		buf[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		buf[0] = (char)(0xc0 | (cp >> 6));
		buf[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		buf[0] = (char)(0xe0 | (cp >> 12));
		buf[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		buf[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	if (cp < 0x110000) {
		buf[0] = (char)(0xf0 | (cp >> 18));
		buf[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
		buf[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
		buf[3] = (char)(0x80 | (cp & 0x3f));
		return 4;
	}
	return 0;
}

/* ---- query editing ---- */

static bool query_pop_char(struct cv_launcher *l) {
	if (l->query_len == 0) {
		return false;
	}
	size_t end = l->query_len;
	while (end > 0 && ((unsigned char)l->query[end - 1] & 0xc0) == 0x80) {
		end--;
	}
	if (end > 0) {
		end--;
	}
	l->query_len = end;
	l->query[end] = '\0';
	return true;
}

static bool query_pop_word(struct cv_launcher *l) {
	size_t end = l->query_len;
	while (end > 0 && l->query[end - 1] == ' ') {
		end--;
	}
	while (end > 0 && l->query[end - 1] != ' ') {
		end--;
	}
	if (end == l->query_len) {
		return false;
	}
	l->query_len = end;
	l->query[end] = '\0';
	return true;
}

static void query_changed(struct cv_launcher *l) {
	l->nresults = apps_search(&l->apps, l->query, l->results, l->apps.count);
	l->selected = 0;
	l->scroll = 0;
	l->stale = true;
	output_damage(l->output);
}

/* ---- selection ---- */

static void scroll_to_selection(struct cv_launcher *l, int rows) {
	if (l->selected < l->scroll) {
		l->scroll = l->selected;
	} else if (l->selected >= l->scroll + (size_t)rows) {
		l->scroll = l->selected - (size_t)rows + 1;
		size_t max_scroll =
			l->nresults > (size_t)rows ? l->nresults - (size_t)rows : 0;
		if (l->scroll > max_scroll) {
			l->scroll = max_scroll;
		}
	}
}

static void move_selection(struct cv_launcher *l, int rows, int delta) {
	if (l->nresults == 0 || delta == 0) {
		return;
	}
	long sel = (long)l->selected + delta;
	if (sel < 0) {
		sel = 0;
	}
	if ((size_t)sel >= l->nresults) {
		sel = (long)l->nresults - 1;
	}
	if ((size_t)sel == l->selected) {
		return;
	}
	l->selected = (size_t)sel;
	scroll_to_selection(l, rows);
	l->stale = true;
	output_damage(l->output);
}

/* ---- drawing ---- */

static void draw_icon(cairo_t *cr, const struct cv_app *app, double x, double y,
	float scale) {
	int idev = (int)(LAUNCHER_ICON * scale + 0.5f);
	cairo_surface_t *icon = app->icon != NULL ? icon_lookup(app->icon, idev) : NULL;
	if (icon != NULL) {
		int dw = cairo_image_surface_get_width(icon);
		int dh = cairo_image_surface_get_height(icon);
		if (dw > 0 && dh > 0) {
			/* The cr is scaled by `scale`; counter-scale so the icon's
			 * device pixels map 1:1, centred in the logical square. */
			cairo_save(cr);
			cairo_translate(cr, x, y);
			cairo_scale(cr, 1.0 / scale, 1.0 / scale);
			cairo_set_source_surface(cr, icon,
				(LAUNCHER_ICON * scale - dw) / 2.0,
				(LAUNCHER_ICON * scale - dh) / 2.0);
			cairo_paint(cr);
			cairo_restore(cr);
			return;
		}
	}

	/* Fallback: rounded square in a colour derived from the name hash. */
	const char *name = app->name != NULL ? app->name : "";
	rounded_path(cr, x, y, LAUNCHER_ICON, LAUNCHER_ICON, 6);
	set_source_hsl(cr, name_hash(name) % 360, 0.55, 0.45);
	cairo_fill(cr);
	char letter[5];
	first_letter(name, letter);
	if (letter[0] != '\0') {
		int lw = text_width(cr, letter, LAUNCHER_FONT, -1);
		paint_text(cr, letter, LAUNCHER_FONT, COLOR_LAUNCHER_TEXT,
			x + (LAUNCHER_ICON - lw) / 2.0, y + LAUNCHER_ICON / 2.0, -1);
	}
}

static struct wlr_texture *draw_panel(struct cv_server *server,
	const struct cv_launcher *l, const struct cv_output *output,
	const struct wlr_box *box, int rows) {
	float scale = output->wlr_output->scale;
	int dev_w = (int)ceil(box->width * scale);
	int dev_h = (int)ceil(box->height * scale);
	if (dev_w < 1) {
		dev_w = 1;
	}
	if (dev_h < 1) {
		dev_h = 1;
	}
	cairo_surface_t *surf =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, dev_w, dev_h);
	cairo_t *cr = cairo_create(surf);
	cairo_scale(cr, scale, scale);
	double w = box->width, h = box->height;

	/* Panel: dark rounded card with a 1 px border. */
	rounded_path(cr, 0, 0, w, h, 8);
	set_source_rgba32(cr, COLOR_LAUNCHER);
	cairo_fill(cr);
	rounded_path(cr, 0.5, 0.5, w - 1, h - 1, 7.5);
	cairo_set_source_rgb(cr, 0x3a / 255.0, 0x3a / 255.0, 0x3a / 255.0);
	cairo_set_line_width(cr, 1.0);
	cairo_stroke(cr);

	/* Search field with magnifier glyph. */
	double fx = 12, fy = 12, fw = w - 24, fh = LAUNCHER_SEARCH_HEIGHT;
	rounded_path(cr, fx, fy, fw, fh, 6);
	set_source_rgba32(cr, COLOR_LAUNCHER_SEARCH);
	cairo_fill(cr);
	double gx = fx + 24, gy = fy + fh / 2 - 2, gr = 6;
	set_source_rgba32(cr, COLOR_LAUNCHER_DIM_TEXT);
	cairo_set_line_width(cr, 1.5);
	cairo_new_path(cr);
	cairo_arc(cr, gx, gy, gr, 0, 2 * LAUNCHER_PI);
	cairo_stroke(cr);
	double gd = gr * 0.70710678;
	cairo_move_to(cr, gx + gd, gy + gd);
	cairo_line_to(cr, gx + gd + 5, gy + gd + 5);
	cairo_stroke(cr);

	double tx = fx + 44;
	int tavail = fw > 60 ? (int)(fw - 44 - 14) : 10;
	double tcy = fy + fh / 2;
	if (l->query_len > 0) {
		int qw = paint_text(cr, l->query, LAUNCHER_SEARCH_FONT,
			COLOR_LAUNCHER_TEXT, tx, tcy, tavail);
		double cx = floor(tx + qw + 4) + 0.5;
		set_source_rgba32(cr, COLOR_LAUNCHER_TEXT);
		cairo_set_line_width(cr, 1.0);
		cairo_move_to(cr, cx, tcy - 11);
		cairo_line_to(cr, cx, tcy + 11);
		cairo_stroke(cr);
	} else {
		paint_text(cr, "Type to search apps", LAUNCHER_SEARCH_FONT,
			COLOR_LAUNCHER_DIM_TEXT, tx, tcy, tavail);
	}

	double rows_y = 12 + LAUNCHER_SEARCH_HEIGHT + 8;
	if (l->nresults == 0) {
		char msg[300];
		snprintf(msg, sizeof msg, "No apps match \xE2\x80\x9C%s\xE2\x80\x9D",
			l->query);
		int avail = (int)w - 48;
		int mw = text_width(cr, msg, LAUNCHER_FONT, avail);
		paint_text(cr, msg, LAUNCHER_FONT, COLOR_LAUNCHER_DIM_TEXT,
			(w - mw) / 2.0, rows_y + rows * LAUNCHER_ROW_HEIGHT / 2.0, avail);
	} else {
		for (int i = 0; i < rows; i++) {
			size_t idx = l->scroll + (size_t)i;
			if (idx >= l->nresults) {
				break;
			}
			const struct cv_app *app = &l->apps.items[l->results[idx]];
			double ry = rows_y + i * LAUNCHER_ROW_HEIGHT;
			double rw = w - 24;
			if (idx == l->selected) {
				rounded_path(cr, 12, ry, rw, LAUNCHER_ROW_HEIGHT, 4);
				set_source_rgba32(cr, COLOR_LAUNCHER_SELECTED);
				cairo_fill(cr);
			} else if (l->hover >= 0 && (size_t)l->hover == idx) {
				rounded_path(cr, 12, ry, rw, LAUNCHER_ROW_HEIGHT, 4);
				set_source_rgba32(cr, COLOR_LAUNCHER_HOVER);
				cairo_fill(cr);
			}

			draw_icon(cr, app, 24,
				ry + (LAUNCHER_ROW_HEIGHT - LAUNCHER_ICON) / 2.0, scale);

			double nx = 24 + LAUNCHER_ICON + 10;
			int navail = (int)w - 12 - 8 - (int)nx;
			double ncy = ry + LAUNCHER_ROW_HEIGHT / 2.0;
			int nw = 0;
			if (app->name != NULL) {
				nw = paint_text(cr, app->name, LAUNCHER_FONT,
					COLOR_LAUNCHER_TEXT, nx, ncy, navail);
			}
			if (app->generic != NULL) {
				int rem = navail - nw - 8;
				if (rem >= 32) {
					paint_text(cr, app->generic, LAUNCHER_FONT,
						COLOR_LAUNCHER_DIM_TEXT, nx + nw + 8, ncy, rem);
				}
			}
		}

		/* Thin scrollbar when the results don't fit. */
		if (l->nresults > (size_t)rows) {
			double th = rows * (double)LAUNCHER_ROW_HEIGHT;
			double thumb = th * rows / (double)l->nresults;
			if (thumb < 24) {
				thumb = 24;
			}
			if (thumb > th) {
				thumb = th;
			}
			size_t max_scroll = l->nresults - (size_t)rows;
			double frac = (double)l->scroll / (double)max_scroll;
			double ty = rows_y + frac * (th - thumb);
			rounded_path(cr, w - 16, rows_y, 4, th, 2);
			set_source_rgba32(cr, COLOR_LAUNCHER_HOVER);
			cairo_fill(cr);
			rounded_path(cr, w - 16, ty, 4, thumb, 2);
			set_source_rgba32(cr, COLOR_LAUNCHER_DIM_TEXT);
			cairo_fill(cr);
		}
	}

	/* Footer. */
	double fcy = h - 14;
	paint_text(cr, "\xE2\x86\x91\xE2\x86\x93 select   Enter open   Esc close",
		UI_FONT, COLOR_LAUNCHER_DIM_TEXT, 20, fcy, (int)w - 40);
	char count[32];
	snprintf(count, sizeof count, "%zu apps", l->nresults);
	int cw = text_width(cr, count, UI_FONT, -1);
	paint_text(cr, count, UI_FONT, COLOR_LAUNCHER_DIM_TEXT, w - 20 - cw, fcy, -1);

	cairo_destroy(cr);
	struct wlr_texture *texture = texture_from_cairo(server->renderer, surf);
	cairo_surface_destroy(surf);
	return texture;
}

/* ---- lifecycle ---- */

static void launcher_open(struct cv_server *server, struct cv_output *output) {
	struct cv_launcher *l = server->launcher;
	if (l == NULL) {
		l = calloc(1, sizeof *l);
		if (l == NULL) {
			return;
		}
		server->launcher = l;
	}
	if (l->open) {
		launcher_close(server);
	}
	l->open = true;
	l->output = output;
	l->query[0] = '\0';
	l->query_len = 0;
	apps_load(&l->apps);
	l->results = l->apps.count > 0
		? malloc(l->apps.count * sizeof *l->results) : NULL;
	l->nresults = apps_search(&l->apps, l->query, l->results, l->apps.count);
	l->selected = 0;
	l->scroll = 0;
	l->hover = -1;
	l->pressed_row = -1;
	l->stale = true;
	output_damage(output);
}

void launcher_toggle(struct cv_server *server, struct cv_output *output) {
	struct cv_launcher *l = server->launcher;
	if (l != NULL && l->open) {
		launcher_close(server);
		return;
	}
	if (output != NULL) {
		launcher_open(server, output);
	}
}

void launcher_close(struct cv_server *server) {
	struct cv_launcher *l = server->launcher;
	if (l == NULL || !l->open) {
		return;
	}
	struct cv_output *output = l->output;
	l->open = false;
	l->output = NULL;
	apps_free(&l->apps);
	memset(&l->apps, 0, sizeof l->apps);
	free(l->results);
	l->results = NULL;
	l->nresults = 0;
	l->selected = 0;
	l->scroll = 0;
	l->hover = -1;
	l->pressed_row = -1;
	if (l->texture != NULL) {
		wlr_texture_destroy(l->texture);
		l->texture = NULL;
	}
	l->stale = true;
	if (output != NULL) {
		output_damage(output);
	}
}

bool launcher_is_open(struct cv_server *server) {
	return server->launcher != NULL && server->launcher->open;
}

void launcher_output_destroyed(struct cv_server *server, struct cv_output *output) {
	struct cv_launcher *l = server->launcher;
	if (l != NULL && l->open && l->output == output) {
		launcher_close(server);
	}
}

void launcher_finish(struct cv_server *server) {
	struct cv_launcher *l = server->launcher;
	if (l == NULL) {
		return;
	}
	launcher_close(server);
	free(l);
	server->launcher = NULL;
}

/* ---- keyboard ---- */

void launcher_key(struct cv_server *server, xkb_keysym_t sym, uint32_t utf32,
	uint32_t mods) {
	struct cv_launcher *l = server->launcher;
	if (l == NULL || !l->open) {
		return;
	}
	bool ctrl = (mods & WLR_MODIFIER_CTRL) != 0;
	int rows = panel_rows(l->output->height);

	if (sym == XKB_KEY_Escape) {
		launcher_close(server);
		return;
	}
	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		if (l->selected < l->nresults) {
			app_launch(&l->apps.items[l->results[l->selected]]);
			launcher_close(server);
		}
		return;
	}
	if (ctrl && (sym == XKB_KEY_u || sym == XKB_KEY_U)) {
		if (l->query_len > 0) {
			l->query_len = 0;
			l->query[0] = '\0';
			query_changed(l);
		}
		return;
	}
	if (ctrl && (sym == XKB_KEY_w || sym == XKB_KEY_W
			|| sym == XKB_KEY_BackSpace)) {
		if (query_pop_word(l)) {
			query_changed(l);
		}
		return;
	}
	if (ctrl && (sym == XKB_KEY_p || sym == XKB_KEY_P)) {
		move_selection(l, rows, -1);
		return;
	}
	if (ctrl && (sym == XKB_KEY_n || sym == XKB_KEY_N)) {
		move_selection(l, rows, 1);
		return;
	}
	if (sym == XKB_KEY_Up || sym == XKB_KEY_ISO_Left_Tab) {
		move_selection(l, rows, -1);
		return;
	}
	if (sym == XKB_KEY_Down || sym == XKB_KEY_Tab) {
		move_selection(l, rows, 1);
		return;
	}
	if (sym == XKB_KEY_Page_Up) {
		move_selection(l, rows, -rows);
		return;
	}
	if (sym == XKB_KEY_Page_Down) {
		move_selection(l, rows, rows);
		return;
	}
	if (sym == XKB_KEY_Home || sym == XKB_KEY_End) {
		/* Only meaningful while browsing the unfiltered list. */
		if (l->query_len == 0 && l->nresults > 0) {
			l->selected = sym == XKB_KEY_Home ? 0 : l->nresults - 1;
			size_t max_scroll =
				l->nresults > (size_t)rows ? l->nresults - (size_t)rows : 0;
			l->scroll = sym == XKB_KEY_Home ? 0 : max_scroll;
			l->stale = true;
			output_damage(l->output);
		}
		return;
	}
	if (sym == XKB_KEY_BackSpace) {
		if (query_pop_char(l)) {
			query_changed(l);
		}
		return;
	}
	if (utf32 >= 0x20 && utf32 != 0x7f
			&& !(mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO))) {
		char buf[4];
		size_t n = utf8_encode(utf32, buf);
		if (n > 0 && l->query_len + n <= LAUNCHER_QUERY_MAX) {
			memcpy(l->query + l->query_len, buf, n);
			l->query_len += n;
			l->query[l->query_len] = '\0';
			query_changed(l);
		}
	}
}

/* ---- pointer ---- */

bool launcher_pointer_motion(struct cv_server *server, struct cv_output *output,
	double ox, double oy) {
	struct cv_launcher *l = server->launcher;
	if (l == NULL || !l->open || l->output != output) {
		return false;
	}
	int rows;
	struct wlr_box box = panel_box(output, &rows);
	if (!wlr_box_contains_point(&box, ox, oy)) {
		if (l->hover != -1) {
			l->hover = -1;
			l->stale = true;
			output_damage(output);
		}
		return false;
	}
	int row = row_at(l, &box, rows, ox, oy);
	if (row != l->hover) {
		l->hover = row;
		l->stale = true;
		output_damage(output);
	}
	return true;
}

bool launcher_pointer_button(struct cv_server *server, struct cv_output *output,
	double ox, double oy, uint32_t button, bool pressed) {
	(void)button;
	struct cv_launcher *l = server->launcher;
	if (l == NULL || !l->open || l->output != output) {
		return false;
	}
	int rows;
	struct wlr_box box = panel_box(output, &rows);
	bool inside = wlr_box_contains_point(&box, ox, oy);
	if (pressed) {
		if (inside) {
			l->pressed_row = row_at(l, &box, rows, ox, oy);
			return true;
		}
		launcher_close(server);
		return true;
	}
	int row = inside ? row_at(l, &box, rows, ox, oy) : -1;
	if (row >= 0 && row == l->pressed_row) {
		l->selected = (size_t)row;
		app_launch(&l->apps.items[l->results[l->selected]]);
		launcher_close(server);
		return true;
	}
	l->pressed_row = -1;
	return true;
}

bool launcher_pointer_axis(struct cv_server *server, struct cv_output *output,
	double ox, double oy, double delta) {
	struct cv_launcher *l = server->launcher;
	if (l == NULL || !l->open || l->output != output) {
		return false;
	}
	int rows;
	struct wlr_box box = panel_box(output, &rows);
	if (!wlr_box_contains_point(&box, ox, oy)) {
		return false;
	}
	if (delta != 0 && l->nresults > (size_t)rows) {
		size_t max_scroll = l->nresults - (size_t)rows;
		long next = (long)l->scroll + (delta > 0 ? 1 : -1);
		if (next < 0) {
			next = 0;
		}
		if ((size_t)next > max_scroll) {
			next = (long)max_scroll;
		}
		if ((size_t)next != l->scroll) {
			l->scroll = (size_t)next;
			l->stale = true;
			output_damage(output);
		}
	}
	return true;
}

/* ---- rendering ---- */

struct wlr_texture *launcher_texture(struct cv_server *server,
	struct cv_output *output, struct wlr_box *box) {
	struct cv_launcher *l = server->launcher;
	if (l == NULL || !l->open || l->output != output) {
		return NULL;
	}
	int rows;
	struct wlr_box b = panel_box(output, &rows);
	float scale = output->wlr_output->scale;
	if (l->texture == NULL || l->stale || l->scale != scale
			|| memcmp(&l->box, &b, sizeof b) != 0) {
		if (l->texture != NULL) {
			wlr_texture_destroy(l->texture);
		}
		l->texture = draw_panel(server, l, output, &b, rows);
		l->scale = scale;
		l->box = b;
		l->stale = false;
	}
	*box = b;
	return l->texture;
}
