#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon.h>
#include "core/util.h"

class Output;
class Server;
class View;

// One drawn rectangle of the capture overlay, output-local logical px.
struct CaptureFill {
	wlr_box box{};
	uint32_t rgba = 0;
};

// A hollow rectangle: four thin fills of `thickness` logical px.
struct CaptureOutline {
	wlr_box box{};
	uint32_t rgba = 0;
	double thickness = 1.0;
};

// A texture placed on the output, output-local logical px.
struct CaptureImage {
	wlr_texture *texture = nullptr;
	wlr_box box{};
};

// Everything the capture UI draws on one output this frame; the painter
// turns each entry into a render-pass primitive.
struct CaptureOverlay {
	std::vector<CaptureFill> fills;
	std::vector<CaptureOutline> outlines;
	std::vector<CaptureImage> images;
};

// Where a shot goes and how it is taken; the toolbar owns one set of these
// as its visible options, the key chords pass one-shot sets. Outside the
// class: a nested class's member initializers cannot be used from the
// enclosing class's own member declarations.
struct CaptureOptions {
	bool clipboard = false; // copy as image/png only: no file, no thumbnail
	int timer_s = 0; // countdown before the capture or recording starts
	bool show_pointer = false; // the cursor composited into shots
};

// The shots-and-recordings UI the compositor draws itself: the picker
// overlay (a region dragged, a window or an output clicked), the capture
// toolbar, the after-shot thumbnail, and the recorder's life cycle. Shots
// are taken through the shared offscreen captures (src/render/capture.h);
// a recording is those same offscreen captures piped as raw frames into
// ffmpeg, the one encoder, so it works nested and from a TTY alike. While
// the picker, the toolbar or a countdown is up, the capture UI owns the
// keyboard and the pointer.
class CaptureUi {
public:
	explicit CaptureUi(Server &server);
	~CaptureUi();
	CaptureUi(const CaptureUi &) = delete;
	CaptureUi &operator=(const CaptureUi &) = delete;

	// What the picker is after; `any` offers windows and outputs alike.
	enum class PickWhat { region, window, output, any };
	// What happens to the finished pick.
	enum class PickUse { ipc, shot, record };

	// Where a shot goes and how it is taken (see CaptureOptions).
	using Options = CaptureOptions;


	// ---- the picker ----

	// True while the picker overlay is up.
	bool picking() const { return picker_.active; }

	// Opens the picker on `output` (the output under the cursor when null);
	// false when there is no output to pick on or a pick already runs. An
	// ipc pick answers the pending `capture.pick` request; a shot or record
	// pick uses the given options.
	bool start_pick(Output *output, PickWhat what, PickUse use, Options options);

	// ---- the toolbar ----

	// Opens the capture toolbar on `output` (under the cursor when null);
	// a second open replaces it. `recording_focus` opens it with the
	// recording choices accented (Alt+Print with nothing running); a
	// `preset` with clipboard turns the destination option to the
	// clipboard (the Ctrl chords).
	void open_toolbar(Output *output, bool recording_focus,
		const Options &preset = {});

	// ---- input ----

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
	// The cursor image the picker wants (crosshair while framing a region).
	const char *cursor_image() const;

	// ---- key-chord actions (bindings.cpp) ----

	// A shot of the whole output now.
	void shoot_screen(Output &output, Options options);
	// The Super+Shift+4 / Print picker: drag a region, click a window.
	void pick_for_shot(Output *output, Options options);
	// Alt+Print: stop the recording, else the toolbar on its recordings.
	void stop_recording_or_toolbar(Output *output, Options options);

	// ---- recording ----

	bool recording() const { return recorder_pid_ > 0; }
	const std::string &recording_path() const { return recorder_path_; }
	// Stops the running recorder (a no-op when none runs); the recording
	// event fires when the process has left its file finished.
	void stop_recording();

	// ---- rendering ----

	// What the capture UI draws on `output` this frame, at `scale` (the
	// output's): the picker overlay, the toolbar, the thumbnail. Rebuilt
	// into a reused buffer; empty when nothing is up.
	const CaptureOverlay &overlay(Output &output, float scale);

	// ---- lifecycle ----

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
	// A finished pick.
	struct Pick {
		enum class Kind { region, window, output };
		Kind kind = Kind::region;
		Output *output = nullptr; // every kind
		wlr_box area{}; // region: output-local logical
		View *view = nullptr; // window
	};

	// What the picker is doing right now.
	struct Picker {
		bool active = false;
		Output *output = nullptr; // where a region is dragged
		PickWhat what = PickWhat::region;
		bool window_mode = false; // Space: windows instead of a region
		PickUse use = PickUse::ipc;
		Options options{};
		bool pressed = false;
		bool dragging = false;
		double press_x = 0, press_y = 0; // output-local logical
		Output *cur_output = nullptr; // the output under the pointer
		double cur_x = 0, cur_y = 0;
		View *hover = nullptr; // window mode
	};

