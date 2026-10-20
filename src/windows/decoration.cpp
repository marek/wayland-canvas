#include <cmath>
#include <pango/pangocairo.h>
#include "core/config.h"
#include "render/draw.h"
#include "core/server.h"
#include "windows/view.h"

namespace {

void fill_rect(cairo_t *cr, double x, double y, double w, double h, uint32_t rgba) {
	set_source_rgba32(cr, rgba);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

// ---- title bar drawing (Windows 11 dark look) ----

// 10x10 logical glyph, centred in the button; 1px lines. Straight lines sit on
// half-logical-pixel coordinates so they land on device pixel rows/columns at
// every integer scale.
void draw_glyph(cairo_t *cr, DecoPart part, double gx, double gy) {
	cairo_set_line_width(cr, 1.0);
	cairo_new_path(cr);
	if (part == DecoPart::minimize) {
		cairo_move_to(cr, gx + 0.5, gy + 9.5);
		cairo_line_to(cr, gx + 9.5, gy + 9.5);
		cairo_stroke(cr);
	} else if (part == DecoPart::maximize) {
		cairo_rectangle(cr, gx + 0.5, gy + 0.5, 9, 9);
		cairo_stroke(cr);
	} else if (part == DecoPart::close) {
		cairo_move_to(cr, gx + 0.5, gy + 0.5);
		cairo_line_to(cr, gx + 9.5, gy + 9.5);
		cairo_move_to(cr, gx + 9.5, gy + 0.5);
		cairo_line_to(cr, gx + 0.5, gy + 9.5);
		cairo_stroke(cr);
	}
}

// Restore glyph: back square offset up-right, front square filled with the
// button's composed background so it occludes the back square.
void draw_restore_glyph(cairo_t *cr, double gx, double gy,
		uint32_t base, uint32_t overlay) {
	cairo_set_line_width(cr, 1.0);
	cairo_new_path(cr);
	cairo_rectangle(cr, gx + 3.5, gy + 0.5, 6, 6);
	cairo_stroke(cr);
	fill_rect(cr, gx + 1.5, gy + 4.5, 5, 5, base);
	if (overlay != 0) {
		fill_rect(cr, gx + 1.5, gy + 4.5, 5, 5, overlay);
	}
	cairo_rectangle(cr, gx + 0.5, gy + 3.5, 6, 6);
	cairo_stroke(cr);
}

void draw_button(cairo_t *cr, const View &view, DecoPart part, int width, bool active) {
	double x = width
		- (static_cast<int>(DecoPart::close) - static_cast<int>(part) + 1)
			* config::button_width;
	double bw = config::button_width;
	double bh = config::titlebar_height;

	bool hover = view.deco.hover == part;
	bool pressed = view.deco.pressed == part;

	const config::Palette &pal = view.server.theme.colors();
	uint32_t base = active ? pal.title_active : pal.title_inactive;
	uint32_t overlay = 0;
	if (part == DecoPart::close) {
		if (pressed) {
			fill_rect(cr, x, 0, bw, bh, pal.close_press);
		} else if (hover) {
			fill_rect(cr, x, 0, bw, bh, pal.close_hover);
		}
	} else if (pressed) {
		overlay = pal.button_press;
		fill_rect(cr, x, 0, bw, bh, overlay);
	} else if (hover) {
		overlay = pal.button_hover;
		fill_rect(cr, x, 0, bw, bh, overlay);
	}

	uint32_t fg = active ? pal.text_active : pal.text_inactive;
	if (part == DecoPart::close && (hover || pressed)) {
		fg = pal.close_text; // white on the attention red in both palettes
	}
	set_source_rgba32(cr, fg);

	double gx = x + (bw - 10) / 2.0;
	double gy = (bh - 10) / 2.0;
	if (part == DecoPart::maximize && view.maximized) {
		draw_restore_glyph(cr, gx, gy, base, overlay);
	} else {
		draw_glyph(cr, part, gx, gy);
	}
}

} // namespace

// ---- Decoration ----

Decoration::Decoration(View &view)
	: view_(view) {
}

Decoration::~Decoration() = default;

// ---- metrics ----

int Decoration::titlebar() const {
	return ssd && !view_.fullscreen ? config::titlebar_height : 0;
}

int Decoration::border() const {
	return ssd && !view_.fullscreen && !view_.maximized ? config::border_width : 0;
}

wlr_texture *Decoration::texture() const {
	return texture_.get();
}

void Decoration::update(float scale) {
	Server &server = view_.server;
	const char *title = view_.shell_title();
	bool active = server.focused_view == &view_;

	if (titlebar() == 0) {
		texture_.reset();
		width_ = 0;
		return;
	}

	wlr_box content = view_.content_box();
	int width = content.width + 2 * border();
	if (width < 1) {
		width = 1;
	}

	bool changed = texture_ == nullptr || width_ != width || scale_ != scale
		|| active_ != active || maximized_ != view_.maximized
		|| hover_ != hover || pressed_ != pressed
		|| (title != nullptr) != title_.has_value()
		|| (title != nullptr && title_.has_value() && *title_ != title);
	if (!changed) {
		return;
	}

	if (title != nullptr) {
		title_ = std::string(title);
	} else {
		title_.reset();
	}

	CairoSurfacePtr surf(cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		static_cast<int>(std::ceil(width * scale)),
		static_cast<int>(std::ceil(config::titlebar_height * scale))));
	CairoPtr cr(cairo_create(surf.get()));
	cairo_scale(cr.get(), scale, scale);
	uint32_t base = active ? server.theme.colors().title_active
		: server.theme.colors().title_inactive;
	fill_rect(cr.get(), 0, 0, width, config::titlebar_height, base);

