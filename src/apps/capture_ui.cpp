// capture_ui.cpp: the capture UI's routing — keys and pointer to the picker,
// the toolbar and the thumbnail, what becomes of a finished pick, and the
// countdown. The components are in capture_picker.cpp, capture_toolbar.cpp,
// shot_thumbnail.cpp, shot_taker.cpp, clipboard_image.cpp and recorder.cpp.

#include <linux/input-event-codes.h>

#include <xkbcommon/xkbcommon-keysyms.h>

#include <cstdint>
#include <format>
#include <optional>
#include <string>

#include "apps/capture_geometry.h"
#include "apps/capture_ui.h"
#include "core/config.h"
#include "core/server.h"
#include "platform/output.h"
#include "render/draw.h"
#include "render/render.h"

namespace {

// One hollow rectangle of the capture overlay: four thin fills. The
// horizontal edges carry the corners.
void draw_outline(const RenderCtx &ctx, const CaptureOutline &outline) {
	wlr_render_color color = color_rgba(outline.rgba);
	double t = outline.thickness;
	const wlr_box &b = outline.box;
	draw_logical_rect(ctx, b.x, b.y, b.width, t, color);
	draw_logical_rect(ctx, b.x, b.y + b.height - t, b.width, t, color);
	draw_logical_rect(ctx, b.x, b.y + t, t, b.height - 2 * t, color);
	draw_logical_rect(ctx, b.x + b.width - t, b.y + t, t, b.height - 2 * t, color);
}

} // namespace

CaptureUi::CaptureUi(Server &server)
	: server_(server), clipboard_(server), thumbnail_(server),
	  shots_(server, clipboard_, thumbnail_), recorder_(server), picker_(server),
	  toolbar_(server) {
}

CaptureUi::~CaptureUi() {
	finish();
}

bool CaptureUi::open_picker(Output *output, PickWhat what, PickGoal goal) {
	if (output == nullptr) {
		output = server_.output_under_cursor();
	}
	if (output == nullptr || picker_.active()) {
		return false;
	}
	toolbar_.close(); // never both: a mode click closes it first anyway
	goal_ = std::move(goal);
	picker_.start(*output, what);
	return true;
}

bool CaptureUi::ask_pick(Output *output, PickWhat what, PickAnswer answer) {
	return open_picker(output, what, std::move(answer));
}

void CaptureUi::cancel_picker() {
	if (!picker_.active()) {
		return;
	}
	picker_.close();
	PickGoal goal = std::move(*goal_);
	goal_.reset();
	if (PickAnswer *answer = std::get_if<PickAnswer>(&goal)) {
		(*answer)(std::nullopt);
	}
}

void CaptureUi::pick_done(const CapturePicker::Pick &pick) {
	PickGoal goal = std::move(*goal_);
	goal_.reset();
	if (PickAnswer *answer = std::get_if<PickAnswer>(&goal)) {
		(*answer)(pick);
		return;
	}
	const auto &[use, options] = std::get<PickJob>(goal);
	std::optional<wlr_box> area;
	if (pick.kind == CapturePicker::Pick::Kind::region) {
		area = pick.area;
	} else if (pick.kind == CapturePicker::Pick::Kind::window && use == PickUse::record) {
		// A window is recorded as the rectangle its frame had when it was
		// picked, so the recording keeps that box whatever the window does.
		area = frame_on_output(*pick.output, *pick.view);
	}
	View *view = pick.kind == CapturePicker::Pick::Kind::window ? pick.view : nullptr;

	switch (use) {
	case PickUse::shot:
		if (options.timer_s > 0) {
			deferred_ = Deferred{.kind = Deferred::Kind::shot, .output = pick.output,
				.area = area, .view = view, .options = options};
			begin_countdown(options.timer_s);
			return;
		}
		shots_.take_shot(*pick.output, area, view, options);
		return;
	case PickUse::record:
		if (options.timer_s > 0) {
			deferred_ = Deferred{.kind = Deferred::Kind::record, .output = pick.output,
				.area = area, .options = options};
			begin_countdown(options.timer_s);
			return;
		}
		recorder_.start(*pick.output, area);
		return;
	}
}

bool CaptureUi::owns_keyboard() const {
	return picker_.active() || toolbar_.is_open() || deferred_.kind != Deferred::Kind::none;
}

bool CaptureUi::takes_pointer_at(const Output &output, double ox, double oy) const {
	if (picker_.active()) {
		return picker_.takes_pointer_at(output);
	}
	if (toolbar_.shows_on(output)) {
		return true;
	}
	return thumbnail_.takes_pointer_at(output, ox, oy);
}

void CaptureUi::key(const KeyPress &press) {
	press.syms.each([&](xkb_keysym_t sym) {
		key_sym(sym, press.mods);
	});
}

void CaptureUi::key_sym(xkb_keysym_t sym, uint32_t mods) {
	sym = xkb_keysym_to_lower(sym);
	if (sym == XKB_KEY_Escape) {
		if (deferred_.kind != Deferred::Kind::none) {
			cancel_countdown();
		} else if (picker_.active()) {
			cancel_picker();
		} else {
			toolbar_.close();
		}
		return;
	}
	if (picker_.active() && sym == XKB_KEY_space && mods == 0
			&& picker_.toggle_window_mode()) {
		return;
	}
	// The chords that reach everywhere (the capture actions') still work
	// with the capture UI up, so the same key that opened something closes
	// it again.
	server_.bindings.handle_everywhere(sym, mods, server_.output_under_cursor());
}

