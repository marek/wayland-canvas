#pragma once

#include <array>
#include <cstddef>
#include "core/box.h"
#include "core/util.h"
#include "render/text_cache.h"

class Output;

// A texture the compositor draws for itself that is not one run of text -
// the launcher's panel, the capture toolbar, a window's title bar - kept
// while what it shows is unchanged. `Key` is everything the picture depends
// on besides the scale (a box, the state of a button), compared with ==. A
// few pictures are kept side by side, so outputs at different scales each
// find theirs. The TextCache that owns it drops it with the rest when the
// palette changes.
template <typename Key>
class CachedTexture final : public Droppable {
public:
	static constexpr std::size_t slot_count = 4;

	explicit CachedTexture(TextCache &cache) : Droppable(cache) {}

	// The picture for `scale` and `key`, from `draw()` (a TexturePtr) when none is held.
	template <typename Draw>
	wlr_texture *get(float scale, const Key &key, Draw &&draw) {
		for (Slot &slot : slots_) {
			if (slot.texture != nullptr && slot.scale == scale && slot.key == key) {
				return slot.texture.get();
			}
		}
		Slot &slot = slots_[next_++ % slot_count];
		slot.texture = draw();
		slot.scale = scale;
		slot.key = key;
		cache_.count_rebuild();
		return slot.texture.get();
	}

	// The next get() draws again: what the picture shows changed. With an
	// output, it repaints too.
	void invalidate() { drop(); }
	void invalidate(Output &output) {
		drop();
		damage(output);
	}

	void drop() override {
		for (Slot &slot : slots_) {
			slot.texture.reset();
		}
	}

private:
	struct Slot {
		TexturePtr texture;
		float scale = 0;
		Key key{};
	};

	std::array<Slot, slot_count> slots_{};
	std::size_t next_ = 0;
};
