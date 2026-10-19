// capture_ui.cpp: the picker overlay, the capture toolbar, the after-shot
// thumbnail, and the recorder's life cycle. Shots are taken through the
// shared offscreen captures (render/capture.cpp); a recording is those
// same captures piped frame by frame into ffmpeg — nested and from a TTY
// alike, with no capture protocol of any recorder's own in the way.

#include <fcntl.h>
#include <linux/input-event-codes.h>

#include <cairo.h>
#include <glib.h>
#include <pango/pangocairo.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <array>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "apps/capture_ui.h"
#include "core/config.h"
#include "core/theme.h"
#include "core/wlr.h"
#include "platform/output.h"
#include "render/capture.h"
#include "render/draw.h"
#include "core/server.h"

// wlroots' own initializer for compositor-owned data sources: it lives in
// the private types/ header but is an exported symbol, and there is no
// public constructor for a source the compositor itself offers.
void wlr_data_source_init(wlr_data_source *source, const wlr_data_source_impl *impl);

namespace {

constexpr double capture_pi = 3.14159265358979323846;

using PangoLayoutPtr = Handle<PangoLayout, g_object_unref>;
using FontDescPtr = Handle<PangoFontDescription, pango_font_description_free>;

// ---- files and names ----

std::string home_dir() {
	const char *home = std::getenv("HOME");
	return home != nullptr && *home != '\0' ? std::string(home) : std::string{"."};
}

// The XDG user dir, from user-dirs.dirs through glib, with a ~/ fallback.
std::string user_dir(GUserDirectory which, const char *fallback) {
	const char *dir = g_get_user_special_dir(which);
	if (dir != nullptr && *dir != '\0') {
		return dir;
	}
	return home_dir() + "/" + fallback;
}

void mkdir_p(const std::string &path) {
	std::string built;
	for (std::size_t i = 0; i < path.size(); i++) {
		built += path[i];
		if (path[i] == '/' || i + 1 == path.size()) {
			mkdir(built.c_str(), 0755); // EEXIST is fine; the write reports the rest
		}
	}
}

// The local-time name part every shot and recording carries.
std::string timestamp() {
	std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_r(&now, &local);
	char buf[40];
	std::strftime(buf, sizeof buf, "%Y-%m-%d at %H.%M.%S", &local);
	return buf;
}

// Single-quotes `text` for /bin/sh.
std::string shell_quote(const std::string &text) {
	std::string out = "'";
	for (const char ch : text) {
		out += ch == '\'' ? "'\\''" : std::string(1, ch);
	}
	return out + "'";
}

// A JSON string literal, for splicing names into a pick answer.
std::string quote_json(const std::string &text) {
	std::string out = "\"";
	for (const char ch : text) {
		switch (ch) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (static_cast<unsigned char>(ch) < 0x20) {
				out += std::format("\\u{:04x}", static_cast<unsigned char>(ch));
			} else {
				out += ch;
			}
		}
	}
	return out + "\"";
}

cairo_status_t png_collect(void *closure, const unsigned char *data, unsigned length) {
	auto *out = static_cast<std::vector<std::uint8_t> *>(closure);
	out->insert(out->end(), data, data + length);
	return CAIRO_STATUS_SUCCESS;
}

// ---- geometry (output-local logical px) ----

wlr_box normalized_box(double x0, double y0, double x1, double y1) {
	return {
		static_cast<int>(std::round(std::fmin(x0, x1))),
		static_cast<int>(std::round(std::fmin(y0, y1))),
		static_cast<int>(std::round(std::fabs(x1 - x0))),
		static_cast<int>(std::round(std::fabs(y1 - y0))),
	};
}

bool box_contains(const wlr_box &box, double x, double y) {
	return x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height;
}

// The view's frame as an output-local logical box, whatever the zoom.
wlr_box frame_on_output(const Output &output, const View &view) {
	Rect frame = view.frame_rect();
	Point corner = output.canvas_to_local(frame.x, frame.y);
	double zoom = output.camera.drawn().zoom;
	return {
		static_cast<int>(std::round(corner.x)),
		static_cast<int>(std::round(corner.y)),
		static_cast<int>(std::round(frame.w * zoom)),
		static_cast<int>(std::round(frame.h * zoom)),
	};
}

// ---- cairo helpers ----

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
void paint_glyph(cairo_t *cr, int target, uint32_t rgba, double cx, double cy) {
	cairo_set_line_width(cr, 1.5);
	set_source_rgba32(cr, rgba);
	if (target == 0) { // a screen on its stand
		rounded_path(cr, cx - 14, cy - 11, 28, 19, 3);
		cairo_stroke(cr);
		cairo_set_line_width(cr, 3);
		cairo_move_to(cr, cx - 5, cy + 11);
		cairo_line_to(cr, cx + 5, cy + 11);
		cairo_stroke(cr);
		return;
	}
	if (target == 1) { // a window with its title strip
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

} // namespace

// ------------------------------------------------------- the clipboard

// The shot's PNG on the seat's selection: one wlr_data_source offering
// image/png, pumped to the reading client without ever blocking the loop.
struct CaptureUi::ClipSource {
	wlr_data_source base{};
	CaptureUi *owner = nullptr;
	std::vector<std::uint8_t> bytes;
	std::size_t sent = 0;
	int fd = -1;
	EventSourcePtr pump;
	Listener destroyed_;

	static void send(wlr_data_source *source, const char *mime_type, int32_t fd);
	static void destroy(wlr_data_source *source);
	static int pump_write(int fd, uint32_t mask, void *data);

	void start(int32_t fd) {
		if (this->fd >= 0) {
			close(this->fd); // one transfer at a time: the newest request wins
			pump.reset();
		}
		this->fd = fd;
		sent = 0;
		long flags = fcntl(fd, F_GETFL);
		if (flags >= 0) {
			fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		}
		pump.reset(wl_event_loop_add_fd(owner->server_.loop, fd, WL_EVENT_WRITABLE,
			pump_write, this));
	}

	void stop() {
		pump.reset();
		if (fd >= 0) {
			close(fd);
			fd = -1;
		}
	}
};

void CaptureUi::ClipSource::send(wlr_data_source *source, const char *mime_type,
		int32_t fd) {
	auto *src = reinterpret_cast<ClipSource *>(source);
	if (mime_type != nullptr && std::strcmp(mime_type, "image/png") == 0) {
		src->start(fd);
	} else {
		close(fd);
	}
}

int CaptureUi::ClipSource::pump_write(int fd, uint32_t mask, void *data) {
	auto *src = static_cast<ClipSource *>(data);
	if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0) {
		src->stop();
		return 0;
	}
	while (src->sent < src->bytes.size()) {
		const std::uint8_t *rest = src->bytes.data() + src->sent;
		ssize_t n = write(fd, rest, src->bytes.size() - src->sent);
		if (n > 0) {
			src->sent += static_cast<std::size_t>(n);
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			return 0; // the client will read; the source stays armed
		} else {
			src->stop();
			return 0;
		}
	}
	src->stop();
	return 0;
}