	draw_button(cr.get(), view_, DecoPart::minimize, width, active);
	draw_button(cr.get(), view_, DecoPart::maximize, width, active);
	draw_button(cr.get(), view_, DecoPart::close, width, active);

	if (title_.has_value()) {
		Handle<PangoLayout, g_object_unref> layout(pango_cairo_create_layout(cr.get()));
		Handle<PangoFontDescription, pango_font_description_free> desc(
			pango_font_description_from_string(config::title_font));
		pango_layout_set_font_description(layout.get(), desc.get());
		pango_layout_set_text(layout.get(), title_->c_str(), -1);
		pango_layout_set_ellipsize(layout.get(), PANGO_ELLIPSIZE_END);
		int avail = width - 2 * config::title_padding - 3 * config::button_width;
		if (avail > 0) {
			pango_layout_set_width(layout.get(), avail * PANGO_SCALE);
		}
		int lw = 0, lh = 0;
		pango_layout_get_pixel_size(layout.get(), &lw, &lh);
		set_source_rgba32(cr.get(), active ? server.theme.colors().text_active
			: server.theme.colors().text_inactive);
		cairo_move_to(cr.get(), config::title_padding,
			(config::titlebar_height - lh) / 2.0);
		pango_cairo_show_layout(cr.get(), layout.get());
	}

	texture_ = texture_from_cairo(server.renderer, surf.get());

	width_ = width;
	scale_ = scale;
	active_ = active;
	maximized_ = view_.maximized;
	hover_ = hover;
	pressed_ = pressed;
}

void Decoration::theme_changed() {
	texture_.reset(); // update() redraws whenever the texture is gone
}

// ---- hit testing (canvas coordinates vs the frame) ----