bool CaptureUi::pointer_motion(Output &output, double ox, double oy) {
	if (picker_.active()) {
		return picker_.pointer_motion(output, ox, oy);
	}
	if (toolbar_.pointer_motion(output, ox, oy)) {
		return true;
	}
	return thumbnail_.pointer_motion(output, ox, oy);
}

bool CaptureUi::pointer_button(Output &output, double ox, double oy, uint32_t button,
		bool pressed) {
	if (picker_.active()) {
		return picker_.pointer_button(output, ox, oy, button, pressed,
			[this](const CapturePicker::Pick &pick) {
				pick_done(pick);
			});
	}
	if (toolbar_.shows_on(output)) {
		return toolbar_.pointer_button(output, ox, oy, button, pressed,
			[this](ToolbarMode mode, Output &on, const CaptureOptions &options) {
				toolbar_chose(mode, on, options);
			});
	}
	return thumbnail_.pointer_button(output, ox, oy, button, pressed);
}

bool CaptureUi::pointer_axis(Output &output, double ox, double oy, wl_pointer_axis,
		double) {
	return takes_pointer_at(output, ox, oy); // scrolling is swallowed
}

void CaptureUi::open_toolbar(Output *output, bool recording_focus,
		const CaptureOptions &preset) {
	if (output == nullptr) {
		output = server_.output_under_cursor();
	}
	if (output == nullptr) {
		return;
	}
	if (picker_.active()) {
		cancel_picker(); // the newest interaction wins
	}
	toolbar_.open(*output, recording_focus, preset);
}

// A mode button was clicked: a pick of what it names, or the capture now.
void CaptureUi::toolbar_chose(ToolbarMode mode, Output &output,
		const CaptureOptions &options) {
	PickUse use = mode.record ? PickUse::record : PickUse::shot;
	if (mode.target != ToolbarTarget::screen) {
		open_picker(&output,
			mode.target == ToolbarTarget::window ? PickWhat::window : PickWhat::region,
			PickJob{use, options});
		return;
	}
	if (options.timer_s > 0) {
		deferred_ = Deferred{.kind = mode.record ? Deferred::Kind::record
			: Deferred::Kind::shot, .output = &output, .options = options};
		begin_countdown(options.timer_s);
		return;
	}
	if (!mode.record) {
		shoot_screen(output, options);
		return;
	}
	recorder_.start(output, std::nullopt);
}

void CaptureUi::shoot_screen(Output &output, CaptureOptions options) {
	shots_.take_shot(output, std::nullopt, nullptr, options);
}

void CaptureUi::pick_for_shot(Output *output, CaptureOptions options) {
	if (picker_.active()) {
		cancel_picker(); // the same chord again cancels
		return;
	}
	open_picker(output, PickWhat::region, PickJob{PickUse::shot, options});
}

void CaptureUi::stop_recording_or_toolbar(Output *output, CaptureOptions options) {
	if (recording()) {
		stop_recording();
		return;
	}
	open_toolbar(output, true, options);
}

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
		shots_.take_shot(*action.output, action.area, action.view, action.options);
		return;
	case Deferred::Kind::record:
		recorder_.start(*action.output, action.area);
		return;
	case Deferred::Kind::none:
		return;
	}
}

const CaptureOverlay &CaptureUi::overlay(Output &output, float scale) {
	overlay_.fills.clear();
	overlay_.outlines.clear();
	overlay_.images.clear();
	if (picker_.active()) {
		picker_.add_overlay(output, scale, overlay_);
	}
	toolbar_.add_overlay(output, scale, overlay_);
	thumbnail_.add_overlay(output, overlay_);
	return overlay_;
}

void CaptureUi::draw(const RenderCtx &ctx) {
	if (ctx.offscreen) {
		return; // a shot or a shared screen never shows the capture UI that took it
	}
	const CaptureOverlay &capture = overlay(*ctx.output, ctx.scale);
	for (const CaptureFill &fill : capture.fills) {
		draw_logical_rect(ctx, fill.box.x, fill.box.y, fill.box.width,
			fill.box.height, color_rgba(fill.rgba));
	}
	for (const CaptureOutline &outline : capture.outlines) {
		draw_outline(ctx, outline);
	}
	for (const CaptureImage &image : capture.images) {
		if (image.texture != nullptr) {
			draw_ui_texture(ctx, image.texture, image.box.x, image.box.y,
				image.box.width, image.box.height, 1.0);
		}
	}
}

void CaptureUi::forget(const View &view) {
	picker_.forget(view);
	if (deferred_.view == &view) {
		cancel_countdown(); // the window the shot waited for is gone
	}
}

void CaptureUi::output_gone(Output &output) {
	if (picker_.uses(output)) {
		cancel_picker();
	}
	if (toolbar_.shows_on(output)) {
		toolbar_.close();
	}
	thumbnail_.output_destroyed(output);
	recorder_.output_destroyed(output);
}

void CaptureUi::finish() {
	cancel_countdown();
	cancel_picker();
	toolbar_.close();
	thumbnail_.finish();
	clipboard_.finish();
	recorder_.finish();
}
