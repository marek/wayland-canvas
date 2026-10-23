#include "render/hint.h"

#include "core/config.h"
#include "core/server.h"
#include "platform/output.h"
#include "render/pass.h"

Hint::Hint(Server &server) : server_(server) {}

Hint::~Hint() = default;

void Hint::set(std::string_view text) {
	if (text.empty()) {
		clear();
		server_.damage_all();
		return;
	}
	if (text == text_) {
		keep();
		return;
	}
	text_.assign(text);
	restart_timer();
	server_.damage_all();
}

void Hint::keep() {
	if (!text_.empty()) {
		restart_timer();
	}
}

void Hint::restart_timer() {
	if (!timer_) {
		timer_.reset(wl_event_loop_add_timer(server_.loop,
			[](void *data) -> int {
				auto *hint = static_cast<Hint *>(data);
				// set({}) may have cleared it already; the timer just no-ops.
				if (!hint->text_.empty()) {
					hint->clear();
					hint->server_.damage_all();
				}
				return 0;
			}, this));
	}
	if (timer_) {
		wl_event_source_timer_update(timer_.get(), 2000);
	}
}

void Hint::clear() {
	text_.clear();
}

void Hint::finish() {
	timer_.reset();
	text_.clear();
}

void Hint::draw(const RenderCtx &ctx, std::optional<wlr_box> row) const {
	if (text_.empty()) {
		return;
	}
	const Output &o = *ctx.output;
	CachedText hint = server_.text_cache.get(
		panel_text_spec(text_, ctx.scale, server_.theme.colors()));
	if (hint.texture == nullptr || hint.w <= 0 || hint.h <= 0) {
		return;
	}
	int y = row.has_value()
		? row->y + row->height - hint.h
		: o.height - config::hint_bottom_margin - hint.h;
	draw_ui_texture(ctx, hint.texture, (o.width - hint.w) / 2.0, y, hint.w, hint.h, 1.0);
}
