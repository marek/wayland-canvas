#pragma once

#include <cairo.h>

#include <optional>

#include "apps/capture_options.h"
#include "core/wlr.h"

class ClipboardImage;
class Output;
class Server;
class ShotThumbnail;
class View;

// Takes a shot through the shared offscreen captures (render/capture.h) and
// sends it where the options say: the clipboard, or a file in the Pictures
// dir that the thumbnail then shows.
class ShotTaker {
public:
	ShotTaker(Server &server, ClipboardImage &clipboard, ShotThumbnail &thumbnail);
	ShotTaker(const ShotTaker &) = delete;
	ShotTaker &operator=(const ShotTaker &) = delete;

	// A shot of `view`, else of `area` of `output` (output-local logical px;
	// the whole output when empty); tells the user how it went.
	void take_shot(Output &output, const std::optional<wlr_box> &area, View *view,
		const CaptureOptions &options);

private:
	void composite_cursor(Output &output, cairo_surface_t *shot, const wlr_box &on_screen);

	Server &server_;
	ClipboardImage &clipboard_;
	ShotThumbnail &thumbnail_;
};
