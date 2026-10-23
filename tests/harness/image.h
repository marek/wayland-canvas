#pragma once

// Screenshots: grim's picture of the compositor under test, and what a
// scenario reads out of one.

#include <array>
#include <cairo.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include "core/wlr.h"

namespace harness {

// grim's picture of the screen (or of `geometry`, in grim's "x,y WxH"
// form) in the scratch directory as <name>.png. grim runs beside the
// compositor, so the driver holds the next step back until it has exited.
void shot(std::string_view name, std::string_view geometry = {});
// The same of one window, named by its ext-foreign-toplevel identifier
// (`grim -T`).
void shot_toplevel(std::string_view name, std::string_view identifier);
bool shot_running();
std::string shot_path(std::string_view name);

// config.h's palette is 0xRRGGBBAA; a cairo pixel is 0xAARRGGBB.
constexpr uint32_t argb_of(uint32_t rgba) {
	return (rgba >> 8) | (rgba << 24);
}

// What counts when two images are compared.
struct ImageCompare {
	// Only these pixels count; all of them when empty.
	std::optional<wlr_box> within;
	// These do not count.
	std::span<const wlr_box> skip;
	// A pixel differs when a colour channel is further off than this.
	int tolerance = 0;
};

// An image read into memory.
class Image {
public:
	// Nullopt when the file is missing or no PNG.
	static std::optional<Image> read(const std::string &path);
	// An image surface already in memory, such as a capture the compositor
	// made. The image holds a reference of its own.
	static Image of(cairo_surface_t *surface);

	// Writes the image to the scratch directory as <name>.png.
	void save(std::string_view name) const;

	int width() const;
	int height() const;
	// 0xAARRGGBB.
	uint32_t pixel(int x, int y) const;
	// The share of the pixels that are the colour `argb`, alpha ignored.
	double share_of(uint32_t argb) const;
	// The mean red, green and blue, 0 to 1 each.
	std::array<double, 3> average_rgb() const;

	// How many pixels of `box`, clipped to the image, are not black: ink of
	// any kind drawn there.
	long ink_in(wlr_box box) const;

	// How many pixels of this image and `other` differ, -1 when their sizes do.
	long differing(const Image &other, const ImageCompare &how = {}) const;

private:
	using Surface = std::unique_ptr<cairo_surface_t, void (*)(cairo_surface_t *)>;
	explicit Image(Surface surface) : surface_(std::move(surface)) {
	}

	Surface surface_;
};

std::optional<Image> read_shot(std::string_view name);

} // namespace harness
