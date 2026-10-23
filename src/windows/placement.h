#pragma once

#include <optional>
#include <string>
#include "windows/view.h"

// Puts the window where it opens: in the area its mode fills, over the window it
// belongs to, or centred in the screen it was launched for and stepped off the windows
// already there. Writes the window's position only.
void place(View &view);

// The area a window in `mode` fills; none for a window in its own box.
std::optional<wlr_box> area_for(View &view, WindowMode mode);
wlr_box fullscreen_area(View &view);

// Whether an output shows the window: an open overview shows the canvas, a normal view
// the windows whose frame centre its viewport holds.
bool shown_anywhere(const View &view);

// For the hint about a window that opened unseen: where it is.
std::string opened_where(View &view);
