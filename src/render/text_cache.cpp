#include "render/text_cache.h"

#include <algorithm>
#include "core/config.h"
#include "core/server.h"
#include "platform/output.h"
#include "render/draw.h"

Droppable::Droppable(TextCache &cache) : cache_(cache) {
	cache_.holders_.push_back(this);
}

Droppable::~Droppable() {
	std::erase(cache_.holders_, this);
}

void Droppable::damage(Output &output) {
	output.damage();
}

TextSpec panel_text_spec(std::string_view text, float scale, const config::Palette &pal) {
	return {
		.text = text,
		.font = config::ui_font,
		.fg = pal.panel_text,
		.bg = pal.panel,
		.scale = scale,
		.pad_x = config::panel_text_pad_x,
		.pad_y = config::panel_text_pad_y,
	};
}

TextCache::TextCache(Server &server) : server_(server) {
}

TextCache::~TextCache() = default;

bool TextCache::same_text(const Entry &entry, const TextSpec &spec) {
	return entry.scale == spec.scale && entry.pad_x == spec.pad_x && entry.pad_y == spec.pad_y
		&& entry.text == spec.text && entry.font == spec.font;
}

CachedText TextCache::get(const TextSpec &spec) {
	clock_++;
	for (Entry &entry : entries_) {
		if (same_text(entry, spec) && entry.fg == spec.fg && entry.bg == spec.bg) {
			entry.used = clock_;
			return {entry.texture.get(), entry.w, entry.h};
		}
	}
	std::string text{spec.text};
	TextTexture drawn = draw_text(server_.renderer, text.c_str(), spec.font, spec.fg, spec.bg,
		spec.scale, spec.pad_x, spec.pad_y);
	rebuilt_++;
	Entry &entry = entries_.emplace_back(Entry{
		.text = std::move(text),
		.font = spec.font,
		.fg = spec.fg,
		.bg = spec.bg,
		.scale = spec.scale,
		.pad_x = spec.pad_x,
		.pad_y = spec.pad_y,
		.texture = std::move(drawn.texture),
		.w = drawn.width,
		.h = drawn.height,
		.used = clock_,
	});
	return {entry.texture.get(), entry.w, entry.h};
}

std::optional<CachedText> TextCache::find_size(const TextSpec &spec) const {
	for (const Entry &entry : entries_) {
		if (same_text(entry, spec)) {
			return CachedText{entry.texture.get(), entry.w, entry.h};
		}
	}
	return std::nullopt;
}

void TextCache::trim() {
	while (entries_.size() > config::text_cache_entries) {
		auto oldest = std::ranges::min_element(entries_, {}, &Entry::used);
		if (oldest != std::prev(entries_.end())) {
			*oldest = std::move(entries_.back());
		}
		entries_.pop_back();
	}
}

void TextCache::clear() {
	entries_.clear();
	for (Droppable *holder : holders_) {
		holder->drop();
	}
}