void CaptureUi::ClipSource::destroy(wlr_data_source *source) {
	auto *src = reinterpret_cast<ClipSource *>(source);
	src->stop();
	// The owner's listener ran first and forgot us; nothing points here now.
	delete src;
}

// ------------------------------------------------------ the capture UI

CaptureUi::CaptureUi(Server &server)
	: server_(server) {
}

CaptureUi::~CaptureUi() {
	finish();
}

// ---- the picker ----

bool CaptureUi::start_pick(Output *output, PickWhat what, PickUse use,
		Options options) {
	if (output == nullptr) {
		output = server_.output_under_cursor();
	}
	if (output == nullptr || picker_.active) {
		return false;
	}
	close_toolbar_only(); // never both: a mode click closes it first anyway
	picker_ = Picker{
		.active = true,
		.output = output,
		.what = what,
		.window_mode = what == PickWhat::window || what == PickWhat::any,
		.use = use,
		.options = options,
	};
	picker_.cur_output = output;
	// The picker starts where the pointer already is: the crosshair and the
	// hover must not wait for the first motion to exist.
	picker_.cur_x = server_.cursor->x - output->lx;
	picker_.cur_y = server_.cursor->y - output->ly;
	picker_.hover = window_at(*output, picker_.cur_x, picker_.cur_y);
	server_.input.set_cursor_image(cursor_image());
	wlr_seat_pointer_notify_clear_focus(server_.seat);
	output->damage();
	return true;
}

void CaptureUi::close_picker() {
	if (!picker_.active) {
		return;
	}
	Output *output = picker_.output;
	picker_ = Picker{};
	server_.input.set_cursor_image("default");
	if (output != nullptr) {
		output->damage();
	}
}

void CaptureUi::cancel_picker() {
	if (!picker_.active) {
		return;
	}
	bool ipc = picker_.use == PickUse::ipc;
	close_picker();
	if (ipc) {
		server_.ipc.answer_pick(false, "cancelled");
	}
}

View *CaptureUi::window_at(Output &output, double ox, double oy) const {
	Point p = output.local_to_canvas(ox, oy);
	for (View *view : server_.views) { // front is topmost
		if (view->mapped && !view->minimized && view->frame_rect().contains(p.x, p.y)) {
			return view;
		}
	}
	return nullptr;
}

void CaptureUi::complete_pick(const Pick &pick) {
	PickUse use = picker_.use;
	Options options = picker_.options;
	close_picker();

	std::optional<wlr_box> area;
	if (pick.kind == Pick::Kind::region) {
		area = pick.area;
	} else if (pick.kind == Pick::Kind::window && use == PickUse::record) {
		// A window is recorded as the rectangle its frame had when it was
		// picked, so the recording keeps that box whatever the window does.
		area = frame_on_output(*pick.output, *pick.view);
	}

	switch (use) {
	case PickUse::ipc:
		answer_ipc(pick);
		return;
	case PickUse::shot:
		if (options.timer_s > 0) {
			deferred_ = Deferred{.kind = Deferred::Kind::shot, .output = pick.output,
				.area = area, .view = pick.kind == Pick::Kind::window ? pick.view : nullptr,
				.options = options};
			begin_countdown(options.timer_s);
			return;
		}
		take_shot(*pick.output, area, pick.kind == Pick::Kind::window ? pick.view : nullptr,
			options);
		return;
	case PickUse::record:
		if (options.timer_s > 0) {
			deferred_ = Deferred{.kind = Deferred::Kind::record, .output = pick.output,
				.area = area, .options = options};
			begin_countdown(options.timer_s);
			return;
		}
		start_recording(*pick.output, area, options);
		return;
	}
}

// The pick as the picker IPC answers it.
void CaptureUi::answer_ipc(const Pick &pick) {
	switch (pick.kind) {
	case Pick::Kind::region: {
		std::string name = pick.output->wlr->name;
		server_.ipc.answer_pick(true, std::format(
			R"({{"output":{},"x":{},"y":{},"w":{},"h":{}}})",
			quote_json(name), pick.area.x, pick.area.y, pick.area.width,
			pick.area.height));
		return;
	}
	case Pick::Kind::window: {
		const char *identifier = pick.view->ext_toplevel_identifier();
		server_.ipc.answer_pick(true, std::format(R"({{"window":{},"identifier":{}}})",
			pick.view->session_id, quote_json(identifier != nullptr ? identifier : "")));
		return;
	}
	case Pick::Kind::output: {
		std::string name = pick.output->wlr->name;
		server_.ipc.answer_pick(true,
			std::format(R"({{"output":{}}})", quote_json(name)));
		return;
	}
	}
}

// The release of a pick press: a drag takes its rectangle, a click takes
// the window or the output under the pointer.
void CaptureUi::release_pick(Output &output, double ox, double oy, bool dragged) {
	if (dragged && &output == picker_.output) {
		wlr_box area = normalized_box(picker_.press_x, picker_.press_y, ox, oy);
		if (area.width > 0 && area.height > 0) {
			complete_pick(Pick{.kind = Pick::Kind::region, .output = picker_.output,
				.area = area});
			return;
		}
		output.damage(); // a zero-sized drag picks nothing; keep picking
		return;
	}
	// A click: the window under the pointer, frame and all — every mode
	// but a plain output pick (Space mode and window picks included).
	if (picker_.what != PickWhat::output) {
		if (View *view = window_at(output, ox, oy)) {
			complete_pick(Pick{.kind = Pick::Kind::window, .output = &output,
				.view = view});
			return;
		}
		if (picker_.what == PickWhat::any) {
			complete_pick(Pick{.kind = Pick::Kind::output, .output = &output});
			return;
		}
		output.damage(); // no window there: the picker waits for a real pick
		return;
	}
	// Output picking: the screen the click landed on.
	complete_pick(Pick{.kind = Pick::Kind::output, .output = &output});
}

// ---- keyboard ----

