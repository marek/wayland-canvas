#pragma once

#include <cstdint>
#include <vector>

#include "core/wlr.h"

// One drawn rectangle of the capture overlay, output-local logical px.
struct CaptureFill {
	wlr_box box{};
	uint32_t rgba = 0;
};

// A hollow rectangle: four thin fills of `thickness` logical px.
struct CaptureOutline {
	wlr_box box{};
	uint32_t rgba = 0;
	double thickness = 1.0;
};

// A texture placed on the output, output-local logical px.
struct CaptureImage {
	wlr_texture *texture = nullptr;
	wlr_box box{};
};

// Everything the capture UI draws on one output this frame; the painter
// turns each entry into a render-pass primitive.
struct CaptureOverlay {
	std::vector<CaptureFill> fills;
	std::vector<CaptureOutline> outlines;
	std::vector<CaptureImage> images;
};
