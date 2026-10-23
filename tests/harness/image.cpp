#include "harness/image.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <sys/wait.h>
#include <unistd.h>
#include "harness/files.h"

namespace harness {

namespace {

pid_t grim_pid = -1;

} // namespace

std::string shot_path(std::string_view name) {
	return std::format("{}/{}.png", scratch(), name);
}

void shot(std::string_view name, std::string_view geometry) {
	std::string path = shot_path(name);
	std::string area{geometry};
	grim_pid = fork();
	if (grim_pid != 0) {
		return;
	}
	if (area.empty()) {
		execlp("grim", "grim", path.c_str(), static_cast<char *>(nullptr));
	} else {
		execlp("grim", "grim", "-g", area.c_str(), path.c_str(), static_cast<char *>(nullptr));
	}
	_exit(127);
}

bool shot_running() {
	if (grim_pid <= 0) {
		return false;
	}
	if (waitpid(grim_pid, nullptr, WNOHANG) == 0) {
		return true;
	}
	grim_pid = -1;
	return false;
}

std::optional<Image> Image::read(const std::string &path) {
	Surface surface(cairo_image_surface_create_from_png(path.c_str()), cairo_surface_destroy);
	if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) {
		return std::nullopt;
	}
	cairo_surface_flush(surface.get()); // read-only from here on
	return Image(std::move(surface));
}

int Image::width() const {
	return cairo_image_surface_get_width(surface_.get());
}

int Image::height() const {
	return cairo_image_surface_get_height(surface_.get());
}

uint32_t Image::pixel(int x, int y) const {
	const unsigned char *data = cairo_image_surface_get_data(surface_.get());
	int stride = cairo_image_surface_get_stride(surface_.get());
	return reinterpret_cast<const uint32_t *>(data + y * stride)[x];
}

double Image::share_of(uint32_t argb) const {
	long hits = 0;
	for (int y = 0; y < height(); y++) {
		for (int x = 0; x < width(); x++) {
			if ((pixel(x, y) & 0x00ffffffu) == (argb & 0x00ffffffu)) {
				hits++;
			}
		}
	}
	long pixels = static_cast<long>(width()) * height();
	return pixels > 0 ? static_cast<double>(hits) / static_cast<double>(pixels) : -1;
}

std::array<double, 3> Image::average_rgb() const {
	double sums[3] = {0, 0, 0};
	for (int y = 0; y < height(); y++) {
		for (int x = 0; x < width(); x++) {
			uint32_t p = pixel(x, y);
			sums[0] += (p >> 16) & 0xff;
			sums[1] += (p >> 8) & 0xff;
			sums[2] += p & 0xff;
		}
	}
	double pixels = static_cast<double>(width()) * height();
	if (pixels <= 0) {
		return {0, 0, 0};
	}
	return {sums[0] / pixels / 255.0, sums[1] / pixels / 255.0, sums[2] / pixels / 255.0};
}

long Image::ink_in(wlr_box box) const {
	long ink = 0;
	for (int y = std::max(box.y, 0); y < std::min(box.y + box.height, height()); y++) {
		for (int x = std::max(box.x, 0); x < std::min(box.x + box.width, width()); x++) {
			if ((pixel(x, y) & 0x00ffffffu) != 0) {
				ink++;
			}
		}
	}
	return ink;
}

long Image::differing(const Image &other, const ImageCompare &how) const {
	if (width() != other.width() || height() != other.height()) {
		return -1;
	}
	wlr_box area = how.within.value_or(wlr_box{0, 0, width(), height()});
	long count = 0;
	for (int y = std::max(area.y, 0); y < std::min(area.y + area.height, height()); y++) {
		for (int x = std::max(area.x, 0); x < std::min(area.x + area.width, width()); x++) {
			bool skipped = std::ranges::any_of(how.skip, [&](const wlr_box &s) {
				return x >= s.x && y >= s.y && x < s.x + s.width && y < s.y + s.height;
			});
			if (skipped) {
				continue;
			}
			uint32_t a = pixel(x, y), b = other.pixel(x, y);
			for (int shift : {0, 8, 16}) {
				if (std::abs(static_cast<int>((a >> shift) & 0xff)
						- static_cast<int>((b >> shift) & 0xff)) > how.tolerance) {
					count++;
					break;
				}
			}
		}
	}
	return count;
}

std::optional<Image> read_shot(std::string_view name) {
	return Image::read(shot_path(name));
}

} // namespace harness