bool CaptureUi::owns_keyboard() const {
	return picker_.active || toolbar_ || deferred_.kind != Deferred::Kind::none;
}

bool CaptureUi::takes_pointer_at(const Output &output, double ox, double oy) const {
	if (picker_.active) {
		bool every = picker_.what == PickWhat::any || picker_.what == PickWhat::output;
		return &output == picker_.output || every;
	}
	if (toolbar_ && &output == toolbar_output_) {
		return true;
	}
	return thumb_texture_ != nullptr
		&& (thumb_pressed_ || (thumb_output_ == &output && box_contains(thumb_box_, ox, oy)));
}

void CaptureUi::key(xkb_keysym_t sym, uint32_t mods) {
	sym = xkb_keysym_to_lower(sym);
	if (sym == XKB_KEY_Escape) {
		if (deferred_.kind != Deferred::Kind::none) {
			cancel_countdown();
		} else if (picker_.active) {
			cancel_picker();
		} else {
			close_toolbar_only();
		}
		return;
	}
	if (picker_.active && sym == XKB_KEY_space && mods == 0
			&& picker_.what == PickWhat::region) {
		picker_.window_mode = !picker_.window_mode;
		server_.input.set_cursor_image(cursor_image());
		if (picker_.output != nullptr) {
			picker_.output->damage();
		}
		return;
	}
	// The capture chords still work with the capture UI up, so the same
	// key that opened something closes it again.
	server_.bindings.handle_capture(sym, mods, server_.output_under_cursor());
}

const char *CaptureUi::cursor_image() const {
	if (picker_.active && !picker_.window_mode && picker_.what == PickWhat::region) {
		return "crosshair";
	}
	return "default";
}

// ---- pointer ----

bool CaptureUi::pointer_motion(Output &output, double ox, double oy) {
	if (picker_.active) {
		// Region picking stays on its own output; window and output picking
		// follow the pointer onto the other screens too.
		bool every = picker_.what == PickWhat::any || picker_.what == PickWhat::output;
		if (&output != picker_.output && !every) {
			return false;
		}
		picker_.cur_output = &output;
		picker_.cur_x = ox;
		picker_.cur_y = oy;
		if (picker_.pressed && !picker_.window_mode
				&& picker_.what == PickWhat::region) {
			double dx = ox - picker_.press_x, dy = oy - picker_.press_y;
			if (!picker_.dragging && std::hypot(dx, dy) > config::drag_threshold) {
				picker_.dragging = true;
			}
		}
		if (picker_.window_mode) {
			View *hover = window_at(output, ox, oy);
			if (hover != picker_.hover) {
				picker_.hover = hover;
				output.damage();
			}
		}
		return true;
	}
	if (toolbar_ && &output == toolbar_output_) {
		record_focus_ = false; // the accent was only the toolbar's opening cue
		int hit = toolbar_hit(toolbar_layout(output), ox, oy);
		if (hit != hover_) {
			hover_ = hit;
			toolbar_stale_ = true;
			output.damage();
		}
		return true;
	}
	return thumbnail_motion(output, ox, oy);
}

bool CaptureUi::pointer_button(Output &output, double ox, double oy, uint32_t button,
		bool pressed) {
	if (picker_.active) {
		bool every = picker_.what == PickWhat::any || picker_.what == PickWhat::output;
		if (&output != picker_.output && !every) {
			return false;
		}
		if (button != BTN_LEFT) {
			return true; // swallowed: the picker is modal
		}
		if (pressed) {
			picker_.pressed = true;
			picker_.dragging = false;
			picker_.press_x = ox;
			picker_.press_y = oy;
			return true;
		}
		picker_.pressed = false;
		bool dragged = picker_.dragging;
		picker_.dragging = false;
		release_pick(output, ox, oy, dragged);
		return true;
	}
	if (toolbar_ && &output == toolbar_output_) {
		if (button != BTN_LEFT) {
			return true;
		}
		if (pressed) {
			record_focus_ = false;
			pressed_ = toolbar_hit(toolbar_layout(output), ox, oy);
			if (pressed_ >= 0) {
				toolbar_stale_ = true;
				output.damage();
			} else {
				close_toolbar_only(); // a click outside cancels
			}
			return true;
		}
		int hit = toolbar_hit(toolbar_layout(output), ox, oy);
		if (hit >= 0 && hit == pressed_) {
			toolbar_activate(hit);
		}
		if (pressed_ >= 0) {
			pressed_ = -1;
			toolbar_stale_ = true;
			if (toolbar_ && toolbar_output_ != nullptr) {
				toolbar_output_->damage();
			}
		}
		return true;
	}
	return thumbnail_button(output, ox, oy, button, pressed);
}

// ---- the toolbar ----

void CaptureUi::open_toolbar(Output *output, bool recording_focus,
		const Options &preset) {
	if (output == nullptr) {
		output = server_.output_under_cursor();
	}
	if (output == nullptr) {
		return;
	}
	if (picker_.active) {
		cancel_picker(); // the newest interaction wins
	}
	bool same = toolbar_ && toolbar_output_ == output;
	toolbar_ = true;
	toolbar_output_ = output;
	record_focus_ = recording_focus && !same;
	if (preset.clipboard) {
		options_.clipboard = true; // the Ctrl chord presets the toggle
	}
	hover_ = -1;
	pressed_ = -1;
	toolbar_stale_ = true;
	wlr_seat_pointer_notify_clear_focus(server_.seat);
	output->damage();
}

void CaptureUi::close_toolbar_only() {
	if (!toolbar_) {
		return;
	}
	Output *output = toolbar_output_;
	toolbar_ = false;
	toolbar_output_ = nullptr;
	record_focus_ = false;
	hover_ = -1;
	pressed_ = -1;
	toolbar_texture_.reset();
	toolbar_stale_ = true;
	if (output != nullptr) {
		output->damage();
	}
}

