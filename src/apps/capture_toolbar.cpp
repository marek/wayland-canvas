#include "apps/capture_toolbar.h"

#include <linux/input-event-codes.h>

#include <cairo.h>
#include <pango/pangocairo.h>

#include <cmath>
#include <format>
#include <string>

#include "core/config.h"
#include "core/server.h"
#include "core/theme.h"
#include "platform/output.h"
#include "render/draw.h"

namespace {

constexpr double capture_pi = 3.14159265358979323846;

using PangoLayoutPtr = Handle<PangoLayout, g_object_unref>;
using FontDescPtr = Handle<PangoFontDescription, pango_font_description_free>;

void rounded_path(cairo_t *cr, double x, double y, double w, double h, double r) {
	if (r > w / 2.0) {
		r = w / 2.0;
	}
	if (r > h / 2.0) {
		r = h / 2.0;
	}
	cairo_new_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -capture_pi / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, capture_pi / 2);
	cairo_arc(cr, x + r, y + h - r, r, capture_pi / 2, capture_pi);
	cairo_arc(cr, x + r, y + r, r, capture_pi, 3 * capture_pi / 2);
	cairo_close_path(cr);
}

// Text centred on (cx, cy); returns its pixel width.
int paint_centred(cairo_t *cr, const char *text, uint32_t rgba, double cx, double cy) {
	PangoLayoutPtr layout(pango_cairo_create_layout(cr));
	FontDescPtr desc(pango_font_description_from_string(config::ui_font));
	pango_layout_set_font_description(layout.get(), desc.get());
	pango_layout_set_text(layout.get(), text, -1);
	int w = 0, h = 0;
	pango_layout_get_pixel_size(layout.get(), &w, &h);
	set_source_rgba32(cr, rgba);
	cairo_move_to(cr, cx - w / 2.0, cy - h / 2.0);
	pango_cairo_show_layout(cr, layout.get());
	return w;
}

// One mode button's glyph — a screen, a window, the picker's dashed
// rectangle — centred on (cx, cy).
void paint_glyph(cairo_t *cr, ToolbarTarget target, uint32_t rgba, double cx, double cy) {
	cairo_set_line_width(cr, 1.5);
	set_source_rgba32(cr, rgba);
	if (target == ToolbarTarget::screen) { // a screen on its stand
		rounded_path(cr, cx - 14, cy - 11, 28, 19, 3);
		cairo_stroke(cr);
		cairo_set_line_width(cr, 3);
		cairo_move_to(cr, cx - 5, cy + 11);
		cairo_line_to(cr, cx + 5, cy + 11);
		cairo_stroke(cr);
		return;
	}
	if (target == ToolbarTarget::window) { // a window with its title strip
		rounded_path(cr, cx - 12, cy - 10, 24, 19, 3);
		cairo_stroke(cr);
		rounded_path(cr, cx - 9, cy - 7, 18, 4, 1.5);
		cairo_fill(cr);
		return;
	}
	// the region the picker drags: a dashed rectangle
	double dashes[2] = {4, 3};
	cairo_set_dash(cr, dashes, 2, 0);
	rounded_path(cr, cx - 13, cy - 10, 26, 19, 3);
	cairo_stroke(cr);
	cairo_set_dash(cr, nullptr, 0, 0);
}

const char *target_name(ToolbarTarget target) {
	if (target == ToolbarTarget::screen) {
		return "Screen";
	}
	return target == ToolbarTarget::window ? "Window" : "Region";
}

std::string option_text(ToolbarOption option, const CaptureOptions &options) {
	if (option == ToolbarOption::destination) {
		return options.clipboard ? "Copy to clipboard" : "Save to Screenshots";
	}
	if (option == ToolbarOption::timer) {
		return options.timer_s == 0 ? std::string{"Timer: off"}
			: std::format("Timer: {} s", options.timer_s);
	}
	return options.show_pointer ? "Pointer: on" : "Pointer: off";
}

