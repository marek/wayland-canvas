#pragma once

#include <cstdint>
#include <span>

#include "core/util.h"

class Output;
class Server;
class View;

// Offscreen captures shared by shots, recordings and the capture protocols.
// Each returns an ARGB32 cairo image surface in device pixels, or null.

// What `output` shows now: `local_area`, an output-local logical area, at the
// output's scale.
CairoSurfacePtr capture_output(Output &output, wlr_box local_area);

// The device-pixel size `local_area` captures at: the area clipped to the
// output, at the output's scale; empty when none of it is on the output.
wlr_box output_device_size(const Output &output, wlr_box local_area);

// capture_output() into storage the caller keeps between calls, the way a
// recording does every frame: `buffer` is the render target, sized
// output_device_size(), and `pixels` receives the frame as BGRA, rows tightly
// packed (width * height * 4 bytes). False when nothing was captured.
bool capture_output(Output &output, wlr_box local_area, wlr_buffer *buffer,
	std::span<std::uint8_t> pixels);

// The window alone: title bar, border, content; nothing over it, whatever
// the canvas zoom; at `scale` device pixels per logical pixel.
CairoSurfacePtr capture_view(View &view, float scale);

// The device-pixel size of `view`'s frame at `scale`: the buffer
// Painter::render_view() fills edge to edge. The capture protocols' window
// source renders through that same call into a buffer it reuses, which
// render_view() clears first, so a reused one never shows an earlier frame
// through translucent content.
wlr_box view_device_size(const View &view, float scale);

// A fresh ARGB8888 render target `width` x `height` device pixels, or null.
wlr_buffer *make_capture_buffer(Server &server, int width, int height);

// Reads a rendered buffer back as an ARGB32 cairo image surface, or null.
CairoSurfacePtr buffer_to_cairo(wlr_renderer *renderer, wlr_buffer *buffer);
