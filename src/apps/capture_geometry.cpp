#include "apps/capture_geometry.h"

#include <cmath>

#include "platform/output.h"
#include "windows/view.h"

wlr_box frame_on_output(const Output &output, const View &view) {
	Rect frame = view.frame_rect();
	Point corner = output.canvas_to_local(frame.x, frame.y);
	double zoom = output.camera.drawn().zoom;
	return {
		static_cast<int>(std::round(corner.x)),
		static_cast<int>(std::round(corner.y)),
		static_cast<int>(std::round(frame.w * zoom)),
		static_cast<int>(std::round(frame.h * zoom)),
	};
}