DecoHit Decoration::hit(double cx, double cy, double zoom) const {
	if (!view_.mapped) {
		return {};
	}
	wlr_box frame = view_.frame_box();
	if (wlr_box_empty(&frame)) {
		return {};
	}
	double fx = frame.x, fy = frame.y;
	double fw = frame.width, fh = frame.height;
	int tb = titlebar();
	int bw = border();
	bool can_resize = !view_.maximized && !view_.fullscreen;
	double margin = config::resize_margin / zoom;
	double corner = 16.0 / zoom;

	// Title bar: buttons right-aligned, resize strip along its top.
	if (tb > 0 && cx >= fx && cx < fx + fw && cy >= fy && cy < fy + tb) {
		if (can_resize && cy < fy + 4) {
			uint32_t edges = WLR_EDGE_TOP;
			if (cx < fx + corner) {
				edges |= WLR_EDGE_LEFT;
			}
			if (cx >= fx + fw - corner) {
				edges |= WLR_EDGE_RIGHT;
			}
			return {DecoPart::resize, edges};
		}
		double right = fx + fw;
		if (cx >= right - config::button_width) {
			return {DecoPart::close, 0};
		}
		if (cx >= right - 2 * config::button_width) {
			return {DecoPart::maximize, 0};
		}
		if (cx >= right - 3 * config::button_width) {
			return {DecoPart::minimize, 0};
		}
		return {DecoPart::title, 0};
	}

	if (can_resize) {
		uint32_t e = 0;
		// Left and right: the border strip inside the frame plus the
		// margin outside it.
		if (cy >= fy - margin && cy <= fy + fh + margin) {
			if (cx >= fx - margin && cx < fx + bw) {
				e |= WLR_EDGE_LEFT;
			}
			if (cx >= fx + fw - bw && cx < fx + fw + margin) {
				e |= WLR_EDGE_RIGHT;
			}
		}
		// Top: the margin above the frame (plus the border strip below the
		// title bar); bottom likewise. The title bar itself was handled above.
		bool top_zone = (cy >= fy - margin && cy < fy)
			|| (tb > 0 ? (cy >= fy + tb && cy < fy + tb + bw)
				: (cy >= fy && cy < fy + bw));
		if (top_zone && cx >= fx - margin && cx <= fx + fw + margin) {
			e |= WLR_EDGE_TOP;
		}
		if (cy >= fy + fh - bw && cy < fy + fh + margin
				&& cx >= fx - margin && cx <= fx + fw + margin) {
			e |= WLR_EDGE_BOTTOM;
		}
		// Outside corners get a wider diagonal grab area.
		if (cx < fx && cy < fy && cx > fx - corner && cy > fy - corner) {
			e |= WLR_EDGE_TOP | WLR_EDGE_LEFT;
		}
		if (cx >= fx + fw && cy < fy && cx < fx + fw + corner && cy > fy - corner) {
			e |= WLR_EDGE_TOP | WLR_EDGE_RIGHT;
		}
		if (cx < fx && cy >= fy + fh && cx > fx - corner && cy < fy + fh + corner) {
			e |= WLR_EDGE_BOTTOM | WLR_EDGE_LEFT;
		}
		if (cx >= fx + fw && cy >= fy + fh && cx < fx + fw + corner
				&& cy < fy + fh + corner) {
			e |= WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT;
		}
		if (e != 0) {
			return {DecoPart::resize, e};
		}
	}

	return {};
}

// ---- mode negotiation ----

// Clients that negotiate neither protocol (GTK4/libadwaita) draw their
// own header bar; adding ours would double the title bar.
void Decoration::refresh_mode() {
	// A shell with no decoration protocol (X11) leaves the window manager
	// to decorate: our own title bar (docs/intents/windows/x11.md).
	bool negotiated = view_.decorated_without_negotiating();
	if (!negotiated) {
		negotiated = xdg_decoration_ != nullptr
			&& xdg_decoration_->current.mode
				== WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
		if (view_.server.decorations.kde_server_mode(view_.surface())) {
			negotiated = true;
		}
	}

	if (negotiated == ssd) {
		return;
	}
	ssd = negotiated;
	if (view_.mapped) {
		view_.damage();
		view_.server.visibility_dirty = true;
	}
}

