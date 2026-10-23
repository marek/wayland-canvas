#include "input/shortcut_capture.h"

#include <cstddef>
#include <utility>
#include "core/config.h"
#include "core/server.h"
#include "input/super_tap.h"

namespace {

// The binding modifier a key changes, when it is one. The settings' own
// bits match the wlr_keyboard ones (core/chord.h).
std::optional<uint32_t> modifier_bit(xkb_keysym_t sym) {
	switch (sym) {
	case XKB_KEY_Super_L:
	case XKB_KEY_Super_R:
		return settings::mod_logo;
	case XKB_KEY_Control_L:
	case XKB_KEY_Control_R:
		return settings::mod_ctrl;
	case XKB_KEY_Alt_L:
	case XKB_KEY_Alt_R:
		return settings::mod_alt;
	case XKB_KEY_Shift_L:
	case XKB_KEY_Shift_R:
		return settings::mod_shift;
	default:
		return std::nullopt;
	}
}

} // namespace

ShortcutCapture::ShortcutCapture(Server &server, SuperTap &tap)
	: server_(server), tap_(tap) {}

void ShortcutCapture::finish() {
	timer_.reset();
}

void ShortcutCapture::begin(Done done) {
	// A second capture cancels the first (it answers nullopt).
	cancel();
	capture_ = Capture{.done = std::move(done)};
	// What is held when the capture begins counts into the chord.
	capture_->mods = server_.input.held_modifiers();
	// The capture swallows everything, tap tracking included.
	tap_.disarm();
	tap_.cancel_pending();
	if (timer_ != nullptr) {
		wl_event_source_timer_update(timer_.get(), 0);
	}
}

void ShortcutCapture::cancel() {
	if (!capture_.has_value()) {
		return;
	}
	Done done = std::move(capture_->done);
	capture_.reset();
	if (done != nullptr) {
		done(std::nullopt);
	}
}

bool ShortcutCapture::takes(const wlr_keyboard_key_event &event,
		std::span<const xkb_keysym_t> syms) {
	if (!capture_.has_value()) {
		return false;
	}
	if (!capture_->ending) {
		key(event, syms);
		return true;
	}
	if (capture_->draining.empty() || event.state != WL_KEYBOARD_KEY_STATE_RELEASED) {
		// Nothing left to drain: the capture is over, and this event is
		// handled as ever.
		capture_.reset();
		return false;
	}
	// The answer is out; the keys held at the end drain.
	std::vector<uint32_t> &draining = capture_->draining;
	if (std::erase(draining, event.keycode) == 0) {
		capture_.reset();
		return false;
	}
	if (draining.empty()) {
		capture_.reset();
	}
	return true;
}

void ShortcutCapture::end(std::optional<settings::Chord> chord) {
	Done done = std::move(capture_->done);
	capture_->ending = true;
	if (wlr_keyboard *wlr = wlr_seat_get_keyboard(server_.seat); wlr != nullptr) {
		for (std::size_t i = 0; i < wlr->num_keycodes; i++) {
			capture_->draining.push_back(wlr->keycodes[i]);
		}
	}
	if (done != nullptr) {
		done(chord);
	}
}

void ShortcutCapture::key(const wlr_keyboard_key_event &event,
		std::span<const xkb_keysym_t> syms) {
	Capture &capture = *capture_;
	bool pressed = event.state == WL_KEYBOARD_KEY_STATE_PRESSED;
	bool all_modifiers = !syms.empty();
	bool logo = false;
	for (xkb_keysym_t sym : syms) {
		std::optional<uint32_t> bit = modifier_bit(sym);
		all_modifiers = all_modifiers && bit.has_value();
		logo = logo || (bit == settings::mod_logo);
		if (bit.has_value()) {
			capture.mods = pressed ? (capture.mods | *bit) : (capture.mods & ~*bit);
		}
	}
	// A modifier never ends the capture by itself — except a Super pressed
	// and released alone: the tap, or with a second within the interval
	// the double tap (docs/intents/shell/settings.md).
	if (all_modifiers) {
		if (!pressed && logo && capture.mods == 0) {
			if (capture.tapped) {
				end(settings::Chord{.mods = settings::mod_logo, .twice = true});
				return;
			}
			capture.tapped = true;
			arm_timer();
		}
		return;
	}
	// A release between presses is swallowed; the capture goes on.
	if (!pressed) {
		return;
	}
	// The first non-modifier key ends it as the chord held now. A bare
	// Escape cancels.
	if (xkb_keysym_to_lower(syms[0]) == XKB_KEY_Escape && capture.mods == 0) {
		end(std::nullopt);
		return;
	}
	// A key after a lone tap: the tap was the chord, the key swallowed
	// with it.
	if (capture.tapped) {
		end(settings::Chord{settings::mod_logo, 0});
		return;
	}
	end(settings::Chord{capture.mods, xkb_keysym_to_lower(syms[0])});
}

void ShortcutCapture::arm_timer() {
	if (timer_ == nullptr) {
		timer_.reset(wl_event_loop_add_timer(server_.loop, &ShortcutCapture::handle_timer,
			this));
	}
	wl_event_source_timer_update(timer_.get(), static_cast<int>(config::double_tap_ms));
}

// The interval passed without a second tap: the one tap was the chord.
int ShortcutCapture::handle_timer(void *data) {
	auto *self = static_cast<ShortcutCapture *>(data);
	if (self->capture_.has_value() && self->capture_->tapped && !self->capture_->ending) {
		self->end(settings::Chord{settings::mod_logo, 0});
	}
	return 0;
}
