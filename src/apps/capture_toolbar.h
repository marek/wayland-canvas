#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include "apps/capture_options.h"
#include "apps/capture_overlay.h"
#include "apps/capture_toolbar_layout.h"
#include "core/util.h"
#include "render/cached_texture.h"

class Output;
class Server;

// The capture toolbar on one output: the options pills, the six mode
// buttons and the footer, drawn into a texture the capture overlay places.
// A click on a mode closes it and reports the mode with the options as
// they stood.
class CaptureToolbar {
public:
	// A mode was clicked: the toolbar is already closed.
	using Chose = std::function_ref<void(ToolbarMode, Output &, const CaptureOptions &)>;

	explicit CaptureToolbar(Server &server);
	CaptureToolbar(const CaptureToolbar &) = delete;
	CaptureToolbar &operator=(const CaptureToolbar &) = delete;

	bool is_open() const { return output_ != nullptr; }
	bool shows_on(const Output &output) const { return output_ == &output; }

	// Opens on `output`; a second open replaces it. `recording_focus`
	// opens it with the recording choices accented (Alt+Print with nothing
	// running); a `preset` with clipboard turns the destination option to
	// the clipboard (the Ctrl chords).
	void open(Output &output, bool recording_focus, const CaptureOptions &preset);
	void close();

	// Pointer at output-local logical (ox, oy). True when the event is
	// consumed: the toolbar is open on `output`.
	bool pointer_motion(Output &output, double ox, double oy);
	bool pointer_button(Output &output, double ox, double oy, uint32_t button,
		bool pressed, Chose chose);

	// The panel, and the accent on the recordings when opened for them,
	// onto `out` when the toolbar is open on `output`.
	void add_overlay(Output &output, float scale, CaptureOverlay &out);

private:
	std::optional<ToolbarButton> button_at(const Output &output, double ox, double oy) const;
	void activate(ToolbarButton button, Chose chose);
	TexturePtr draw(const Output &output, const ToolbarLayout &layout);

	Server &server_;

	Output *output_ = nullptr;
	bool record_focus_ = false;
	CaptureOptions options_{}; // the visible options
	std::optional<ToolbarButton> hover_;
	std::optional<ToolbarButton> pressed_;

	CachedTexture<wlr_box> panel_; // the toolbar image, per scale and layout
};
