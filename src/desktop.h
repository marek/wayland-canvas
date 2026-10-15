#pragma once

#include "util.h"

// A screen-sized rectangle on the canvas. Windows belong to the desktop their centre lies
// in; moving the frame never moves windows.
struct Desktop {
	int id = 0;
	wlr_box box{}; // canvas coordinates
	double swipe_offset = 0; // overview: sideways offset during the abandon gesture

	// Label texture, drawn by render.cpp for label_id at label_scale.
	TexturePtr label;
	int label_id = 0, label_w = 0, label_h = 0;
	float label_scale = 0;
};
