#pragma once

// Where things are on screen, asked of the compositor under test so that a
// scenario never writes a pixel position the layout decides. Points are
// output-local logical pixels on the first output, the ones the fake pointer
// and touchscreen take.

#include <string_view>
#include "core/geometry.h"
#include "windows/decoration.h"

class LayerSurface;
class View;

namespace harness {

// The middle of `part`'s title-bar button on `view`, at the camera the output
// shows now.
Point deco_point(const View &view, DecoPart part);

// A point on `view`'s title bar clear of its buttons: `from_left` px in from
// the frame's left edge, half the title bar down.
Point title_point(const View &view, double from_left = 80);

// The first mapped view titled `title`; null when there is none.
View *titled(std::string_view title);

// The output-local point of the point (`sx`, `sy`) of `view`'s own surface,
// at the camera the output shows now.
Point on_screen(const View &view, double sx, double sy);

// `view`'s whole frame, title bar and border included, where the output shows
// it: the corner rounded to a pixel and the size at the camera's zoom.
wlr_box frame_on_screen(const View &view);

// The mapped layer surface of the shell client with this layer-shell
// namespace, on any output; null when there is none.
LayerSurface *layer_named(const char *ns);

// The bar strip's height, and the y of its middle: where a click on the bar
// lands. Both read the bar's layer surface, and answer for the default bar
// (26 px) until it is mapped.
int bar_height();
double bar_mid_y();

} // namespace harness