CaptureUi::ToolbarLayout CaptureUi::toolbar_layout(const Output &output) const {
	int fixed = 2 * config::capture_toolbar_pad + 5 * config::capture_toolbar_gap
		+ config::capture_toolbar_group_gap;
	int avail = output.width - 2 * config::capture_thumbnail_margin - fixed;
	int bw = (avail - 5 * config::capture_toolbar_gap) / 6;
	if (bw > config::capture_toolbar_button_w) {
		bw = config::capture_toolbar_button_w;
	}
	if (bw < config::capture_toolbar_button_w / 2) {
		bw = config::capture_toolbar_button_w / 2; // tiny outputs clip, not crush
	}
	int inner = 6 * bw + 5 * config::capture_toolbar_gap
		+ config::capture_toolbar_group_gap;
	ToolbarLayout out;
	out.box = {
		.x = (output.width - (inner + 2 * config::capture_toolbar_pad)) / 2,
		.y = output.height - config::capture_toolbar_margin
			- (2 * config::capture_toolbar_pad + config::capture_toolbar_options_h
				+ config::capture_toolbar_gap + config::capture_toolbar_button_h
				+ config::capture_toolbar_gap + config::capture_toolbar_footer_h),
		.width = inner + 2 * config::capture_toolbar_pad,
		.height = 2 * config::capture_toolbar_pad + config::capture_toolbar_options_h
			+ config::capture_toolbar_gap + config::capture_toolbar_button_h
			+ config::capture_toolbar_gap + config::capture_toolbar_footer_h,
	};
	int modes_y = out.box.y + config::capture_toolbar_pad
		+ config::capture_toolbar_options_h + config::capture_toolbar_gap;
	for (int i = 0; i < 6; i++) {
		int x = out.box.x + config::capture_toolbar_pad
			+ i * (bw + config::capture_toolbar_gap)
			+ (i >= 3 ? config::capture_toolbar_group_gap : 0);
		out.modes[i] = {.x = x, .y = modes_y, .width = bw,
			.height = config::capture_toolbar_button_h};
	}
	out.records = {
		.x = out.modes[3].x,
		.y = modes_y,
		.width = out.modes[5].x + bw - out.modes[3].x,
		.height = config::capture_toolbar_button_h,
	};
	int options_y = out.box.y + config::capture_toolbar_pad;
	int pill = (inner - 2 * config::capture_toolbar_gap) / 3;
	for (int i = 0; i < 3; i++) {
		out.options[i] = {
			.x = out.box.x + config::capture_toolbar_pad
				+ i * (pill + config::capture_toolbar_gap),
			.y = options_y,
			.width = pill,
			.height = config::capture_toolbar_options_h,
		};
	}
	return out;
}

int CaptureUi::toolbar_hit(const ToolbarLayout &layout, double ox, double oy) const {
	for (int i = 0; i < 6; i++) {
		if (box_contains(layout.modes[i], ox, oy)) {
			return i;
		}
	}
	for (int i = 0; i < 3; i++) {
		if (box_contains(layout.options[i], ox, oy)) {
			return 6 + i;
		}
	}
	return -1;
}

// Runs one toolbar button: a mode starts that capture, an option turns.
void CaptureUi::toolbar_activate(int index) {
	if (index >= 6) {
		switch (index - 6) {
		case 0:
			options_.clipboard = !options_.clipboard;
			break;
		case 1: // none → 5 s → 10 s → none
			options_.timer_s = options_.timer_s == 0
				? config::capture_timer_steps[1]
				: (options_.timer_s == config::capture_timer_steps[1]
					? config::capture_timer_steps[2] : 0);
			break;
		default:
			options_.show_pointer = !options_.show_pointer;
			break;
		}
		toolbar_stale_ = true;
		if (toolbar_ && toolbar_output_ != nullptr) {
			toolbar_output_->damage();
		}
		return;
	}
	Output *output = toolbar_output_;
	Options options = options_;
	bool shot = index < 3;
	int target = index % 3; // 0 the screen, 1 a window, 2 a region
	close_toolbar_only();
	if (target != 0) {
		start_pick(output, target == 1 ? PickWhat::window : PickWhat::region,
			shot ? PickUse::shot : PickUse::record, options);
		return;
	}
	if (options.timer_s > 0) {
		deferred_ = Deferred{.kind = shot ? Deferred::Kind::shot
			: Deferred::Kind::record, .output = output, .options = options};
		begin_countdown(options.timer_s);
		return;
	}
	if (shot) {
		shoot_screen(*output, options);
		return;
	}
	start_recording(*output, std::nullopt, options);
}

// ---- key-chord actions ----

void CaptureUi::shoot_screen(Output &output, Options options) {
	take_shot(output, std::nullopt, nullptr, options);
}

void CaptureUi::pick_for_shot(Output *output, Options options) {
	if (picker_.active) {
		cancel_picker(); // the same chord again cancels
		return;
	}
	start_pick(output, PickWhat::region, PickUse::shot, options);
}

void CaptureUi::stop_recording_or_toolbar(Output *output, Options options) {
	if (recording()) {
		stop_recording();
		return;
	}
	open_toolbar(output, true, options);
}

// ---- shots ----

void CaptureUi::take_shot(Output &output, const std::optional<wlr_box> &area,
		View *view, const Options &options) {
	wlr_box whole{0, 0, output.width, output.height};
	wlr_box target = area.value_or(whole);
	CairoSurfacePtr surface = view != nullptr
		? capture_view(*view, output.wlr->scale)
		: capture_output(output, target);
	if (surface == nullptr) {
		server_.hint.set("The screenshot failed");
		return;
	}
	// The cursor, composited in only when it sat inside what was captured.
	wlr_box on_screen = view != nullptr ? frame_on_output(output, *view) : target;
	if (options.show_pointer) {
		composite_cursor(output, surface.get(), on_screen);
	}
	if (options.clipboard) {
		std::vector<std::uint8_t> png;
		cairo_surface_write_to_png_stream(surface.get(), png_collect, &png);
		copy_clipboard(std::move(png));
		server_.hint.set("Screenshot copied to the clipboard");
		return;
	}
	std::string dir = user_dir(G_USER_DIRECTORY_PICTURES, "Pictures") + "/Screenshots";
	mkdir_p(dir);
	std::string path = std::format("{}/Screenshot {}.png", dir, timestamp());
	for (int n = 2; access(path.c_str(), F_OK) == 0; n++) { // never overwrite
		path = std::format("{}/Screenshot {} {}.png", dir, timestamp(), n);
	}
	if (cairo_surface_write_to_png(surface.get(), path.c_str()) != CAIRO_STATUS_SUCCESS) {
		server_.hint.set("The screenshot could not be saved");
		return;
	}
	show_thumbnail(path, output, surface.get());
	server_.hint.set("Screenshot saved to " + dir);
}