	// The action waiting out a countdown.
	struct Deferred {
		enum class Kind { none, shot, record };
		Kind kind = Kind::none;
		Output *output = nullptr;
		std::optional<wlr_box> area; // region: output-local logical
		View *view = nullptr; // shot of one window
		Options options{};
		int seconds_left = 0;
	};

	// The toolbar's on-screen layout, shared by drawing and hit-testing.
	struct ToolbarLayout {
		wlr_box box{};
		wlr_box modes[6]{}; // 0..2 shots, 3..5 recordings
		wlr_box options[3]{}; // destination, timer, pointer
		wlr_box records{}; // the three recording buttons together
	};

	// The clipboard's PNG, offered as image/png until replaced.
	struct ClipSource;

	// ---- the picker ----
	void close_picker();
	void cancel_picker(); // close + answer a pending ipc pick "cancelled"
	void complete_pick(const Pick &pick);
	void answer_ipc(const Pick &pick); // the pick as `capture.pick` answers
	void release_pick(Output &output, double ox, double oy, bool dragged);
	View *window_at(Output &output, double ox, double oy) const;

	// ---- the toolbar ----
	void close_toolbar_only();
	ToolbarLayout toolbar_layout(const Output &output) const;
	int toolbar_hit(const ToolbarLayout &layout, double ox, double oy) const;
	void toolbar_activate(int index);

	// ---- shots ----
	void take_shot(Output &output, const std::optional<wlr_box> &area, View *view,
		const Options &options);
	void composite_cursor(Output &output, cairo_surface_t *shot,
		const wlr_box &on_screen);
	void copy_clipboard(std::vector<std::uint8_t> png);
	void clip_gone(); // the seat destroyed our clipboard source

	// ---- recording ----
	void start_recording(Output &output, const std::optional<wlr_box> &area,
		const Options &options);
	int reap_recorder(); // polls the recorder while it runs
	int push_frame(); // pipes one offscreen capture to the recorder
	void recorder_gone(int status); // the process has exited

	// ---- the countdown ----
	void begin_countdown(int seconds); // deferred_ holds the action
	int tick_countdown();
	void cancel_countdown();
	void run_deferred(); // the countdown reached zero

	// ---- the thumbnail ----
	void show_thumbnail(const std::string &path, Output &output,
		cairo_surface_t *shot);
	void dismiss_thumbnail();
	bool thumbnail_motion(Output &output, double ox, double oy);
	bool thumbnail_button(Output &output, double ox, double oy, uint32_t button,
		bool pressed);

	// ---- overlay building ----
	void pick_overlay(Output &output, float scale);
	void dim_around(const wlr_box &box, const Output &output, uint32_t rgba);
	void toolbar_overlay(Output &output, float scale);
	void thumbnail_overlay(Output &output, float scale);
	TexturePtr draw_toolbar(const Output &output, const ToolbarLayout &layout);

	Server &server_;

	Picker picker_;
	Deferred deferred_;

	// Toolbar.
	bool toolbar_ = false;
	Output *toolbar_output_ = nullptr;
	bool record_focus_ = false;
	Options options_{}; // the toolbar's visible options
	int hover_ = -1; // toolbar button index under the pointer
	int pressed_ = -1;

	// Recording. The pipe carries BGRA frames to ffmpeg's stdin; the
	// frame timer produces them from the offscreen captures.
	pid_t recorder_pid_ = -1;
	int recorder_fd_ = -1;
	std::string recorder_path_;
	Output *recorder_output_ = nullptr;
	wlr_box recorder_area_{}; // what is recorded, output-local logical
	EventSourcePtr reaper_;
	EventSourcePtr frame_timer_;

	// Thumbnail.
	TexturePtr thumb_texture_;
	std::string thumb_path_;
	Output *thumb_output_ = nullptr;
	wlr_box thumb_box_{};
	EventSourcePtr thumb_timer_;
	bool thumb_pressed_ = false;
	double thumb_press_x_ = 0, thumb_press_y_ = 0;

	// Countdown timer.
	EventSourcePtr countdown_;

	// The clipboard source, null while the selection is not ours.
	ClipSource *clip_ = nullptr;

	// Cached overlay textures: rebuilt when stale or the scale changes.
	TexturePtr size_texture_; // the drag size label
	std::string size_text_;
	float size_scale_ = 0;
	int size_w_ = 0, size_h_ = 0;
	TexturePtr toolbar_texture_;
	float toolbar_scale_ = 0;
	wlr_box toolbar_box_{};
	bool toolbar_stale_ = true;

	// The reused per-frame draw buffer overlay() fills.
	CaptureOverlay overlay_;
};
