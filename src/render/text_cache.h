#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "core/util.h"

class Output;
class Server;
class TextCache;

namespace config {
struct Palette;
}

// What a text texture shows: `text` in `font`, `fg` on `bg` (0xRRGGBBAA; a
// bg with zero alpha stays transparent), `pad_x` and `pad_y` logical px
// around it, rendered at `scale`.
struct TextSpec {
	std::string_view text;
	const char *font = nullptr;
	uint32_t fg = 0, bg = 0;
	float scale = 1;
	int pad_x = 0, pad_y = 0;
};

// A run of the UI font on a panel, as the hint, the help line and the
// picker's size label show it.
TextSpec panel_text_spec(std::string_view text, float scale, const config::Palette &pal);

// A cached text texture and its size, padding included.
struct CachedText {
	wlr_texture *texture = nullptr; // null when the text failed to render
	int w = 0, h = 0; // logical px
};

// A holder of cached textures that TextCache::clear() empties along with its
// own: it registers for as long as it lives.
class Droppable {
public:
	explicit Droppable(TextCache &cache);
	virtual ~Droppable();
	Droppable(const Droppable &) = delete;
	Droppable &operator=(const Droppable &) = delete;

	// Lets go of every texture held; they are drawn again when next asked for.
	virtual void drop() = 0;

protected:
	// `output` repaints.
	static void damage(Output &output);

	TextCache &cache_;
};

// Every text texture the compositor draws for its own UI, one per distinct
// request, and the one place the textures drawn for the palette are dropped
// when it changes. The scale is part of the request, so outputs at different
// scales each find theirs and a frame that shows what the last one showed
// builds nothing. The least recently asked for go when more than
// config::text_cache_entries are held.
class TextCache {
public:
	explicit TextCache(Server &server);
	~TextCache();
	TextCache(const TextCache &) = delete;
	TextCache &operator=(const TextCache &) = delete;

	// The texture for `spec`, built when none is cached. Valid until the
	// next trim() or clear().
	CachedText get(const TextSpec &spec);
	// The size of a cached texture of `spec`'s text, font, padding and
	// scale, whatever its colours: a text measures the same in every colour.
	std::optional<CachedText> find_size(const TextSpec &spec) const;

	// Drops the least recently asked for textures beyond the capacity. Called
	// between frames: a texture handed out is never dropped while it is drawn.
	void trim();
	// Drops every texture, the registered holders' too: the palette changed,
	// or the renderer goes.
	void clear();

	// How many textures were built since the compositor started, here and by
	// the registered holders.
	unsigned long rebuilt() const { return rebuilt_; }
	void count_rebuild() { rebuilt_++; }

private:
	friend class Droppable;

	struct Entry {
		std::string text, font;
		uint32_t fg = 0, bg = 0;
		float scale = 1;
		int pad_x = 0, pad_y = 0;
		TexturePtr texture;
		int w = 0, h = 0;
		unsigned long used = 0; // clock_ when last asked for
	};

	static bool same_text(const Entry &entry, const TextSpec &spec);

	Server &server_;
	std::vector<Entry> entries_;
	std::vector<Droppable *> holders_;
	unsigned long clock_ = 0;
	unsigned long rebuilt_ = 0;
};