// Draws the cursor image onto the shot, at the place the pointer had on
// screen, when the pointer was over this output and inside `on_screen`.
void CaptureUi::composite_cursor(Output &output, cairo_surface_t *shot,
		const wlr_box &on_screen) {
	double ox = server_.cursor->x - output.lx;
	double oy = server_.cursor->y - output.ly;
	if (!box_contains(on_screen, ox, oy)) {
		return;
	}
	float scale = output.wlr->scale;
	wlr_xcursor *cursor = wlr_xcursor_manager_get_xcursor(server_.xcursor_mgr,
		"default", scale);
	if (cursor == nullptr || cursor->image_count == 0) {
		return;
	}
	wlr_xcursor_image *image = cursor->images[0];
	cairo_surface_t *pixels = cairo_image_surface_create_for_data(image->buffer,
		CAIRO_FORMAT_ARGB32, static_cast<int>(image->width),
		static_cast<int>(image->height), static_cast<int>(image->width) * 4);
	CairoSurfacePtr hold{pixels};
	CairoPtr cr{cairo_create(shot)};
	cairo_set_source_surface(cr.get(), pixels,
		(ox - on_screen.x) * scale - image->hotspot_x,
		(oy - on_screen.y) * scale - image->hotspot_y);
	cairo_paint(cr.get());
}

void CaptureUi::copy_clipboard(std::vector<std::uint8_t> png) {
	if (clip_ != nullptr) {
		wlr_data_source_destroy(&clip_->base); // clip_gone ran; ClipSource freed
	}
	auto *source = new ClipSource{};
	source->owner = this;
	source->bytes = std::move(png);
	// wlroots has no public constructor for compositor-owned data
	// sources; the internal initializer takes our own vtable.
	static const wlr_data_source_impl impl = {
		.send = &ClipSource::send,
		.accept = nullptr,
		.destroy = &ClipSource::destroy,
	};
	wlr_data_source_init(&source->base, &impl);
	const char *mime = "image/png";
	char **slot = static_cast<char **>(wl_array_add(&source->base.mime_types,
		sizeof(char *)));
	*slot = strdup(mime);
	source->destroyed_.connect<&CaptureUi::clip_gone>(source->base.events.destroy,
		this);
	clip_ = source;
	wlr_seat_set_selection(server_.seat, &source->base,
		wl_display_next_serial(server_.display));
}

// The seat (or teardown) destroyed our source: forget it before its memory
// goes with the destroy implementation.
void CaptureUi::clip_gone() {
	if (clip_ != nullptr) {
		clip_->destroyed_.disconnect(); // the signal asserts no listener remains
		clip_ = nullptr;
	}
}

// ---- recording ----

void CaptureUi::start_recording(Output &output, const std::optional<wlr_box> &area,
		const Options &options) {
	if (recording()) {
		server_.hint.set("Already recording — Alt+Print stops it");
		return;
	}
	wlr_box target = area.value_or(wlr_box{0, 0, output.width, output.height});
	wlr_box clipped{};
	wlr_box bounds{0, 0, output.width, output.height};
	wlr_box_intersection(&clipped, &target, &bounds);
	if (wlr_box_empty(&clipped)) {
		server_.hint.set("Nothing there to record");
		return;
	}
	// The frames come from the offscreen captures, so what the recording
	// shows is what a shot of the same area would: the screen's content,
	// none of the compositor's own UI over it.
	float scale = output.wlr->scale;
	wlr_box dev{static_cast<int>(std::lround(clipped.x * scale)),
		static_cast<int>(std::lround(clipped.y * scale)),
		static_cast<int>(std::lround((clipped.x + clipped.width) * scale))
			- static_cast<int>(std::lround(clipped.x * scale)),
		static_cast<int>(std::lround((clipped.y + clipped.height) * scale))
			- static_cast<int>(std::lround(clipped.y * scale))};
	std::string dir = user_dir(G_USER_DIRECTORY_VIDEOS, "Videos");
	mkdir_p(dir);
	recorder_path_ = std::format("{}/Screen Recording {}.mp4", dir, timestamp());
	recorder_area_ = clipped;
	recorder_output_ = &output;

	// ffmpeg reads raw BGRA frames from the pipe and does the encoding;
	// the crop keeps odd-sized areas encodable as yuv420p.
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) < 0) {
		wlr_log_errno(WLR_ERROR, "capture: could not open the recorder pipe");
		recorder_path_.clear();
		recorder_output_ = nullptr;
		server_.hint.set("The recording could not start");
		return;
	}
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		sigset_t mask;
		sigemptyset(&mask);
		sigprocmask(SIG_SETMASK, &mask, nullptr);
		std::signal(SIGINT, SIG_DFL);
		std::signal(SIGTERM, SIG_DFL);
		std::signal(SIGPIPE, SIG_DFL);
		dup2(fds[0], STDIN_FILENO);
		close(fds[0]);
		close(fds[1]);
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0) {
			dup2(null, STDOUT_FILENO);
			dup2(null, STDERR_FILENO);
			if (null > STDERR_FILENO) {
				close(null);
			}
		}
		// ffmpeg starts with only its three standard fds: nothing the
		// compositor holds open reaches it.
		close_range(STDERR_FILENO + 1, ~0U, 0);
		std::vector<std::string> args{config::capture_recorder, "-hide_banner",
			"-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "bgra",
			"-s", std::format("{}x{}", dev.width, dev.height),
			"-r", std::format("{}", config::capture_recorder_fps),
			"-i", "pipe:0", "-vf", "crop=trunc(iw/2)*2:trunc(ih/2)*2",
			"-c:v", "libx264", "-preset", "veryfast", "-crf", "23",
			"-pix_fmt", "yuv420p", "-movflags", "+faststart", recorder_path_};
		std::vector<char *> argv;
		argv.reserve(args.size() + 1);
		for (const std::string &arg : args) {
			argv.push_back(const_cast<char *>(arg.c_str()));
		}
		argv.push_back(nullptr);
		execvp(args[0].c_str(), argv.data());
		_exit(127);
	}
	close(fds[0]);
	if (pid < 0) {
		close(fds[1]);
		wlr_log_errno(WLR_ERROR, "capture: could not fork the recorder");
		recorder_path_.clear();
		recorder_output_ = nullptr;
		server_.hint.set("The recording could not start");
		return;
	}
	recorder_pid_ = pid;
	recorder_fd_ = fds[1];
	reaper_.reset(wl_event_loop_add_timer(server_.loop,
		[](void *data) -> int { return static_cast<CaptureUi *>(data)->reap_recorder(); },
		this));
	if (reaper_) {
		wl_event_source_timer_update(reaper_.get(), config::capture_recorder_poll_ms);
	}
	frame_timer_.reset(wl_event_loop_add_timer(server_.loop,
		[](void *data) -> int { return static_cast<CaptureUi *>(data)->push_frame(); },
		this));
	if (frame_timer_) {
		wl_event_source_timer_update(frame_timer_.get(), 1); // the first frame now
	}
	server_.ipc.emit_recording(true, recorder_path_);
	server_.hint.set("Recording — Alt+Print or the bar's red dot stops it");
}

