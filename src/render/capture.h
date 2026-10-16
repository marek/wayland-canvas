#pragma once

#include "core/util.h"

class Output;
class Server;
class View;

// Offscreen captures shared by shots, recordings and the capture protocols.
// Each returns an ARGB32 cairo image surface in device pixels, or null.

// What `output` shows now: `local_area`, an output-local logical area, at the
// output's scale.
CairoSurfacePtr capture_output(Output &output, wlr_box local_area);

// The window alone: title bar, border, content; nothing over it, whatever
// the canvas zoom; at `scale` device pixels per logical pixel.
CairoSurfacePtr capture_view(View &view, float scale);

// The device-pixel size of `view`'s frame at `scale`, rounded the way
// render_view_offscreen() draws it: the buffer it fills edge to edge.
wlr_box view_device_size(const View &view, float scale);

// Renders the window alone — the same pixels capture_view returns — into
// `buffer`, sized view_device_size(). The capture protocols' window source
// renders through this same path; the buffer is cleared first, so a reused
// one never shows an earlier frame through translucent content.
bool render_view_offscreen(View &view, float scale, wlr_buffer *buffer);

// A fresh ARGB8888 render target `width` x `height` device pixels, or null.
wlr_buffer *make_capture_buffer(Server &server, int width, int height);

// Reads a rendered buffer back as an ARGB32 cairo image surface, or null.
CairoSurfacePtr buffer_to_cairo(wlr_renderer *renderer, wlr_buffer *buffer);
