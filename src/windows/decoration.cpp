#include <cmath>
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

void draw_button(cairo_t *cr, const View &view, DecoPart part, const wlr_box &box, bool active) {
	double x = box.x;
	double bw = box.width;
	double bh = box.height;

	bool hover = view.deco.hover() == part;
	bool pressed = view.deco.pressed() == part;

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
	if (part == DecoPart::maximize && view.mode == WindowMode::maximized) {
		draw_restore_glyph(cr, gx, gy, base, overlay);
	} else {
		draw_glyph(cr, part, gx, gy);
	}
}

} // namespace

// ---- Decoration ----

Decoration::Decoration(View &view)
	: view_(view), bar_(view.server.text_cache) {
}

Decoration::~Decoration() = default;

// ---- metrics ----

int Decoration::titlebar() const {
	return ssd && view_.mode != WindowMode::fullscreen ? config::titlebar_height : 0;
}

int Decoration::border() const {
	return ssd && view_.mode == WindowMode::normal ? config::border_width : 0;
}

bool Decoration::set_hover(DecoPart part) {
	return change_part(hover_, part);
}

bool Decoration::set_pressed(DecoPart part) {
	return change_part(pressed_, part);
}

bool Decoration::change_part(DecoPart &field, DecoPart part) {
	if (field == part) {
		return false;
	}
	field = part;
	view_.damage();
	return true;
}

wlr_texture *Decoration::title_bar(float scale) {
	if (titlebar() == 0) {
		bar_.drop();
		return nullptr;
	}
	const char *title = view_.shell_title();
	wlr_box content = view_.content_box();
	wanted_.width = std::max(content.width + 2 * border(), 1);
	wanted_.active = view_.server.focused_view == &view_;
	wanted_.maximized = view_.mode == WindowMode::maximized;
	wanted_.hover = hover_;
	wanted_.pressed = pressed_;
	if (wanted_.title != (title != nullptr ? title : "")) {
		wanted_.title.assign(title != nullptr ? title : "");
	}
	return bar_.get(scale, wanted_, [&] { return draw_bar(scale); });
}

TexturePtr Decoration::draw_bar(float scale) const {
	Server &server = view_.server;
	int width = wanted_.width;
	CairoSurfacePtr surf(cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		static_cast<int>(std::ceil(width * scale)),
		static_cast<int>(std::ceil(config::titlebar_height * scale))));
	CairoPtr cr(cairo_create(surf.get()));
	cairo_scale(cr.get(), scale, scale);
	uint32_t base = wanted_.active ? server.theme.colors().title_active
		: server.theme.colors().title_inactive;
	fill_rect(cr.get(), 0, 0, width, config::titlebar_height, base);

	for (std::size_t i = 0; i < deco_buttons.size(); i++) {
		draw_button(cr.get(), view_, deco_buttons[i], button_box(i, width), wanted_.active);
	}

	if (!wanted_.title.empty()) {
		int avail = button_box(0, width).x - 2 * config::title_padding;
		paint_text(cr.get(), wanted_.title.c_str(), config::title_font,
			wanted_.active ? server.theme.colors().text_active
				: server.theme.colors().text_inactive,
			config::title_padding, config::titlebar_height / 2.0, TextAnchor::middle_left,
			avail);
	}

	return texture_from_cairo(server.renderer, surf.get());
}

// ---- hit testing (canvas coordinates vs the frame) ----

namespace {

// Where a coordinate falls on one axis of a window frame.
struct EdgeBand {
	bool start;       // in the strip along the low edge (left, top)
	bool end;         // in the strip along the high edge (right, bottom)
	bool alongside;   // within the frame's extent, margin included

	// The edges the coordinate is on, given the edge each end stands for.
	uint32_t edges(uint32_t start_edge, uint32_t end_edge) const {
		return (start ? start_edge : 0) | (end ? end_edge : 0);
	}
};

// The strips of one axis: `outside` the frame's edge and `inside` it, the low
// edge's inner strip beginning `lead` in from the edge (below the title bar).
// Every interval is half-open, [low, high).
EdgeBand edge_band(double c, double frame_start, double frame_length,
		double outside, double inside, double lead) {
	auto within = [c](double low, double high) { return c >= low && c < high; };
	double frame_end = frame_start + frame_length;
	return {
		.start = within(frame_start - outside, frame_start)
			|| within(frame_start + lead, frame_start + lead + inside),
		.end = within(frame_end - inside, frame_end + outside),
		.alongside = within(frame_start - outside, frame_end + outside),
	};
}

} // namespace

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
	bool can_resize = view_.mode == WindowMode::normal;
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
		for (std::size_t i = 0; i < deco_buttons.size(); i++) {
			wlr_box button = button_box(i, frame.width);
			if (cx >= fx + button.x && cx < fx + button.x + button.width) {
				return {deco_buttons[i], 0};
			}
		}
		return {DecoPart::title, 0};
	}

	if (can_resize) {
		EdgeBand strip_x = edge_band(cx, fx, fw, margin, bw, 0);
		EdgeBand strip_y = edge_band(cy, fy, fh, margin, bw, tb);
		uint32_t e = 0;
		// A strip along one edge counts while the point is alongside the
		// frame on the other axis. The title bar itself was handled above.
		if (strip_y.alongside) {
			e |= strip_x.edges(WLR_EDGE_LEFT, WLR_EDGE_RIGHT);
		}
		if (strip_x.alongside) {
			e |= strip_y.edges(WLR_EDGE_TOP, WLR_EDGE_BOTTOM);
		}
		// Outside corners get a wider diagonal grab area.
		uint32_t corner_h = edge_band(cx, fx, fw, corner, 0, 0).edges(WLR_EDGE_LEFT, WLR_EDGE_RIGHT);
		uint32_t corner_v = edge_band(cy, fy, fh, corner, 0, 0).edges(WLR_EDGE_TOP, WLR_EDGE_BOTTOM);
		if (corner_h != 0 && corner_v != 0) {
			e |= corner_h | corner_v;
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
		view_.visibility_changed();
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
			view_.visibility_changed();
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