// One recording frame: the offscreen capture of the recorded area, its rows
// written whole to ffmpeg's stdin. A blocked or ended pipe ends the run.
int CaptureUi::push_frame() {
	if (recorder_fd_ < 0 || recorder_output_ == nullptr) {
		return 0;
	}
	CairoSurfacePtr frame = capture_output(*recorder_output_, recorder_area_);
	if (frame != nullptr) {
		cairo_surface_flush(frame.get());
		const unsigned char *data = cairo_image_surface_get_data(frame.get());
		int w = cairo_image_surface_get_width(frame.get());
		int h = cairo_image_surface_get_height(frame.get());
		int stride = cairo_image_surface_get_stride(frame.get());
		for (int y = 0; y < h && recorder_fd_ >= 0; y++) {
			const unsigned char *row = data + static_cast<std::size_t>(y) * stride;
			std::size_t left = static_cast<std::size_t>(w) * 4;
			while (left > 0) {
				ssize_t n = write(recorder_fd_, row, left);
				if (n < 0 && errno == EINTR) {
					continue;
				}
				if (n <= 0) {
					stop_recording(); // ffmpeg has exited
					return 0;
				}
				row += n;
				left -= static_cast<std::size_t>(n);
			}
		}
	}
	if (recorder_fd_ >= 0) {
		wl_event_source_timer_update(frame_timer_.get(),
			std::max(1, 1000 / config::capture_recorder_fps));
	}
	return 0;
}

int CaptureUi::reap_recorder() {
	int status = 0;
	pid_t done = waitpid(recorder_pid_, &status, WNOHANG);
	if (done == 0 || (done < 0 && errno == EINTR)) {
		wl_event_source_timer_update(reaper_.get(), config::capture_recorder_poll_ms);
		return 0;
	}
	recorder_gone(done < 0 ? 0 : status);
	return 0;
}

// The recorder process has left its file finished (or failed): report it.
void CaptureUi::recorder_gone(int status) {
	recorder_pid_ = -1;
	if (recorder_fd_ >= 0) {
		close(recorder_fd_);
		recorder_fd_ = -1;
	}
	reaper_.reset();
	frame_timer_.reset();
	recorder_output_ = nullptr;
	std::string path = std::move(recorder_path_);
	recorder_path_.clear();
	server_.ipc.emit_recording(false, path);
	struct stat st{};
	bool saved = !path.empty() && stat(path.c_str(), &st) == 0 && st.st_size > 0;
	wlr_log(WLR_INFO, "capture: the recorder exited with status %#x; %s", status,
		path.c_str());
	server_.hint.set(saved ? "Recording saved" : "The recording failed");
}

void CaptureUi::stop_recording() {
	if (!recording()) {
		return;
	}
	// The pipe's end is ffmpeg's clean stop: it sees the last of its
	// input, finalises the container and exits.
	if (recorder_fd_ >= 0) {
		close(recorder_fd_);
		recorder_fd_ = -1;
	}
	frame_timer_.reset();
	server_.hint.set("Stopping the recording…");
}

// ---- the countdown ----

void CaptureUi::begin_countdown(int seconds) {
	deferred_.seconds_left = seconds;
	if (!countdown_) {
		countdown_.reset(wl_event_loop_add_timer(server_.loop,
			[](void *data) -> int { return static_cast<CaptureUi *>(data)->tick_countdown(); },
			this));
	}
	if (countdown_) {
		wl_event_source_timer_update(countdown_.get(), config::capture_countdown_ms);
	}
	const char *what = deferred_.kind == Deferred::Kind::record ? "Recording" : "Shot";
	server_.hint.set(std::format("{} in {} s — Esc cancels", what, seconds));
}

int CaptureUi::tick_countdown() {
	deferred_.seconds_left--;
	if (deferred_.seconds_left > 0) {
		const char *what = deferred_.kind == Deferred::Kind::record ? "Recording" : "Shot";
		server_.hint.set(std::format("{} in {} s — Esc cancels",
			what, deferred_.seconds_left));
		wl_event_source_timer_update(countdown_.get(), config::capture_countdown_ms);
		return 0;
	}
	run_deferred();
	return 0;
}

void CaptureUi::cancel_countdown() {
	if (deferred_.kind == Deferred::Kind::none) {
		return;
	}
	deferred_ = Deferred{};
	countdown_.reset();
	server_.hint.set("");
}

void CaptureUi::run_deferred() {
	Deferred action = std::move(deferred_);
	deferred_ = Deferred{};
	countdown_.reset();
	server_.hint.set("");
	if (action.output == nullptr) {
		return;
	}
	switch (action.kind) {
	case Deferred::Kind::shot:
		take_shot(*action.output, action.area, action.view, action.options);
		return;
	case Deferred::Kind::record:
		start_recording(*action.output, action.area, action.options);
		return;
	case Deferred::Kind::none:
		return;
	}
}

// ---- the thumbnail ----

void CaptureUi::show_thumbnail(const std::string &path, Output &output,
		cairo_surface_t *shot) {
	thumb_texture_.reset();
	thumb_texture_ = texture_from_cairo(server_.renderer, shot);
	int dev_w = thumb_texture_ ? cairo_image_surface_get_width(shot) : 0;
	int dev_h = thumb_texture_ ? cairo_image_surface_get_height(shot) : 0;
	if (dev_w <= 0 || dev_h <= 0) {
		thumb_texture_.reset();
		return;
	}
	thumb_path_ = path;
	thumb_output_ = &output;
	double ar = static_cast<double>(dev_w) / dev_h;
	double w = std::fmin(config::capture_thumbnail_max_w,
		config::capture_thumbnail_max_h * ar);
	double h = w / ar;
	thumb_box_ = {
		static_cast<int>(std::round(output.width - config::capture_thumbnail_margin - w)),
		static_cast<int>(std::round(output.height - config::capture_thumbnail_margin - h)),
		static_cast<int>(std::round(w)),
		static_cast<int>(std::round(h)),
	};
	if (!thumb_timer_) {
		thumb_timer_.reset(wl_event_loop_add_timer(server_.loop,
			[](void *data) -> int {
				static_cast<CaptureUi *>(data)->dismiss_thumbnail();
				return 0;
			}, this));
	}
	if (thumb_timer_) {
		wl_event_source_timer_update(thumb_timer_.get(), config::capture_thumbnail_ms);
	}
	output.damage();
}