bool option_on(ToolbarOption option, const CaptureOptions &options) {
	if (option == ToolbarOption::destination) {
		return options.clipboard;
	}
	return option == ToolbarOption::timer ? options.timer_s > 0 : options.show_pointer;
}

} // namespace

CaptureToolbar::CaptureToolbar(Server &server) : server_(server) {
}

void CaptureToolbar::open(Output &output, bool recording_focus, const CaptureOptions &preset) {
	bool same = output_ == &output;
	output_ = &output;
	record_focus_ = recording_focus && !same;
	if (preset.clipboard) {
		options_.clipboard = true; // the Ctrl chord presets the toggle
	}
	hover_.reset();
	pressed_.reset();
	stale_ = true;
	wlr_seat_pointer_notify_clear_focus(server_.seat);
	output.damage();
}

void CaptureToolbar::close() {
	if (output_ == nullptr) {
		return;
	}
	Output *output = output_;
	output_ = nullptr;
	record_focus_ = false;
	hover_.reset();
	pressed_.reset();
	texture_.reset();
	stale_ = true;
	output->damage();
}

std::optional<ToolbarButton> CaptureToolbar::button_at(const Output &output, double ox,
		double oy) const {
	return toolbar_hit(toolbar_layout(output.width, output.height), ox, oy);
}

bool CaptureToolbar::pointer_motion(Output &output, double ox, double oy) {
	if (output_ != &output) {
		return false;
	}
	record_focus_ = false; // the accent was only the toolbar's opening cue
	std::optional<ToolbarButton> hit = button_at(output, ox, oy);
	if (hit != hover_) {
		hover_ = hit;
		stale_ = true;
		output.damage();
	}
	return true;
}

bool CaptureToolbar::pointer_button(Output &output, double ox, double oy, uint32_t button,
		bool pressed, Chose chose) {
	if (output_ != &output) {
		return false;
	}
	if (button != BTN_LEFT) {
		return true;
	}
	if (pressed) {
		record_focus_ = false;
		pressed_ = button_at(output, ox, oy);
		if (pressed_) {
			stale_ = true;
			output.damage();
		} else {
			close(); // a click outside cancels
		}
		return true;
	}
	std::optional<ToolbarButton> hit = button_at(output, ox, oy);
	if (hit && hit == pressed_) {
		activate(*hit, chose);
	}
	if (pressed_) {
		pressed_.reset();
		stale_ = true;
		if (output_ != nullptr) {
			output_->damage();
		}
	}
	return true;
}

// Runs one toolbar button: a mode closes the toolbar and is reported, an
// option turns.
void CaptureToolbar::activate(ToolbarButton button, Chose chose) {
	if (const auto *option = std::get_if<ToolbarOption>(&button)) {
		if (*option == ToolbarOption::destination) {
			options_.clipboard = !options_.clipboard;
		} else if (*option == ToolbarOption::timer) { // none → 5 s → 10 s → none
			options_.timer_s = options_.timer_s == 0
				? config::capture_timer_steps[1]
				: (options_.timer_s == config::capture_timer_steps[1]
					? config::capture_timer_steps[2] : 0);
		} else {
			options_.show_pointer = !options_.show_pointer;
		}
		stale_ = true;
		if (output_ != nullptr) {
			output_->damage();
		}
		return;
	}
	Output &output = *output_;
	CaptureOptions options = options_;
	close();
	chose(std::get<ToolbarMode>(button), output, options);
}

