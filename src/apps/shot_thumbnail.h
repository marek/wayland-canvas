#pragma once

#include <cairo.h>

#include <cstdint>
#include <string>

#include "apps/capture_overlay.h"
#include "core/util.h"

class Output;
class Server;

// The thumbnail of the last saved shot in an output's corner: opened by a
// click, sent away by a swipe, gone after a few seconds.
class ShotThumbnail {
public:
	explicit ShotThumbnail(Server &server);
	ShotThumbnail(const ShotThumbnail &) = delete;
	ShotThumbnail &operator=(const ShotThumbnail &) = delete;

	// Shows `shot`, saved at `path`, on `output` for a while.
	void show(const std::string &path, Output &output, cairo_surface_t *shot);
	void dismiss();

	// Pointer at output-local logical (ox, oy). True when the event is
	// consumed: the pointer is over the thumbnail, or a press on it is
	// still going.
	bool pointer_motion(Output &output, double ox, double oy);
	bool pointer_button(Output &output, double ox, double oy, uint32_t button,
		bool pressed);
	// Whether the pointer at output-local (ox, oy) would be the thumbnail's.
	bool takes_pointer_at(const Output &output, double ox, double oy) const;

	void add_overlay(Output &output, CaptureOverlay &out);
	// An output went away: a thumbnail on it goes.
	void output_destroyed(Output &output);
	// Drops the thumbnail and its timer before the display and renderer go.
	void finish() { dismiss(); }

private:
	bool over(const Output &output, double ox, double oy) const;

	Server &server_;

	TexturePtr texture_;
	std::string path_;
	Output *output_ = nullptr;
	wlr_box box_{};
	EventSourcePtr timer_;
	bool pressed_ = false;
	double press_x_ = 0, press_y_ = 0;
};
