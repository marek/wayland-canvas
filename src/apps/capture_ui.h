#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <xkbcommon/xkbcommon.h>
#include "apps/capture_options.h"
#include "apps/capture_overlay.h"
#include "apps/capture_picker.h"
#include "apps/capture_toolbar.h"
#include "apps/clipboard_image.h"
#include "apps/recorder.h"
#include "apps/shot_taker.h"
#include "apps/shot_thumbnail.h"
#include "core/util.h"

class Output;
class Server;
class View;

// The shots-and-recordings UI the compositor draws itself: the picker
// overlay (a region dragged, a window or an output clicked), the capture
// toolbar, the after-shot thumbnail, and the recorder's life cycle. Each is
// a component of its own; this class routes the keys and the pointer to
// them, takes their reports and runs the countdown. Shots are taken through
// the shared offscreen captures (src/render/capture.h); a recording is those
// same offscreen captures piped as raw frames into ffmpeg, the one encoder,
// so it works nested and from a TTY alike. While the picker, the toolbar or
// a countdown is up, the capture UI owns the keyboard and the pointer.
class CaptureUi {
public:
	explicit CaptureUi(Server &server);
	~CaptureUi();
	CaptureUi(const CaptureUi &) = delete;
	CaptureUi &operator=(const CaptureUi &) = delete;

	// True while the picker overlay is up.
	bool picking() const { return picker_.active(); }

	// Opens the picker on `output` (the output under the cursor when null);
	// false when there is no output to pick on or a pick already runs. An
	// ipc pick answers the pending `capture.pick` request; a shot or record
	// pick uses the given options.
	bool start_pick(Output *output, PickWhat what, PickUse use, CaptureOptions options);

	// Opens the capture toolbar on `output` (under the cursor when null);
	// a second open replaces it. `recording_focus` opens it with the
	// recording choices accented (Alt+Print with nothing running); a
	// `preset` with clipboard turns the destination option to the
	// clipboard (the Ctrl chords).
	void open_toolbar(Output *output, bool recording_focus,
		const CaptureOptions &preset = {});

	// True while the capture UI takes every key: the picker, the toolbar
	// or a countdown is up.
	bool owns_keyboard() const;
	// One key press, with the modifiers. Esc cancels or closes, Space
	// switches the picker between region and window picking, and the
	// capture chords toggle their own UI.
	void key(xkb_keysym_t sym, uint32_t mods);

	// Pointer at output-local logical (ox, oy). True when the event is
	// consumed: the picker or the toolbar is up, or the pointer is over
	// the thumbnail.
	bool pointer_motion(Output &output, double ox, double oy);
	bool pointer_button(Output &output, double ox, double oy, uint32_t button,
		bool pressed);
	// Whether the pointer at output-local (ox, oy) would be the capture
	// UI's — the claim pointer_motion makes, without its hover: a touch or
	// the pen asks before it picks a client.
	bool takes_pointer_at(const Output &output, double ox, double oy) const;
	// The cursor image the picker wants (crosshair while framing a region).
	const char *cursor_image() const { return picker_.cursor_image(); }

	// A shot of the whole output now.
	void shoot_screen(Output &output, CaptureOptions options);
	// The Super+Shift+4 / Print picker: drag a region, click a window.
	void pick_for_shot(Output *output, CaptureOptions options);
	// Alt+Print: stop the recording, else the toolbar on its recordings.
	void stop_recording_or_toolbar(Output *output, CaptureOptions options);

	bool recording() const { return recorder_.running(); }
	const std::string &recording_path() const { return recorder_.path(); }
	std::size_t recording_dropped_frames() const { return recorder_.dropped_frames(); }
	// Stops the running recorder (a no-op when none runs); the recording
	// event fires when the process has left its file finished.
	void stop_recording() { recorder_.stop(); }

	// What the capture UI draws on `output` this frame, at `scale` (the
	// output's): the picker overlay, the toolbar, the thumbnail. Rebuilt
	// into a reused buffer; empty when nothing is up.
	const CaptureOverlay &overlay(Output &output, float scale);

	// A view went away: the picker stops hovering or picking it.
	void forget(const View &view);
	// An output went away: the UI on it closes, its recording stops.
	void output_destroyed(Output &output);
	// Drops the cached textures so the next frame redraws in the new palette.
	void theme_changed();
	// Stops the recorder and releases timers and textures before the
	// display and renderer go.
	void finish();

private:
	// The action waiting out a countdown.
	struct Deferred {
		enum class Kind { none, shot, record };
		Kind kind = Kind::none;
		Output *output = nullptr;
		std::optional<wlr_box> area; // region: output-local logical
		View *view = nullptr; // shot of one window
		CaptureOptions options{};
		int seconds_left = 0;
	};

	void cancel_picker(); // close + answer a pending ipc pick "cancelled"
	void pick_done(const CapturePicker::Pick &pick, PickUse use,
		const CaptureOptions &options);
	void answer_ipc(const CapturePicker::Pick &pick); // the pick as `capture.pick` answers
	void toolbar_chose(ToolbarMode mode, Output &output, const CaptureOptions &options);

	void begin_countdown(int seconds); // deferred_ holds the action
	int tick_countdown();
	void cancel_countdown();
	void run_deferred(); // the countdown reached zero

	Server &server_;

	ClipboardImage clipboard_;
	ShotThumbnail thumbnail_;
	ShotTaker shots_;
	Recorder recorder_;
	CapturePicker picker_;
	CaptureToolbar toolbar_;

	Deferred deferred_;
	EventSourcePtr countdown_;

	// The reused per-frame draw buffer overlay() fills.
	CaptureOverlay overlay_;
};