void CaptureUi::dismiss_thumbnail() {
	if (thumb_texture_ == nullptr && thumb_output_ == nullptr) {
		return;
	}
	thumb_texture_.reset();
	thumb_path_.clear();
	Output *output = thumb_output_;
	thumb_output_ = nullptr;
	thumb_timer_.reset();
	thumb_pressed_ = false;
	if (output != nullptr) {
		output->damage();
	}
}

bool CaptureUi::thumbnail_motion(Output &output, double ox, double oy) {
	if (thumb_texture_ == nullptr) {
		return false;
	}
	if (thumb_pressed_) {
		// A drag past the threshold — a swipe — sends it away.
		if (&output == thumb_output_) {
			double dx = ox - thumb_press_x_, dy = oy - thumb_press_y_;
			if (std::hypot(dx, dy) > config::drag_threshold) {
				dismiss_thumbnail();
			}
		} else {
			dismiss_thumbnail(); // dragged onto another output
		}
		return true;
	}
	return thumb_output_ == &output && box_contains(thumb_box_, ox, oy);
}

bool CaptureUi::thumbnail_button(Output &output, double ox, double oy, uint32_t button,
		bool pressed) {
	if (thumb_texture_ == nullptr) {
		return false;
	}
	bool over = thumb_output_ == &output && box_contains(thumb_box_, ox, oy);
	if (button != BTN_LEFT) {
		return over;
	}
	if (pressed) {
		if (!over) {
			return false;
		}
		thumb_pressed_ = true;
		thumb_press_x_ = ox;
		thumb_press_y_ = oy;
		return true;
	}
	if (!thumb_pressed_) {
		return false;
	}
	thumb_pressed_ = false;
	if (over) {
		spawn((std::string{"xdg-open "} + shell_quote(thumb_path_)).c_str());
	}
	return true;
}

// ---- rendering ----

const CaptureOverlay &CaptureUi::overlay(Output &output, float scale) {
	overlay_.fills.clear();
	overlay_.outlines.clear();
	overlay_.images.clear();
	if (picker_.active) {
		pick_overlay(output, scale);
	}
	if (toolbar_) {
		toolbar_overlay(output, scale);
	}
	if (thumb_texture_ != nullptr) {
		thumbnail_overlay(output, scale);
	}
	return overlay_;
}

// The picker overlay: everything outside the selection dimmed, a
// crosshair while a region is being framed, the selection with its size,
// the hovered window's frame, or the hovered output's own outline.
void CaptureUi::pick_overlay(Output &output, float scale) {
	const config::Palette &pal = server_.theme.colors();

	bool region = picker_.what == PickWhat::region && !picker_.window_mode;
	if (region) {
		if (&output != picker_.output) {
			return; // a region is framed on the picker's own screen
		}
		wlr_box sel{};
		if (!picker_.dragging) {
			int cx = static_cast<int>(std::round(picker_.cur_x));
			int cy = static_cast<int>(std::round(picker_.cur_y));
			overlay_.fills.push_back({.box = {0, cy, output.width, 1},
				.rgba = pal.picker_cross});
			overlay_.fills.push_back({.box = {cx, 0, 1, output.height},
				.rgba = pal.picker_cross});
			return;
		}
		sel = normalized_box(picker_.press_x, picker_.press_y, picker_.cur_x,
			picker_.cur_y);
		dim_around(sel, output, pal.launcher_backdrop);
		overlay_.outlines.push_back({.box = sel, .rgba = pal.accent,
			.thickness = config::capture_pick_outline});
		// The size beside the selection's far corner, kept on the screen.
		std::string text = std::format("{} × {}", sel.width, sel.height);
		if (size_texture_ == nullptr || size_text_ != text || size_scale_ != scale) {
			size_texture_.reset();
			TextTexture label = draw_text(server_.renderer, text.c_str(),
				config::ui_font, pal.panel_text, pal.panel, scale, 10, 5);
			size_texture_ = std::move(label.texture);
			size_text_ = text;
			size_scale_ = scale;
			size_w_ = label.width;
			size_h_ = label.height;
		}
		if (size_texture_ != nullptr && size_w_ > 0) {
			int lx = sel.x + sel.width + config::capture_pick_label_gap;
			int ly = sel.y + sel.height + config::capture_pick_label_gap;
			if (lx + size_w_ > output.width) {
				lx = sel.x - config::capture_pick_label_gap - size_w_;
			}
			if (ly + size_h_ > output.height) {
				ly = sel.y - config::capture_pick_label_gap - size_h_;
			}
			overlay_.images.push_back({.texture = size_texture_.get(),
				.box = {lx < 0 ? 0 : lx, ly < 0 ? 0 : ly, size_w_, size_h_}});
		}
		return;
	}

	// Window and output picking draw on the screen under the pointer.
	if (&output != picker_.cur_output) {
		return;
	}
	bool windows = picker_.window_mode || picker_.what == PickWhat::window
		|| picker_.what == PickWhat::any;
	if (windows && picker_.hover != nullptr) {
		wlr_box frame = frame_on_output(output, *picker_.hover);
		dim_around(frame, output, pal.launcher_backdrop);
		overlay_.outlines.push_back({.box = frame, .rgba = pal.accent,
			.thickness = config::capture_pick_outline});
		return;
	}
	if (!windows || picker_.what == PickWhat::any || picker_.what == PickWhat::output) {
		// No window under the pointer: the screen itself is the candidate.
		int inset = static_cast<int>(config::capture_pick_outline);
		wlr_box whole{inset, inset, output.width - 2 * inset,
			output.height - 2 * inset};
		overlay_.outlines.push_back({.box = whole, .rgba = pal.accent,
			.thickness = config::capture_pick_outline});
		return;
	}
	overlay_.fills.push_back({.box = {0, 0, output.width, output.height},
		.rgba = pal.launcher_backdrop});
}

// Four dim fills around `box`, covering the rest of the output.
void CaptureUi::dim_around(const wlr_box &box, const Output &output, uint32_t rgba) {
	overlay_.fills.push_back({.box = {0, 0, output.width, box.y}, .rgba = rgba});
	overlay_.fills.push_back({.box = {0, box.y + box.height, output.width,
		output.height - box.y - box.height}, .rgba = rgba});
	overlay_.fills.push_back({.box = {0, box.y, box.x, box.height}, .rgba = rgba});
	overlay_.fills.push_back({.box = {box.x + box.width, box.y,
		output.width - box.x - box.width, box.height}, .rgba = rgba});
}

