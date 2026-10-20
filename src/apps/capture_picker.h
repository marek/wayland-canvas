#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "apps/capture_options.h"
#include "apps/capture_overlay.h"
#include "core/util.h"

class Output;
class Server;
class View;

// What the picker is after; `any` offers windows and outputs alike.
enum class PickWhat { region, window, output, any };
// What happens to the finished pick.
enum class PickUse { ipc, shot, record };

// The picker overlay: a region dragged, or a window or an output clicked.
// It frames the pick and reports the finished one; what becomes of it is
// the caller's.
class CapturePicker {
public:
	// A finished pick.
	struct Pick {
		enum class Kind { region, window, output };
		Kind kind = Kind::region;
		Output *output = nullptr; // every kind
		wlr_box area{}; // region: output-local logical
		View *view = nullptr; // window
	};

	// A pick finished: the picker is already closed. Given the use and the
	// options the pick was opened with.
	using Picked = std::function_ref<void(const Pick &, PickUse, const CaptureOptions &)>;

	explicit CapturePicker(Server &server);
	CapturePicker(const CapturePicker &) = delete;
	CapturePicker &operator=(const CapturePicker &) = delete;

	bool active() const { return state_.active; }
	// Whether the pick is on `output` or has the pointer there.
	bool uses(const Output &output) const;

	// Opens the picker on `output`; the caller checks none is open.
	void start(Output &output, PickWhat what, PickUse use, const CaptureOptions &options);
	// Closes the picker: the use of the pick it was open for, if it was.
	std::optional<PickUse> close();

	// Space: windows instead of a region, and back. False when this pick
	// is not a region pick.
	bool toggle_window_mode();

	// Pointer at output-local logical (ox, oy). True when the event is
	// consumed; a region stays on its own output.
	bool pointer_motion(Output &output, double ox, double oy);
	bool pointer_button(Output &output, double ox, double oy, uint32_t button,
		bool pressed, Picked picked);
	// Whether the picker claims the pointer on `output`.
	bool takes_pointer_at(const Output &output) const;
	// The cursor image the picker wants (crosshair while framing a region).
	const char *cursor_image() const;

	// A view went away: the picker stops hovering it.
	void forget(const View &view);

	// The dimmed screen, selection, size label and frames onto `out`.
	void add_overlay(Output &output, float scale, CaptureOverlay &out);

private:
	// What the picker is doing right now.
	struct State {
		bool active = false;
		Output *output = nullptr; // where a region is dragged
		PickWhat what = PickWhat::region;
		bool window_mode = false; // Space: windows instead of a region
		PickUse use = PickUse::ipc;
		CaptureOptions options{};
		bool pressed = false;
		bool dragging = false;
		double press_x = 0, press_y = 0; // output-local logical
		Output *cur_output = nullptr; // the output under the pointer
		double cur_x = 0, cur_y = 0;
		View *hover = nullptr; // window mode
	};

	bool spans_outputs() const; // window and output picks follow the pointer
	View *window_at(Output &output, double ox, double oy) const;
	void complete(const Pick &pick, Picked picked);
	void release(Output &output, double ox, double oy, bool dragged, Picked picked);

	Server &server_;

	State state_;
};