void Decoration::handle_initial_commit() {
	if (xdg_decoration_ != nullptr && view_.initialized()) {
		wlr_xdg_toplevel_decoration_v1_set_mode(xdg_decoration_,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

void Decoration::attach(wlr_xdg_toplevel_decoration_v1 *decoration) {
	xdg_decoration_ = decoration;
	deco_request_mode_.connect<&Decoration::handle_request_mode>(
		decoration->events.request_mode, this);
	deco_destroy_.connect<&Decoration::handle_deco_destroy>(
		decoration->events.destroy, this);
	if (view_.initialized()) {
		wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

void Decoration::handle_request_mode() {
	if (xdg_decoration_ == nullptr) {
		return;
	}
	// We always decide server-side; the client gets to know once the
	// toplevel is initialized (before that the initial commit tells it).
	if (view_.initialized()) {
		wlr_xdg_toplevel_decoration_v1_set_mode(xdg_decoration_,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	refresh_mode();
}

void Decoration::handle_deco_destroy() {
	deco_request_mode_.disconnect();
	deco_destroy_.disconnect();
	xdg_decoration_ = nullptr;
	if (ssd) {
		ssd = false;
		if (view_.mapped) {
			view_.damage();
			view_.server.visibility_dirty = true;
		}
	}
}

// ---- Decorations ----

// One KDE server-decoration object; refresh_mode() looks the negotiated mode
// up by surface.
struct Decorations::KdeDeco {
	Decorations &owner;
	wlr_server_decoration *decoration;

	KdeDeco(Decorations &owner, wlr_server_decoration *decoration)
		: owner(owner), decoration(decoration) {
		mode_.connect<&Decorations::KdeDeco::handle_mode>(
			decoration->events.mode, this);
		destroy_.connect<&Decorations::KdeDeco::handle_destroy>(
			decoration->events.destroy, this);
	}

	void handle_mode() {
		if (View *view = View::from_surface(decoration->surface)) {
			view->deco.refresh_mode();
		}
	}

	void handle_destroy() {
		destroy_owned(owner.kde_decos_, this);
	}

	Listener mode_, destroy_;
};

Decorations::Decorations(Server &server)
	: server_(server) {
}

Decorations::~Decorations() = default;

bool Decorations::kde_server_mode(wlr_surface *surface) const {
	for (const std::unique_ptr<KdeDeco> &kde : kde_decos_) {
		if (kde->decoration->surface == surface
				&& kde->decoration->mode
					== WLR_SERVER_DECORATION_MANAGER_MODE_SERVER) {
			return true;
		}
	}
	return false;
}

void Decorations::handle_new_xdg_decoration(wlr_xdg_toplevel_decoration_v1 *decoration) {
	View *view = static_cast<View *>(decoration->toplevel->base->data);
	if (view == nullptr) {
		return;
	}
	view->deco.attach(decoration);
}

void Decorations::handle_new_kde_decoration(wlr_server_decoration *decoration) {
	kde_decos_.push_back(std::make_unique<KdeDeco>(*this, decoration));
	if (View *view = View::from_surface(decoration->surface)) {
		view->deco.refresh_mode();
	}
}

void Decorations::init() {
	xdg_decoration_mgr_ = wlr_xdg_decoration_manager_v1_create(server_.display);
	new_xdg_decoration_.connect<&Decorations::handle_new_xdg_decoration>(
		xdg_decoration_mgr_->events.new_toplevel_decoration, this);

	kde_decoration_mgr_ = wlr_server_decoration_manager_create(server_.display);
	wlr_server_decoration_manager_set_default_mode(kde_decoration_mgr_,
		WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	new_kde_decoration_.connect<&Decorations::handle_new_kde_decoration>(
		kde_decoration_mgr_->events.new_decoration, this);
}

void Decorations::finish() {
	new_xdg_decoration_.disconnect();
	new_kde_decoration_.disconnect();
	kde_decos_.clear();
}
