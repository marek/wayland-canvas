#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include "core/geometry.h"
#include "core/wlr.h"

// wlroots' integer box and geometry.h's canvas Rect, and the ways to turn
// one into the other. A Rect becomes a box by one of four roundings, each
// kept by the sites that need exactly it.

inline bool operator==(const wlr_box &a, const wlr_box &b) {
	return wlr_box_equal(&a, &b);
}

inline Rect rect_of_box(const wlr_box &box) {
	return Rect{
		static_cast<double>(box.x),
		static_cast<double>(box.y),
		static_cast<double>(box.width),
		static_cast<double>(box.height),
	};
}

inline Point box_centre(const wlr_box &box) {
	return Point{
		.x = box.x + box.width / 2.0,
		.y = box.y + box.height / 2.0,
	};
}

// A width x height box centred on (cx, cy), its origin rounded to an integer.
inline wlr_box centred_box(int width, int height, double cx, double cy) {
	return wlr_box{
		static_cast<int>(std::round(cx - width / 2.0)),
		static_cast<int>(std::round(cy - height / 2.0)),
		width,
		height,
	};
}

// Each of x, y, width and height to the nearest integer.
inline wlr_box box_rounded(const Rect &r) {
	return wlr_box{
		static_cast<int>(std::round(r.x)),
		static_cast<int>(std::round(r.y)),
		static_cast<int>(std::round(r.w)),
		static_cast<int>(std::round(r.h)),
	};
}

// The four edges to the nearest integer, so boxes that share an edge keep
// sharing it and boxes a gap apart stay that far apart at any rounding.
inline wlr_box box_snapped(const Rect &r) {
	int left = static_cast<int>(std::round(r.x));
	int top = static_cast<int>(std::round(r.y));
	return wlr_box{
		left,
		top,
		static_cast<int>(std::round(r.x + r.w)) - left,
		static_cast<int>(std::round(r.y + r.h)) - top,
	};
}

// The origin to the nearest integer, the size rounded up: an output's
// viewport as the canvas box it shows.
inline wlr_box box_of_viewport(const Rect &r) {
	return wlr_box{
		static_cast<int>(std::round(r.x)),
		static_cast<int>(std::round(r.y)),
		static_cast<int>(std::ceil(r.w)),
		static_cast<int>(std::ceil(r.h)),
	};
}

// The origin rounded down, the size up: a box that holds the whole rect.
inline wlr_box box_covering(const Rect &r) {
	return wlr_box{
		static_cast<int>(std::floor(r.x)),
		static_cast<int>(std::floor(r.y)),
		static_cast<int>(std::ceil(r.w)),
		static_cast<int>(std::ceil(r.h)),
	};
}

// The index of the first of `boxes` that holds the point, if any.
inline std::optional<std::size_t> box_hit(std::span<const wlr_box> boxes, double x, double y) {
	for (std::size_t i = 0; i < boxes.size(); i++) {
		if (wlr_box_contains_point(&boxes[i], x, y)) {
			return i;
		}
	}
	return std::nullopt;
}