void CaptureUi::toolbar_overlay(Output &output, float scale) {
	if (toolbar_output_ != &output) {
		return;
	}
	ToolbarLayout layout = toolbar_layout(output);
	if (toolbar_texture_ == nullptr || toolbar_stale_ || toolbar_scale_ != scale
			|| toolbar_box_.x != layout.box.x || toolbar_box_.y != layout.box.y
			|| toolbar_box_.width != layout.box.width
			|| toolbar_box_.height != layout.box.height) {
		toolbar_texture_.reset();
		toolbar_texture_ = draw_toolbar(output, layout);
		toolbar_scale_ = scale;
		toolbar_box_ = layout.box;
		toolbar_stale_ = false;
	}
	if (toolbar_texture_ == nullptr) {
		return;
	}
	overlay_.images.push_back({.texture = toolbar_texture_.get(), .box = layout.box});
	if (record_focus_) {
		// Alt+Print opened this: the recording choices carry the accent.
		overlay_.outlines.push_back({.box = layout.records,
			.rgba = server_.theme.colors().accent,
			.thickness = config::capture_pick_outline});
	}
}

void CaptureUi::thumbnail_overlay(Output &output, float scale) {
	(void)scale;
	if (thumb_output_ != &output) {
		return;
	}
	overlay_.images.push_back({.texture = thumb_texture_.get(), .box = thumb_box_});
	overlay_.outlines.push_back({.box = thumb_box_,
		.rgba = server_.theme.colors().launcher_border, .thickness = 1.0});
}

// The toolbar panel: its options row, the six mode buttons, the footer.
TexturePtr CaptureUi::draw_toolbar(const Output &output, const ToolbarLayout &layout) {
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

	const char *option_text[3] = {
		options_.clipboard ? "Copy to clipboard" : "Save to Screenshots",
		nullptr, // filled below: the timer names its setting
		options_.show_pointer ? "Pointer: on" : "Pointer: off",
	};
	std::string timer = options_.timer_s == 0
		? std::string{"Timer: off"}
		: std::format("Timer: {} s", options_.timer_s);
	option_text[1] = timer.c_str();
	bool option_on[3] = {options_.clipboard, options_.timer_s > 0,
		options_.show_pointer};
	for (int i = 0; i < 3; i++) {
		const wlr_box &box = layout.options[i];
		rounded_path(cr.get(), px + box.x, py + box.y, box.width, box.height, 8);
		set_source_rgba32(cr.get(),
			option_on[i] ? pal.launcher_selected : pal.launcher_search);
		cairo_fill(cr.get());
		paint_centred(cr.get(), option_text[i],
			option_on[i] ? pal.accent_text : pal.launcher_dim_text,
			px + box.x + box.width / 2.0, py + box.y + box.height / 2.0);
	}

	const char *mode_names[3] = {"Screen", "Window", "Region"};
	for (int i = 0; i < 6; i++) {
		const wlr_box &box = layout.modes[i];
		bool lit = hover_ == i || pressed_ == i;
		rounded_path(cr.get(), px + box.x, py + box.y, box.width, box.height, 8);
		set_source_rgba32(cr.get(), lit ? pal.launcher_hover : pal.launcher_search);
		cairo_fill(cr.get());
		double cx = px + box.x + box.width / 2.0;
		double cy = py + box.y + box.height / 2.0 - 7;
		paint_glyph(cr.get(), i % 3, pal.launcher_text, cx, cy);
		if (i >= 3) { // the recording mark: a red dot on the glyph's corner
			cairo_set_line_width(cr.get(), 1.5);
			cairo_arc(cr.get(), cx + 15, cy - 9, 4, 0, 2 * capture_pi);
			set_source_rgba32(cr.get(), pal.close_hover);
			cairo_fill(cr.get());
		}
		paint_centred(cr.get(), mode_names[i % 3], pal.launcher_dim_text, cx,
			py + box.y + box.height - 11);
	}

	paint_centred(cr.get(), "Esc closes", pal.launcher_dim_text,
		px + layout.box.width / 2.0,
		py + layout.box.height - config::capture_toolbar_pad / 2.0);

	return texture_from_cairo(server_.renderer, surf.get());
}

// ---- lifecycle ----

void CaptureUi::forget(const View &view) {
	if (picker_.hover == &view) {
		picker_.hover = nullptr;
	}
	if (deferred_.view == &view) {
		cancel_countdown(); // the window the shot waited for is gone
	}
}

void CaptureUi::output_destroyed(Output &output) {
	if (picker_.active && (picker_.output == &output || picker_.cur_output == &output)) {
		cancel_picker();
	}
	if (toolbar_ && toolbar_output_ == &output) {
		close_toolbar_only();
	}
	if (thumb_output_ == &output) {
		dismiss_thumbnail();
	}
	if (recorder_output_ == &output) {
		stop_recording(); // the recorded screen is gone
	}
}

void CaptureUi::theme_changed() {
	size_texture_.reset();
	size_text_.clear();
	size_scale_ = 0;
	toolbar_stale_ = true;
}

void CaptureUi::finish() {
	cancel_countdown();
	cancel_picker();
	close_toolbar_only();
	dismiss_thumbnail();
	if (clip_ != nullptr) {
		wlr_data_source_destroy(&clip_->base); // clip_gone ran; ClipSource freed
	}
	stop_recording();
	if (recorder_pid_ > 0) {
		// The polite stop gets a moment, then the recording is torn down.
		for (int waited = 0; waited < config::capture_recorder_stop_ms;
				waited += config::capture_recorder_poll_ms) {
			int status = 0;
			pid_t done = waitpid(recorder_pid_, &status, WNOHANG);
			if (done == recorder_pid_ || (done < 0 && errno == ECHILD)) {
				recorder_pid_ = -1;
				break;
			}
			timespec pause{.tv_sec = 0,
				.tv_nsec = config::capture_recorder_poll_ms * 1'000'000L};
			nanosleep(&pause, nullptr);
		}
		if (recorder_pid_ > 0) {
			kill(recorder_pid_, SIGKILL);
			int status = 0;
			while (waitpid(recorder_pid_, &status, 0) < 0 && errno == EINTR) {
			}
			recorder_pid_ = -1;
		}
	}
	reaper_.reset();
	recorder_output_ = nullptr;
	thumb_texture_.reset();
	toolbar_texture_.reset();
	size_texture_.reset();
}
