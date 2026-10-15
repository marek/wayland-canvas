#include <cairo.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "core/config.h"
#include "render/draw.h"
#include "platform/output.h"
#include "core/server.h"

namespace {

constexpr double launcher_pi = 3.14159265358979323846;
constexpr std::size_t query_max = 255;

using PangoLayoutPtr = Handle<PangoLayout, g_object_unref>;
using FontDescPtr = Handle<PangoFontDescription, pango_font_description_free>;

// ---- geometry (output-local logical px) ----

int panel_rows(int output_height) {
	int chrome = 12 + config::launcher_search_height + 8 + 8 + 28;
	int rows = (output_height - 64 - chrome) / config::launcher_row_height;
	if (rows < 1) {
		rows = 1;
	}
	if (rows > config::launcher_max_rows) {
		rows = config::launcher_max_rows;
	}
	return rows;
}

struct PanelLayout {
	wlr_box box{};
	int rows = 0;
};

PanelLayout panel_layout(const Output &output) {
	int width = config::launcher_width;
	int avail = output.width - 32;
	if (width > avail) {
		width = avail > 1 ? avail : 1;
	}
	int rows = panel_rows(output.height);
	int height = 12 + config::launcher_search_height + 8
		+ rows * config::launcher_row_height + 8 + 28;
	return {
		.box = {
			.x = (output.width - width) / 2,
			.y = (output.height - height) / 2,
			.width = width,
			.height = height,
		},
		.rows = rows,
	};
}

bool box_eq(const wlr_box &a, const wlr_box &b) {
	return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

// ---- cairo/pango helpers ----

void rounded_path(cairo_t *cr, double x, double y, double w, double h, double r) {
	if (r > w / 2.0) {
		r = w / 2.0;
	}
	if (r > h / 2.0) {
		r = h / 2.0;
	}
	cairo_new_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -launcher_pi / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, launcher_pi / 2);
	cairo_arc(cr, x + r, y + h - r, r, launcher_pi / 2, launcher_pi);
	cairo_arc(cr, x + r, y + r, r, launcher_pi, 3 * launcher_pi / 2);
	cairo_close_path(cr);
}

PangoLayoutPtr make_layout(cairo_t *cr, const char *text, const char *font, int avail) {
	PangoLayoutPtr layout(pango_cairo_create_layout(cr));
	FontDescPtr desc(pango_font_description_from_string(font));
	pango_layout_set_font_description(layout.get(), desc.get());
	pango_layout_set_text(layout.get(), text, -1);
	pango_layout_set_ellipsize(layout.get(), PANGO_ELLIPSIZE_END);
	if (avail > 0) {
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

// Draws `text` left-aligned at x, vertically centred on cy; returns its
// (possibly ellipsized) pixel width.
int paint_text(cairo_t *cr, const char *text, const char *font,
	uint32_t rgba, double x, double cy, int avail) {
	PangoLayoutPtr layout = make_layout(cr, text, font, avail);
	int w = 0, h = 0;
	pango_layout_get_pixel_size(layout.get(), &w, &h);
	set_source_rgba32(cr, rgba);
	cairo_move_to(cr, x, cy - h / 2.0);
	pango_cairo_show_layout(cr, layout.get());
	return w;
}

uint32_t name_hash(const char *s) {
	uint32_t h = 5381;
	for (; s != nullptr && *s != '\0'; s++) {
		h = h * 33u + static_cast<uint8_t>(*s);
	}
	return h;
}

void set_source_hsl(cairo_t *cr, double hue, double sat, double light) {
	double c = (1.0 - std::fabs(2.0 * light - 1.0)) * sat;
	double hp = hue / 60.0;
	int sector = static_cast<int>(hp);
	if (sector < 0) {
		sector = 0;
	}
	if (sector > 5) {
		sector = 5;
	}
	double x = c * (1.0 - std::fabs(std::fmod(hp, 2.0) - 1.0));
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

// Copies the first UTF-8 character of `s` ("" when empty) into out.
void first_letter(const char *s, char out[5]) {
	if (s == nullptr || s[0] == '\0') {
		out[0] = '\0';
		return;
	}
	std::size_t n = 1;
	while (n < 4 && (static_cast<unsigned char>(s[n]) & 0xc0) == 0x80) {
		n++;
	}
	std::copy_n(s, n, out);
	out[n] = '\0';
}

std::size_t utf8_encode(uint32_t cp, char buf[4]) {
	if (cp < 0x80) {
		buf[0] = static_cast<char>(cp);
		return 1;
	}
	if (cp < 0x800) {
		buf[0] = static_cast<char>(0xc0 | (cp >> 6));
		buf[1] = static_cast<char>(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		buf[0] = static_cast<char>(0xe0 | (cp >> 12));
		buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
		buf[2] = static_cast<char>(0x80 | (cp & 0x3f));
		return 3;
	}
	if (cp < 0x110000) {
		buf[0] = static_cast<char>(0xf0 | (cp >> 18));
		buf[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
		buf[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
		buf[3] = static_cast<char>(0x80 | (cp & 0x3f));
		return 4;
	}
	return 0;
}

// ---- drawing ----

void draw_icon(cairo_t *cr, Icons &icons, const App &app, double x, double y,
	float scale, uint32_t text_color) {
	int idev = static_cast<int>(config::launcher_icon * scale + 0.5f);
	cairo_surface_t *icon = !app.icon.empty() ? icons.lookup(app.icon, idev) : nullptr;
	if (icon != nullptr) {
		int dw = cairo_image_surface_get_width(icon);
		int dh = cairo_image_surface_get_height(icon);
		if (dw > 0 && dh > 0) {
			// The cr is scaled by `scale`; counter-scale so the icon's
			// device pixels map 1:1, centred in the logical square.
			cairo_save(cr);
			cairo_translate(cr, x, y);
			cairo_scale(cr, 1.0 / scale, 1.0 / scale);
			cairo_set_source_surface(cr, icon,
				(config::launcher_icon * scale - dw) / 2.0,
				(config::launcher_icon * scale - dh) / 2.0);
			cairo_paint(cr);
			cairo_restore(cr);
			return;
		}
	}

	// Fallback: rounded square in a colour derived from the name hash.
	const char *name = app.name.c_str();
	rounded_path(cr, x, y, config::launcher_icon, config::launcher_icon, 6);
	set_source_hsl(cr, name_hash(name) % 360, 0.55, 0.45);
	cairo_fill(cr);
	char letter[5];
	first_letter(name, letter);
	if (letter[0] != '\0') {
		int lw = text_width(cr, letter, config::launcher_font, -1);
		paint_text(cr, letter, config::launcher_font, text_color,
			x + (config::launcher_icon - lw) / 2.0,
			y + config::launcher_icon / 2.0, -1);
	}
}

} // namespace

Launcher::Launcher(Server &server)
	: server_(server) {
}

Launcher::~Launcher() {
	finish();
}

// ---- query editing ----

bool Launcher::query_pop_char() {
	if (query_.empty()) {
		return false;
	}
	std::size_t end = query_.size();
	while (end > 0 && (static_cast<unsigned char>(query_[end - 1]) & 0xc0) == 0x80) {
		end--;
	}
	if (end > 0) {
		end--;
	}
	query_.resize(end);
	return true;
}

bool Launcher::query_pop_word() {
	std::size_t end = query_.size();
	while (end > 0 && query_[end - 1] == ' ') {
		end--;
	}
	while (end > 0 && query_[end - 1] != ' ') {
		end--;
	}
	if (end == query_.size()) {
		return false;
	}
	query_.resize(end);
	return true;
}

void Launcher::query_changed() {
	results_ = apps_.search(query_);
	selected_ = 0;
	scroll_ = 0;
	stale_ = true;
	output_->damage();
}

// ---- selection ----

void Launcher::scroll_to_selection(int rows) {
	if (selected_ < scroll_) {
		scroll_ = selected_;
	} else if (selected_ >= scroll_ + static_cast<std::size_t>(rows)) {
		scroll_ = selected_ - static_cast<std::size_t>(rows) + 1;
		std::size_t max_scroll = results_.size() > static_cast<std::size_t>(rows)
			? results_.size() - static_cast<std::size_t>(rows) : 0;
		if (scroll_ > max_scroll) {
			scroll_ = max_scroll;
		}
	}
}

void Launcher::move_selection(int rows, int delta) {
	if (results_.empty() || delta == 0) {
		return;
	}
	long sel = static_cast<long>(selected_) + delta;
	if (sel < 0) {
		sel = 0;
	}
	if (static_cast<std::size_t>(sel) >= results_.size()) {
		sel = static_cast<long>(results_.size()) - 1;
	}
	if (static_cast<std::size_t>(sel) == selected_) {
		return;
	}
	selected_ = static_cast<std::size_t>(sel);
	scroll_to_selection(rows);
	stale_ = true;
	output_->damage();
}

int Launcher::row_at(const wlr_box &box, int rows, double ox, double oy) const {
	double rel = oy - (box.y + 12 + config::launcher_search_height + 8);
	if (rel < 0) {
		return -1;
	}
	int vis = static_cast<int>(rel / config::launcher_row_height);
	if (vis >= rows) {
		return -1;
	}
	std::size_t idx = scroll_ + static_cast<std::size_t>(vis);
	if (idx >= results_.size()) {
		return -1;
	}
	return static_cast<int>(idx);
}

TexturePtr Launcher::draw_panel(const Output &output, const wlr_box &box, int rows) {
	float scale = output.wlr->scale;
	int dev_w = static_cast<int>(std::ceil(box.width * scale));
	int dev_h = static_cast<int>(std::ceil(box.height * scale));
	if (dev_w < 1) {
		dev_w = 1;
	}
	if (dev_h < 1) {
		dev_h = 1;
	}
	CairoSurfacePtr surf(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, dev_w, dev_h));
	CairoPtr cr_owner(cairo_create(surf.get()));
	cairo_t *cr = cr_owner.get();
	cairo_scale(cr, scale, scale);
	double w = box.width, h = box.height;
	const config::Palette &pal = server_.theme.colors();

	// Panel: rounded card with a 1 px border.
	rounded_path(cr, 0, 0, w, h, 8);
	set_source_rgba32(cr, pal.launcher);
	cairo_fill(cr);
	rounded_path(cr, 0.5, 0.5, w - 1, h - 1, 7.5);
	set_source_rgba32(cr, pal.launcher_border);
	cairo_set_line_width(cr, 1.0);
	cairo_stroke(cr);

	// Search field with magnifier glyph.
	double fx = 12, fy = 12, fw = w - 24, fh = config::launcher_search_height;
	rounded_path(cr, fx, fy, fw, fh, 6);
	set_source_rgba32(cr, pal.launcher_search);
	cairo_fill(cr);
	double gx = fx + 24, gy = fy + fh / 2 - 2, gr = 6;
	set_source_rgba32(cr, pal.launcher_dim_text);
	cairo_set_line_width(cr, 1.5);
	cairo_new_path(cr);
	cairo_arc(cr, gx, gy, gr, 0, 2 * launcher_pi);
	cairo_stroke(cr);
	double gd = gr * 0.70710678;
	cairo_move_to(cr, gx + gd, gy + gd);
	cairo_line_to(cr, gx + gd + 5, gy + gd + 5);
	cairo_stroke(cr);

	double tx = fx + 44;
	int tavail = fw > 60 ? static_cast<int>(fw - 44 - 14) : 10;
	double tcy = fy + fh / 2;
	if (!query_.empty()) {
		int qw = paint_text(cr, query_.c_str(), config::launcher_search_font,
			pal.launcher_text, tx, tcy, tavail);
		double cx = std::floor(tx + qw + 4) + 0.5;
		set_source_rgba32(cr, pal.launcher_text);
		cairo_set_line_width(cr, 1.0);
		cairo_move_to(cr, cx, tcy - 11);
		cairo_line_to(cr, cx, tcy + 11);
		cairo_stroke(cr);
	} else {
		paint_text(cr, "Type to search apps", config::launcher_search_font,
			pal.launcher_dim_text, tx, tcy, tavail);
	}

	double rows_y = 12 + config::launcher_search_height + 8;
	if (results_.empty()) {
		std::string msg = std::format("No apps match \xE2\x80\x9C{}\xE2\x80\x9D", query_);
		int avail = static_cast<int>(w) - 48;
		int mw = text_width(cr, msg.c_str(), config::launcher_font, avail);
		paint_text(cr, msg.c_str(), config::launcher_font, pal.launcher_dim_text,
			(w - mw) / 2.0, rows_y + rows * config::launcher_row_height / 2.0, avail);
	} else {
		for (int i = 0; i < rows; i++) {
			std::size_t idx = scroll_ + static_cast<std::size_t>(i);
			if (idx >= results_.size()) {
				break;
			}
			const App &app = apps_.items()[results_[idx]];
			double ry = rows_y + i * config::launcher_row_height;
			double rw = w - 24;
			if (idx == selected_) {
				rounded_path(cr, 12, ry, rw, config::launcher_row_height, 4);
				set_source_rgba32(cr, pal.launcher_selected);
				cairo_fill(cr);
			} else if (hover_ >= 0 && static_cast<std::size_t>(hover_) == idx) {
				rounded_path(cr, 12, ry, rw, config::launcher_row_height, 4);
				set_source_rgba32(cr, pal.launcher_hover);
				cairo_fill(cr);
			}

			draw_icon(cr, icons_, app, 24,
				ry + (config::launcher_row_height - config::launcher_icon) / 2.0,
				scale, pal.launcher_text);

			double nx = 24 + config::launcher_icon + 10;
			int navail = static_cast<int>(w) - 12 - 8 - static_cast<int>(nx);
			double ncy = ry + config::launcher_row_height / 2.0;
			int nw = 0;
			if (!app.name.empty()) {
				nw = paint_text(cr, app.name.c_str(), config::launcher_font,
					pal.launcher_text, nx, ncy, navail);
			}
			if (!app.generic.empty()) {
				int rem = navail - nw - 8;
				if (rem >= 32) {
					paint_text(cr, app.generic.c_str(), config::launcher_font,
						pal.launcher_dim_text, nx + nw + 8, ncy, rem);
				}
			}
		}

		// Thin scrollbar when the results don't fit.
		if (results_.size() > static_cast<std::size_t>(rows)) {
			double th = rows * static_cast<double>(config::launcher_row_height);
			double thumb = th * rows / static_cast<double>(results_.size());
			if (thumb < 24) {
				thumb = 24;
			}
			if (thumb > th) {
				thumb = th;
			}
			std::size_t max_scroll = results_.size() - static_cast<std::size_t>(rows);
			double frac = static_cast<double>(scroll_) / static_cast<double>(max_scroll);
			double ty = rows_y + frac * (th - thumb);
			rounded_path(cr, w - 16, rows_y, 4, th, 2);
			set_source_rgba32(cr, pal.launcher_hover);
			cairo_fill(cr);
			rounded_path(cr, w - 16, ty, 4, thumb, 2);
			set_source_rgba32(cr, pal.launcher_dim_text);
			cairo_fill(cr);
		}
	}

	// Footer.
	double fcy = h - 14;
	paint_text(cr, "\xE2\x86\x91\xE2\x86\x93 select   Enter open   Esc close",
		config::ui_font, pal.launcher_dim_text, 20, fcy,
		static_cast<int>(w) - 40);
	std::string count = std::format("{} app{}", results_.size(),
		results_.size() == 1 ? "" : "s");
	int cw = text_width(cr, count.c_str(), config::ui_font, -1);
	paint_text(cr, count.c_str(), config::ui_font, pal.launcher_dim_text,
		w - 20 - cw, fcy, -1);

	return texture_from_cairo(server_.renderer, surf.get());
}

// ---- lifecycle ----

void Launcher::open(Output &output) {
	if (open_) {
		close();
	}
	open_ = true;
	output_ = &output;
	query_.clear();
	apps_.load();
	results_ = apps_.search(query_);
	selected_ = 0;
	scroll_ = 0;
	hover_ = -1;
	pressed_row_ = -1;
	stale_ = true;
	output.damage();
}

void Launcher::toggle(Output *output) {
	if (open_) {
		close();
		return;
	}
	if (output != nullptr) {
		open(*output);
	}
}

void Launcher::close() {
	if (!open_) {
		return;
	}
	Output *output = output_;
	open_ = false;
	output_ = nullptr;
	apps_.clear();
	results_.clear();
	selected_ = 0;
	scroll_ = 0;
	hover_ = -1;
	pressed_row_ = -1;
	texture_.reset();
	stale_ = true;
	if (output != nullptr) {
		output->damage();
	}
}

bool Launcher::is_open() const {
	return open_;
}

void Launcher::output_destroyed(Output &output) {
	if (open_ && output_ == &output) {
		close();
	}
}

void Launcher::theme_changed() {
	stale_ = true; // panel() redraws on the next frame
}

void Launcher::finish() {
	close();
	icons_.clear();
}

// ---- keyboard ----

void Launcher::key(xkb_keysym_t sym, uint32_t utf32, uint32_t mods) {
	if (!open_) {
		return;
	}
	bool ctrl = (mods & WLR_MODIFIER_CTRL) != 0;
	int rows = panel_rows(output_->height);

	if (sym == XKB_KEY_Escape) {
		close();
		return;
	}
	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		if (selected_ < results_.size()) {
			launch(apps_.items()[results_[selected_]]);
			close();
		}
		return;
	}
	if (ctrl && (sym == XKB_KEY_u || sym == XKB_KEY_U)) {
		if (!query_.empty()) {
			query_.clear();
			query_changed();
		}
		return;
	}
	if (ctrl && (sym == XKB_KEY_w || sym == XKB_KEY_W
			|| sym == XKB_KEY_BackSpace)) {
		if (query_pop_word()) {
			query_changed();
		}
		return;
	}
	if (ctrl && (sym == XKB_KEY_p || sym == XKB_KEY_P)) {
		move_selection(rows, -1);
		return;
	}
	if (ctrl && (sym == XKB_KEY_n || sym == XKB_KEY_N)) {
		move_selection(rows, 1);
		return;
	}
	if (sym == XKB_KEY_Up || sym == XKB_KEY_ISO_Left_Tab) {
		move_selection(rows, -1);
		return;
	}
	if (sym == XKB_KEY_Down || sym == XKB_KEY_Tab) {
		move_selection(rows, 1);
		return;
	}
	if (sym == XKB_KEY_Page_Up) {
		move_selection(rows, -rows);
		return;
	}
	if (sym == XKB_KEY_Page_Down) {
		move_selection(rows, rows);
		return;
	}
	if (sym == XKB_KEY_Home || sym == XKB_KEY_End) {
		// Only meaningful while browsing the unfiltered list.
		if (query_.empty() && !results_.empty()) {
			selected_ = sym == XKB_KEY_Home ? 0 : results_.size() - 1;
			std::size_t max_scroll = results_.size() > static_cast<std::size_t>(rows)
				? results_.size() - static_cast<std::size_t>(rows) : 0;
			scroll_ = sym == XKB_KEY_Home ? 0 : max_scroll;
			stale_ = true;
			output_->damage();
		}
		return;
	}
	if (sym == XKB_KEY_BackSpace) {
		if (query_pop_char()) {
			query_changed();
		}
		return;
	}
	if (utf32 >= 0x20 && utf32 != 0x7f
			&& !(mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO))) {
		char buf[4];
		std::size_t n = utf8_encode(utf32, buf);
		if (n > 0 && query_.size() + n <= query_max) {
			query_.append(buf, n);
			query_changed();
		}
	}
}

// ---- pointer ----

bool Launcher::pointer_motion(Output &output, double ox, double oy) {
	if (!open_ || output_ != &output) {
		return false;
	}
	PanelLayout pl = panel_layout(output);
	if (!wlr_box_contains_point(&pl.box, ox, oy)) {
		if (hover_ != -1) {
			hover_ = -1;
			stale_ = true;
			output.damage();
		}
		return false;
	}
	int row = row_at(pl.box, pl.rows, ox, oy);
	if (row != hover_) {
		hover_ = row;
		stale_ = true;
		output.damage();
	}
	return true;
}

bool Launcher::pointer_button(Output &output, double ox, double oy, uint32_t button,
	bool pressed) {
	(void)button;
	if (!open_ || output_ != &output) {
		return false;
	}
	PanelLayout pl = panel_layout(output);
	bool inside = wlr_box_contains_point(&pl.box, ox, oy);
	if (pressed) {
		if (inside) {
			pressed_row_ = row_at(pl.box, pl.rows, ox, oy);
			return true;
		}
		close();
		return true;
	}
	int row = inside ? row_at(pl.box, pl.rows, ox, oy) : -1;
	if (row >= 0 && row == pressed_row_) {
		selected_ = static_cast<std::size_t>(row);
		launch(apps_.items()[results_[selected_]]);
		close();
		return true;
	}
	pressed_row_ = -1;
	return true;
}

bool Launcher::pointer_axis(Output &output, double ox, double oy, double delta) {
	if (!open_ || output_ != &output) {
		return false;
	}
	PanelLayout pl = panel_layout(output);
	if (!wlr_box_contains_point(&pl.box, ox, oy)) {
		return false;
	}
	if (delta != 0 && results_.size() > static_cast<std::size_t>(pl.rows)) {
		std::size_t max_scroll = results_.size() - static_cast<std::size_t>(pl.rows);
		long next = static_cast<long>(scroll_) + (delta > 0 ? 1 : -1);
		if (next < 0) {
			next = 0;
		}
		if (static_cast<std::size_t>(next) > max_scroll) {
			next = static_cast<long>(max_scroll);
		}
		if (static_cast<std::size_t>(next) != scroll_) {
			scroll_ = static_cast<std::size_t>(next);
			stale_ = true;
			output.damage();
		}
	}
	return true;
}

// ---- rendering ----

std::optional<Launcher::Panel> Launcher::panel(Output &output) {
	if (!open_ || output_ != &output) {
		return std::nullopt;
	}
	PanelLayout pl = panel_layout(output);
	float scale = output.wlr->scale;
	if (!texture_ || stale_ || texture_scale_ != scale || !box_eq(texture_box_, pl.box)) {
		texture_ = draw_panel(output, pl.box, pl.rows);
		texture_scale_ = scale;
		texture_box_ = pl.box;
		stale_ = false;
	}
	if (!texture_) {
		return std::nullopt;
	}
	return Panel{.texture = texture_.get(), .box = pl.box};
}