void CaptureToolbar::add_overlay(Output &output, float scale, CaptureOverlay &out) {
	if (output_ != &output) {
		return;
	}
	ToolbarLayout layout = toolbar_layout(output.width, output.height);
	if (texture_ == nullptr || stale_ || scale_ != scale || !wlr_box_equal(&box_, &layout.box)) {
		texture_.reset();
		texture_ = draw(output, layout);
		scale_ = scale;
		box_ = layout.box;
		stale_ = false;
	}
	if (texture_ == nullptr) {
		return;
	}
	out.images.push_back({.texture = texture_.get(), .box = layout.box});
	if (record_focus_) {
		// Alt+Print opened this: the recording choices carry the accent.
		out.outlines.push_back({.box = layout.records,
			.rgba = server_.theme.colors().accent,
			.thickness = config::capture_pick_outline});
	}
}

// The toolbar panel: its options row, the six mode buttons, the footer.
TexturePtr CaptureToolbar::draw(const Output &output, const ToolbarLayout &layout) {
	const config::Palette &pal = server_.theme.colors();
	float scale = output.wlr->scale;
	int dev_w = static_cast<int>(std::ceil(layout.box.width * scale));
	int dev_h = static_cast<int>(std::ceil(layout.box.height * scale));
	CairoSurfacePtr surf{cairo_image_surface_create(CAIRO_FORMAT_ARGB32, dev_w, dev_h)};
	if (surf == nullptr) {
		return nullptr;
	}
	CairoPtr cr{cairo_create(surf.get())};
	cairo_scale(cr.get(), scale, scale);
	cairo_set_antialias(cr.get(), CAIRO_ANTIALIAS_GOOD);

	// The panel, in the layout's own coordinates.
	double px = -layout.box.x, py = -layout.box.y; // panel-local origin
	rounded_path(cr.get(), px, py, layout.box.width, layout.box.height, 12);
	set_source_rgba32(cr.get(), pal.launcher);
	cairo_fill_preserve(cr.get());
	set_source_rgba32(cr.get(), pal.launcher_border);
	cairo_set_line_width(cr.get(), 1);
	cairo_stroke(cr.get());

	for (std::size_t i = 0; i < toolbar_options.size(); i++) {
		ToolbarOption option = toolbar_options[i];
		const wlr_box &box = layout.options[i];
		bool on = option_on(option, options_);
		rounded_path(cr.get(), px + box.x, py + box.y, box.width, box.height, 8);
		set_source_rgba32(cr.get(), on ? pal.launcher_selected : pal.launcher_search);
		cairo_fill(cr.get());
		paint_centred(cr.get(), option_text(option, options_).c_str(),
			on ? pal.accent_text : pal.launcher_dim_text,
			px + box.x + box.width / 2.0, py + box.y + box.height / 2.0);
	}

	for (std::size_t i = 0; i < toolbar_modes.size(); i++) {
		const ToolbarMode &mode = toolbar_modes[i];
		const wlr_box &box = layout.modes[i];
		bool lit = hover_ == ToolbarButton{mode} || pressed_ == ToolbarButton{mode};
		rounded_path(cr.get(), px + box.x, py + box.y, box.width, box.height, 8);
		set_source_rgba32(cr.get(), lit ? pal.launcher_hover : pal.launcher_search);
		cairo_fill(cr.get());
		double cx = px + box.x + box.width / 2.0;
		double cy = py + box.y + box.height / 2.0 - 7;
		paint_glyph(cr.get(), mode.target, pal.launcher_text, cx, cy);
		if (mode.record) { // the recording mark: a red dot on the glyph's corner
			cairo_set_line_width(cr.get(), 1.5);
			cairo_arc(cr.get(), cx + 15, cy - 9, 4, 0, 2 * capture_pi);
			set_source_rgba32(cr.get(), pal.close_hover);
			cairo_fill(cr.get());
		}
		paint_centred(cr.get(), target_name(mode.target), pal.launcher_dim_text, cx,
			py + box.y + box.height - 11);
	}

	paint_centred(cr.get(), "Esc closes", pal.launcher_dim_text,
		px + layout.box.width / 2.0,
		py + layout.box.height - config::capture_toolbar_pad / 2.0);

	return texture_from_cairo(server_.renderer, surf.get());
}
