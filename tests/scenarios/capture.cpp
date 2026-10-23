// End-to-end capture scenario: the key chords (whole screen, region picker,
// window picking with Space, Esc, the Print family, Ctrl for the
// clipboard), the after-shot thumbnail (shown, opened by a click through an
// xdg-open stub, dismissed by a swipe), the capture toolbar (modes and
// options, the timer), the `capture.pick` IPC with its deferred answers,
// the `wayland-canvas msg capture.pick` chooser bridge, and recordings
// (the compositor's own offscreen frames piped into ffmpeg; started,
// stopped by Alt+Print and by `capture.stop`, left as playable files,
// proven by ffprobe, with a command started beside one holding none of
// the compositor's descriptors; and one whose encoder never reads, which
// leaves input and drawing at their pace until the stop ends it). Shots
// are compared against grim pixel for pixel.
// Writes only under $CANVAS_TMP: the XDG pictures and videos dirs point
// into it, and xdg-open is a stub.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <format>
#include <linux/input-event-codes.h>
#include <optional>
#include <ranges>
#include <signal.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include "apps/capture_toolbar_layout.h"
#include "core/config.h"
#include "core/process.h"
#include "core/server.h"
#include "harness/check.h"
#include "harness/child.h"
#include "harness/control.h"
#include "harness/devices.h"
#include "harness/files.h"
#include "harness/image.h"
#include "harness/run.h"
#include "platform/output.h"
#include "windows/view.h"

using namespace harness;

namespace {

Child events; // the events subscriber, running for the whole run
std::string term_geometry; // the window crop the key-leak check compares
std::string stall_bin; // the dir holding the stand-in encoder as `ffmpeg`
std::string pictures_dir; // the scratch XDG pictures dir
std::string videos_dir; // the scratch XDG videos dir
std::chrono::steady_clock::time_point state_asked; // when the stalled state was asked

// grim and the compositor round differently: pixels differ by up to this
// much in a channel and still count as the same.
constexpr int shot_tolerance = 2;

// The command a recording runs beside. The compositor's own spawn starts it
// and double forks, so it is nobody's child; it dies with the scenario
// whatever step the run ends at.
class HeldCommand {
public:
	~HeldCommand() {
		stop();
	}

	void start() {
		pid_ = spawn("sleep 60");
	}

	pid_t pid() const {
		return pid_;
	}

	void stop() {
		if (pid_ > 0) {
			kill(pid_, SIGKILL);
			pid_ = -1;
		}
	}

private:
	pid_t pid_ = -1;
};
HeldCommand held;

// The shot files that carry `prefix` in `dir`, newest first.
std::vector<std::string> files_with(const std::string &dir, std::string_view prefix) {
	std::vector<std::string> out;
	DIR *d = opendir(dir.c_str());
	if (d == nullptr) {
		return out;
	}
	while (dirent *e = readdir(d)) {
		if (e->d_name[0] != '.' && std::string_view{e->d_name}.starts_with(prefix)) {
			out.push_back(dir + "/" + e->d_name);
		}
	}
	closedir(d);
	auto mtime = [](const std::string &path) {
		struct stat st{};
		return stat(path.c_str(), &st) == 0 ? st.st_mtim.tv_sec * 1000000000ll
			+ st.st_mtim.tv_nsec : -1;
	};
	std::ranges::sort(out, [&](const std::string &a, const std::string &b) {
		return mtime(a) > mtime(b);
	});
	return out;
}

// The descriptors a live process holds, sorted; empty when it is gone.
std::vector<std::string> proc_fds(pid_t pid) {
	std::string dir = std::format("/proc/{}/fd", pid);
	std::vector<std::string> out;
	DIR *d = opendir(dir.c_str());
	if (d == nullptr) {
		return out;
	}
	while (dirent *e = readdir(d)) {
		if (e->d_name[0] != '.') {
			out.push_back(e->d_name);
		}
	}
	closedir(d);
	std::ranges::sort(out);
	return out;
}

// Runs a command that needs no compositor interaction to completion, its
// stdout in <scratch>/<out>; its exit status.
int run_sync(const std::string &cmd, const char *out) {
	pid_t pid = fork();
	if (pid == 0) {
		FILE *f = std::fopen((scratch() + "/" + out).c_str(), "w");
		if (f == nullptr || dup2(fileno(f), 1) < 0) {
			_exit(126);
		}
		execl("/bin/sh", "/bin/sh", "-c", cmd.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// ------------------------------------------------------------- pictures

std::string newest_shot_path() {
	std::vector<std::string> files = files_with(pictures_dir + "/Screenshots", "Screenshot");
	return files.empty() ? std::string{} : files.front();
}

// The newest shot file under the pictures dir; empty when there is none (or
// it is unreadable).
std::optional<Image> read_newest_shot() {
	std::string path = newest_shot_path();
	return path.empty() ? std::nullopt : Image::read(path);
}

int shots_saved() {
	return static_cast<int>(files_with(pictures_dir + "/Screenshots", "Screenshot").size());
}

// The box around the pointer's on-screen position, translated into the
// reference image's coordinates (a crop's origin for grim -g shots).
wlr_box cursor_box(double px, double py, int ref_x = 0, int ref_y = 0) {
	return {static_cast<int>(px) - ref_x - 24, static_cast<int>(py) - ref_y - 24, 64, 64};
}

// How many pixels of `got` differ from the shot named `reference`, outside
// `skip` (the cursor's box in the reference, none when the pointer was parked
// outside it); -1 when either is missing or their sizes differ.
long pixels_off(const std::optional<Image> &got, std::string_view reference,
		std::optional<wlr_box> skip = std::nullopt) {
	std::optional<Image> ref = read_shot(reference);
	if (!got.has_value() || !ref.has_value()) {
		return -1;
	}
	std::span<const wlr_box> skipped;
	if (skip.has_value()) {
		skipped = std::span<const wlr_box>{&*skip, 1};
	}
	return got->differing(*ref, {.skip = skipped, .tolerance = shot_tolerance});
}

// A thousandth of the picture may differ.
bool within_budget(long off, long total) {
	return off >= 0 && off * 1000 <= total;
}

// A shot (an offscreen capture, cursorless) against a grim reference: they
// match, at the usual 0.1 % budget, outside `skip`. `pw`/`ph` are the sizes
// the shot must have.
bool shot_matches(const std::optional<Image> &got, std::string_view reference,
		std::optional<wlr_box> skip, int pw, int ph) {
	if (!got.has_value() || got->width() != pw || got->height() != ph) {
		return false;
	}
	return within_budget(pixels_off(got, reference, skip), static_cast<long>(pw) * ph);
}

// Pixels of two grims that differ inside `box`; -1 when one cannot be read.
long grim_differs_inside(std::string_view a, std::string_view b, const wlr_box &box) {
	std::optional<Image> first = read_shot(a), second = read_shot(b);
	if (!first.has_value() || !second.has_value()) {
		return -1;
	}
	return first->differing(*second, {.within = box, .tolerance = shot_tolerance});
}

// ffprobe's csv rows ("W,H" and the duration) joined by the tr in
// run_sync: the size matches and the recording runs at least `min_s`.
bool probe_matches(const std::string &probe, int want_w, int want_h, double min_s) {
	int w = 0, h = 0;
	double dur = 0;
	if (std::sscanf(probe.c_str(), "%d,%d %lf", &w, &h, &dur) != 3) {
		return false;
	}
	return w == want_w && h == want_h && dur >= min_s;
}

// ffprobe of `file`, as run_sync leaves it in <scratch>/<out>; its status.
int probe_recording(const std::string &file, const char *out) {
	return run_sync(std::format(
		"ffprobe -v error -select_streams v:0 "
			"-show_entries stream=width,height -show_entries format=duration "
			"-of csv=p=0 \"{}\" | tr '\\n' ' '", file), out);
}

// The `recording` member of a `msg state` answer: "" when absent, "none"
// when null, else the recording's path.
std::string state_recording(std::string_view name) {
	JsonAnswer answer(command_out(name));
	JsonObject *object = answer.object();
	if (object == nullptr || !json_object_has_member(object, "recording")) {
		return "";
	}
	JsonNode *node = json_object_get_member(object, "recording");
	if (json_node_get_node_type(node) == JSON_NODE_NULL) {
		return "none";
	}
	if (json_node_get_node_type(node) == JSON_NODE_OBJECT) {
		return member_string(json_node_get_object(node), "path");
	}
	return "";
}

// The chooser bridge: `msg capture.pick` fed a chooser list on stdin.
void run_bridge(const char *list, std::string_view name) {
	run_command(std::format("\"{}\" msg capture.pick < {}/{}", std::getenv("WAYLAND_CANVAS_BIN"),
		scratch(), list), name);
}

// ------------------------------------------------------ window helpers

// The view's frame where the output shows it, output-local logical px.
wlr_box view_on_screen(View &view) {
	Camera cam = output().camera.drawn();
	wlr_box frame = view.frame_box();
	Point tl = output().canvas_to_local(frame.x, frame.y);
	return wlr_box{
		.x = static_cast<int>(std::lround(tl.x)),
		.y = static_cast<int>(std::lround(tl.y)),
		.width = static_cast<int>(std::lround(frame.width * cam.zoom)),
		.height = static_cast<int>(std::lround(frame.height * cam.zoom)),
	};
}

// The scenario's one window's frame on screen; empty when it is gone.
wlr_box window_frame() {
	View *view = first_mapped_view();
	return view != nullptr ? view_on_screen(*view) : wlr_box{};
}

void click_title_bar(const wlr_box &frame) {
	click(frame.x + 60.0, frame.y + 14.0);
}

// The toolbar as the compositor lays it out on the first output.
ToolbarLayout toolbar_on_screen() {
	return toolbar_layout(output().width, output().height);
}

void click_mode(ToolbarMode mode) {
	wlr_box box = toolbar_on_screen().mode_box(mode);
	click(box.x + box.width / 2.0, box.y + box.height / 2.0);
}

void click_option(ToolbarOption option) {
	wlr_box box = toolbar_on_screen().option_box(option);
	click(box.x + box.width / 2.0, box.y + box.height / 2.0);
}

// The thumbnail's on-screen box (the newest shot is 1280x720, fit into the
// 240x140 slot), for the appear/dismiss pixel checks.
wlr_box thumb_box() {
	double ar = static_cast<double>(screen_w) / screen_h;
	double w = std::fmin(config::capture_thumbnail_max_w, config::capture_thumbnail_max_h * ar);
	double h = w / ar;
	return {static_cast<int>(std::lround(output().width - config::capture_thumbnail_margin - w)),
		static_cast<int>(std::lround(output().height - config::capture_thumbnail_margin - h)),
		static_cast<int>(std::lround(w)), static_cast<int>(std::lround(h))};
}

// A swipe (a drag) across the thumbnail dismisses it.
void swipe_thumbnail_away() {
	wlr_box box = thumb_box();
	drag(box.x + box.width / 2.0, box.y + box.height / 2.0,
		box.x + box.width / 2.0 - 160, box.y + box.height / 2.0);
}

// ---------------------------------------------------------------- steps

// The fake input devices, and one foot window to frame; the events
// subscriber runs for the whole run.
Result window_and_subscriber() {
	attach_keyboard();
	attach_pointer();
	spawn("foot -T cap sh -c 'echo capture scenario; exec sleep 600'");
	check(events.start({std::getenv("WAYLAND_CANVAS_BIN"), "msg", "subscribe"}),
		"the events subscriber started");
	return after(500);
}

// The window mapped and the camera settled.
Result window_mapped() {
	if (auto wait = wait_for(first_mapped_view() != nullptr && settled())) {
		return *wait;
	}
	View *view = first_mapped_view();
	check(view != nullptr, "one foot window mapped");
	if (view != nullptr) {
		server->focus.focus_view(view);
	}
	return after(2500); // any startup hint clears before the reference shot
}

// The reference: the whole output, cursor parked top-left.
Result reference_shot() {
	move_to(40, 40);
	shot("ref-full");
	return after(400);
}

// Super+Shift+3: the whole screen, saved and thumbnailed.
Result whole_screen_chord() {
	chord(KEY_3, mods::super_shift);
	int saved = shots_saved();
	check(saved == 1, std::format("Super+Shift+3 saved one shot ({})", saved));
	check(shot_matches(read_newest_shot(), "ref-full", cursor_box(40, 40), screen_w, screen_h),
		"the saved shot matches grim of the whole screen");
	return after(2300); // the save hint clears; the thumbnail (5 s) is still up
}

// The thumbnail is on screen, bottom-right.
Result thumbnail_shot() {
	shot("with-thumb");
	return after(400);
}

Result thumbnail_shown() {
	wlr_box box = thumb_box();
	long changed = grim_differs_inside("with-thumb", "ref-full", box);
	check(changed > 500,
		std::format("the shot shows as a bottom-right thumbnail "
			"({} of {} px changed)", changed, static_cast<long>(box.width) * box.height));
	return after(200);
}

// A swipe on the thumbnail dismisses it.
Result swipe_on_thumbnail() {
	swipe_thumbnail_away();
	return after(400);
}

Result thumbnail_gone_shot() {
	shot("thumb-gone");
	return after(400);
}

Result thumbnail_dismissed() {
	wlr_box box = thumb_box();
	long changed = grim_differs_inside("thumb-gone", "with-thumb", box);
	check(changed > 500,
		std::format("a swipe dismissed the thumbnail ({} of {} px changed)",
			changed, static_cast<long>(box.width) * box.height));
	return after(300);
}

// Ctrl+Super+Shift+3: the clipboard instead of a file.
Result clipboard_chord() {
	chord(KEY_3, mods::super_ctrl_shift);
	check(shots_saved() == 1, "the Ctrl chord saved no file");
	run_command(std::format("wl-paste --no-newline | tee {}/pasted-1.png", scratch()), "paste-1");
	return after(400);
}

Result clipboard_pasted() {
	if (auto wait = wait_for(command_done("paste-1"), {.budget_ms = 5000})) {
		return *wait;
	}
	check(command_code("paste-1") == 0, "wl-paste read the clipboard");
	// pasted-1.png lands in the scratch dir; compare as pictures.
	std::optional<Image> pasted = read_shot("pasted-1");
	check(pasted.has_value()
			&& within_budget(pixels_off(pasted, "ref-full", cursor_box(40, 40)),
				static_cast<long>(screen_w) * screen_h),
		"the clipboard holds the shot as PNG, matching grim");
	return after(1200); // a later shot keeps its own second
}

// Super+Shift+4: drag a region.
Result region_reference() {
	move_to(40, 40); // the cursor parks outside the crop
	shot("ref-region", "300,200 400x300");
	return after(400);
}

Result region_picker() {
	chord(KEY_4, mods::super_shift);
	check(server->capture_ui.picking(), "Super+Shift+4 opened the picker");
	shot("picker-region");
	return after(400);
}

Result region_dragged() {
	drag(300, 200, 700, 500);
	check(shots_saved() == 2, "the drag saved a second shot");
	check(shot_matches(read_newest_shot(), "ref-region", std::nullopt, 400, 300),
		"the region shot matches grim's crop of the same area");
	return after(2300); // let the hint clear while the thumbnail is still up
}

// Dismiss that thumbnail, then reference the window alone.
Result dismiss_region_thumbnail() {
	swipe_thumbnail_away();
	return after(400);
}

Result window_reference() {
	View *view = first_mapped_view();
	if (view == nullptr) {
		check(false, "the window is still mapped");
		return after(200);
	}
	wlr_box f = view_on_screen(*view);
	move_to(f.x + 60.0, f.y + 14.0); // the pointer parks on the title bar
	shot("ref-window", std::format("{},{} {}x{}", f.x, f.y, f.width, f.height));
	return after(400);
}

// A click on the window captures the window alone.
Result window_click() {
	View *view = first_mapped_view();
	wlr_box f = window_frame();
	chord(KEY_4, mods::super_shift);
	check(server->capture_ui.picking(), "the picker opened again");
	click_title_bar(f);
	check(shots_saved() == 3, "the click saved a window shot");
	check(view != nullptr
			&& shot_matches(read_newest_shot(), "ref-window",
				cursor_box(f.x + 60, f.y + 14, f.x, f.y), f.width, f.height),
		"the window shot matches grim's crop of its frame, alone");
	return after(2300);
}

// Dismiss, then Space: window picking on purpose.
Result dismiss_window_thumbnail() {
	swipe_thumbnail_away();
	return after(400);
}

Result space_picks_windows() {
	wlr_box f = window_frame();
	chord(KEY_4, mods::super_shift);
	tap(KEY_SPACE);
	click(80, 650); // empty canvas: nothing is picked
	check(server->capture_ui.picking(),
		"Space switched to window picking; empty canvas picks nothing");
	check(shots_saved() == 3, "no shot from a click on the empty canvas");
	click_title_bar(f);
	check(shots_saved() == 4, "clicking the window in Space mode shot it");
	return after(2300);
}

// Esc cancels.
Result dismiss_space_thumbnail() {
	swipe_thumbnail_away();
	return after(400);
}

Result esc_cancels() {
	chord(KEY_4, mods::super_shift);
	check(server->capture_ui.picking(), "the picker is up for Esc");
	tap(KEY_ESC);
	check(!server->capture_ui.picking(), "Esc cancelled the picker");
	check(shots_saved() == 4, "no shot from a cancelled pick");
	return after(300);
}

// Print opens the same picker; Shift+Print takes the screen.
Result print_opens_the_picker() {
	tap(KEY_SYSRQ);
	check(server->capture_ui.picking(), "Print opened the picker");
	tap(KEY_ESC);
	check(!server->capture_ui.picking(), "Esc closed it again");
	return after(1200);
}

Result shift_print() {
	move_to(40, 40);
	chord(KEY_SYSRQ, mods::shift);
	check(shots_saved() == 5, "Shift+Print saved a whole-screen shot");
	check(shot_matches(read_newest_shot(), "ref-full", cursor_box(40, 40), screen_w, screen_h),
		"the Shift+Print shot matches grim of the whole screen");
	return after(2300);
}

// A click opens the shot: the xdg-open stub logs its path.
Result click_opens_the_shot() {
	wlr_box box = thumb_box();
	click(box.x + box.width / 2.0, box.y + box.height / 2.0);
	return after(600);
}

Result xdg_open_ran() {
	std::string path = newest_shot_path();
	std::string opened = read_text(scratch() + "/xdg-open.args");
	while (!opened.empty() && opened.back() == '\n') {
		opened.pop_back();
	}
	check(opened == path, std::format("a click on the thumbnail opened the shot ({})", opened));
	// clean up: dismiss this thumbnail too
	swipe_thumbnail_away();
	return after(400);
}

// The toolbar: Super+Shift+5.
Result toolbar_opens() {
	chord(KEY_5, mods::super_shift);
	check(server->capture_ui.owns_keyboard(), "the toolbar owns the keyboard");
	shot("toolbar");
	return after(400);
}

// Destination to the clipboard, then a screen shot from it.
Result toolbar_clipboard() {
	click_option(ToolbarOption::destination); // clipboard on
	click_mode({ToolbarTarget::screen, false}); // shot the screen
	check(shots_saved() == 5, "the toolbar's clipboard option saved no file");
	run_command(std::format("wl-paste --no-newline | tee {}/pasted-2.png", scratch()), "paste-2");
	return after(400);
}

Result toolbar_clipboard_pasted() {
	if (auto wait = wait_for(command_done("paste-2"), {.budget_ms = 5000})) {
		return *wait;
	}
	check(within_budget(pixels_off(read_shot("pasted-2"), "ref-full", cursor_box(40, 40)),
			static_cast<long>(screen_w) * screen_h),
		"the toolbar's clipboard shot matches grim");
	return after(200);
}

// The timer: 5 s, then a screen shot that waits for it.
Result countdown_starts() {
	chord(KEY_5, mods::super_shift);
	click_option(ToolbarOption::destination); // clipboard off
	click_option(ToolbarOption::timer); // timer 5 s
	click_mode({ToolbarTarget::screen, false}); // shot the screen
	check(server->capture_ui.owns_keyboard(), "the countdown owns the keyboard");
	shot("countdown");
	return after(2000); // inside the countdown: no file yet
}

Result countdown_saved_nothing() {
	check(shots_saved() == 5, "the countdown had not saved anything yet");
	return after(4200); // past the 5 s
}

Result countdown_saved_it() {
	check(shots_saved() == 6, "the shot landed after the countdown");
	return after(600); // its thumbnail will be dismissed with the next picker
}

// capture.pick, region: the answer is deferred until the drag.
Result pick_region_asked() {
	swipe_thumbnail_away();
	run_msg("capture.pick", R"({"what":"region"})", "pick-region");
	return after(400);
}

Result pick_region_answered() {
	if (auto wait = act_when(server->capture_ui.picking(), [] {
				check(server->capture_ui.picking(), "capture.pick opened the picker");
				drag(400, 150, 600, 350);
			}, {.budget_ms = 3000})) {
		return *wait;
	}
	if (auto wait = wait_for(command_done("pick-region"), {.budget_ms = 3000})) {
		return *wait;
	}
	JsonAnswer answer(command_out("pick-region"));
	JsonObject *result = answer.object();
	bool ok = result != nullptr && member_string(result, "output") != ""
		&& member_int(result, "x") == 400 && member_int(result, "y") == 150
		&& member_int(result, "w") == 200 && member_int(result, "h") == 200;
	check(ok, std::format("capture.pick answered the dragged region ({})",
		command_out("pick-region")));
	return after(300);
}

// capture.pick, window: the window's id and identifier.
Result pick_window_asked() {
	run_msg("capture.pick", R"({"what":"window"})", "pick-window");
	return after(400);
}

Result pick_window_answered() {
	if (auto wait = act_when(server->capture_ui.picking(),
			[] { click_title_bar(window_frame()); }, {.budget_ms = 3000})) {
		return *wait;
	}
	View *view = first_mapped_view();
	if (auto wait = wait_for(command_done("pick-window"), {.budget_ms = 3000})) {
		return *wait;
	}
	JsonAnswer answer(command_out("pick-window"));
	JsonObject *result = answer.object();
	bool ok = result != nullptr && view != nullptr
		&& member_int(result, "window") == static_cast<long long>(view->session_id)
		&& !member_string(result, "identifier").empty();
	check(ok, std::format("capture.pick answered the picked window by id "
		"and identifier ({})", command_out("pick-window")));
	return after(300);
}

// capture.pick, output and any.
Result pick_output_asked() {
	run_msg("capture.pick", R"({"what":"output"})", "pick-output");
	return after(400);
}

Result pick_output_answered() {
	if (auto wait = act_when(server->capture_ui.picking(), [] { click(80, 650); },
			{.budget_ms = 3000})) {
		return *wait;
	}
	if (auto wait = wait_for(command_done("pick-output"), {.budget_ms = 3000})) {
		return *wait;
	}
	std::string out = command_out("pick-output");
	check(out.find("\"output\"") != std::string::npos,
		std::format("capture.pick answered the output ({})", out));
	run_msg("capture.pick", R"({"what":"any"})", "pick-any");
	return after(400);
}

// Any: the empty canvas picks the output, a window the window.
Result pick_any_empty_answered() {
	if (auto wait = act_when(server->capture_ui.picking(), [] { click(80, 650); },
			{.budget_ms = 3000})) {
		return *wait;
	}
	if (auto wait = wait_for(command_done("pick-any"), {.budget_ms = 3000})) {
		return *wait;
	}
	std::string out = command_out("pick-any");
	check(out.find("\"output\"") != std::string::npos,
		std::format("capture.pick in any mode picked the output on the empty "
			"canvas ({})", out));
	run_msg("capture.pick", R"({"what":"any"})", "pick-any2");
	return after(400);
}

Result pick_any_window_answered() {
	if (auto wait = act_when(server->capture_ui.picking(),
			[] { click_title_bar(window_frame()); }, {.budget_ms = 3000})) {
		return *wait;
	}
	if (auto wait = wait_for(command_done("pick-any2"), {.budget_ms = 3000})) {
		return *wait;
	}
	std::string out = command_out("pick-any2");
	check(out.find("\"window\"") != std::string::npos,
		std::format("capture.pick in any mode picked the window ({})", out));
	return after(300);
}

// Only one pick at a time.
Result second_pick_asked() {
	chord(KEY_4, mods::super_shift);
	run_msg("capture.pick", R"({"what":"region"})", "pick-busy");
	return after(400);
}

Result second_pick_refused() {
	if (auto wait = wait_for(command_done("pick-busy"), {.budget_ms = 3000})) {
		return *wait;
	}
	check(command_code("pick-busy") == 1
			&& command_err("pick-busy").find("already running") != std::string::npos,
		"a second pick is answered with an error");
	tap(KEY_ESC); // close the key-opened picker
	check(!server->capture_ui.picking(), "Esc closed the key-opened picker");
	return after(300);
}

// A cancel is an error answer.
Result pick_cancel_asked() {
	run_msg("capture.pick", R"({"what":"region"})", "pick-cancel");
	return after(400);
}

Result pick_cancel_answered() {
	if (auto wait = act_when(server->capture_ui.picking(), [] { tap(KEY_ESC); },
			{.budget_ms = 3000})) {
		return *wait;
	}
	if (auto wait = wait_for(command_done("pick-cancel"), {.budget_ms = 3000})) {
		return *wait;
	}
	check(command_code("pick-cancel") == 1
			&& command_err("pick-cancel").find("cancelled") != std::string::npos,
		"cancelling the pick answers with an error");
	return after(300);
}

// Params are checked.
Result bad_pick_asked() {
	run_msg("capture.pick", R"({"what":"nope"})", "pick-bad");
	return after(400);
}

Result bad_pick_refused() {
	if (auto wait = wait_for(command_done("pick-bad"), {.budget_ms = 3000})) {
		return *wait;
	}
	check(command_code("pick-bad") == 1, "a bad `what` is answered with the usage error");
	return after(300);
}

// The identifier the window pick answered with, for the chooser list.
std::string picked_window_identifier() {
	JsonAnswer answer(command_out("pick-window"));
	return member_string(answer.object(), "identifier");
}

// The chooser bridge: both monitors and windows on stdin.
Result bridge_output_asked() {
	std::string name = output().wlr->name;
	std::string identifier = picked_window_identifier();
	if (identifier.empty()) {
		check(false, "the window pick had an identifier for the bridge list");
		return after(200);
	}
	write_file(scratch() + "/chooser.txt", std::format(
		"Monitor: {} the headless output\nWindow: cap ({})\n", name, identifier));
	run_bridge("chooser.txt", "bridge-output");
	return after(400);
}

// The bridge asks `any`; a click on the canvas picks the monitor.
Result bridge_output_answered() {
	if (auto wait = act_when(server->capture_ui.picking(), [] { click(80, 650); },
			{.budget_ms = 3000})) {
		return *wait;
	}
	if (auto wait = wait_for(command_done("bridge-output"), {.budget_ms = 3000})) {
		return *wait;
	}
	std::string name = output().wlr->name;
	check(command_out("bridge-output") == std::format("Monitor: {} the headless output", name)
			&& command_code("bridge-output") == 0,
		"the bridge printed the chosen monitor's line, byte-exactly");
	run_bridge("chooser.txt", "bridge-window");
	return after(400);
}

// The same list, a window chosen.
Result bridge_window_answered() {
	if (auto wait = act_when(server->capture_ui.picking(),
			[] { click_title_bar(window_frame()); }, {.budget_ms = 3000})) {
		return *wait;
	}
	if (auto wait = wait_for(command_done("bridge-window"), {.budget_ms = 3000})) {
		return *wait;
	}
	check(command_out("bridge-window")
			== std::format("Window: cap ({})", picked_window_identifier())
			&& command_code("bridge-window") == 0,
		"the bridge printed the chosen window's line, byte-exactly");
	run_bridge("chooser.txt", "bridge-cancel");
	return after(400);
}

// A cancelled bridge prints nothing and exits 0.
Result bridge_cancel_answered() {
	if (auto wait = act_when(server->capture_ui.picking(), [] { tap(KEY_ESC); },
			{.budget_ms = 3000})) {
		return *wait;
	}
	if (auto wait = wait_for(command_done("bridge-cancel"), {.budget_ms = 3000})) {
		return *wait;
	}
	check(command_out("bridge-cancel").empty() && command_code("bridge-cancel") == 0,
		"a cancelled chooser bridge prints nothing, exiting 0");
	return after(300);
}

// A recording from the toolbar, whole screen.
Result recording_starts() {
	chord(KEY_5, mods::super_shift);
	// the timer option still reads 5 s from the countdown test: cycle
	// it back to none (5 -> 10 -> none)
	click_option(ToolbarOption::timer);
	click_option(ToolbarOption::timer);
	click_mode({ToolbarTarget::screen, true}); // record the screen
	return after(600);
}

Result recording_started() {
	if (auto wait = wait_for(server->capture_ui.recording(), {.budget_ms = 3000})) {
		return *wait;
	}
	std::string path = server->capture_ui.recording_path();
	check(server->capture_ui.recording()
			&& path.starts_with(videos_dir)
			&& path.find("Screen Recording") != std::string::npos,
		std::format("the recording started into the videos dir ({})", path));
	check(events.said().find("capture.recording") != std::string_view::npos,
		"the capture.recording event told the bar");
	run_msg("state", "", "state-recording");
	return after(400);
}

Result state_reports_recording() {
	if (auto wait = wait_for(command_done("state-recording"), {.budget_ms = 3000})) {
		return *wait;
	}
	std::string rec = state_recording("state-recording");
	check(!rec.empty() && rec != "none",
		std::format("the state reports the recording ({})", rec));
	shot("recording-hint");
	// the spawn path a shortcut takes, running beside the recording
	held.start();
	return after(2500); // some footage
}

// The started command holds nothing of the compositor's.
Result spawned_command_holds_nothing() {
	std::vector<std::string> fds = proc_fds(held.pid());
	std::string listed;
	for (const std::string &fd : fds) {
		listed += listed.empty() ? fd : ", " + fd;
	}
	check(fds == std::vector<std::string>{"0", "1", "2"},
		std::format("the command started beside the recording holds only "
			"fds 0, 1, 2 ({})", listed));
	chord(KEY_SYSRQ, mods::alt);
	return after(400);
}

Result alt_print_stopped_it() {
	// ffmpeg finalises the file after the pipe's end: a wide budget
	if (auto wait = wait_for(!server->capture_ui.recording(), {.budget_ms = 10'000})) {
		return *wait;
	}
	check(!server->capture_ui.recording(), "Alt+Print stopped the recording");
	run_msg("state", "", "state-stopped");
	return after(400);
}

Result state_reports_stopped() {
	if (auto wait = wait_for(command_done("state-stopped"), {.budget_ms = 3000})) {
		return *wait;
	}
	check(state_recording("state-stopped") == "none",
		"the state reports no recording any more");
	std::vector<std::string> files = files_with(videos_dir, "Screen Recording");
	check(files.size() == 1 && !files.empty(), "one recording file was left behind");
	if (!files.empty()) {
		int code = probe_recording(files.front(), "ffprobe-screen.txt");
		std::string probe = read_text(scratch() + "/ffprobe-screen.txt");
		check(code == 0 && probe_matches(probe, screen_w, screen_h, 0.5),
			std::format("ffprobe reads the recording as a 1280x720 video ({})", probe));
	}
	held.stop();
	return after(300);
}

// capture.stop on nothing is an error.
Result stop_idle_asked() {
	run_msg("capture.stop", "", "stop-idle");
	return after(400);
}

Result stop_idle_refused() {
	if (auto wait = wait_for(command_done("stop-idle"), {.budget_ms = 3000})) {
		return *wait;
	}
	check(command_code("stop-idle") == 1
			&& command_err("stop-idle").find("not recording") != std::string::npos,
		"capture.stop with nothing running is an error");
	return after(200);
}

// A region recording, stopped through the IPC.
Result region_recording_starts() {
	chord(KEY_5, mods::super_shift);
	click_mode({ToolbarTarget::region, true}); // record a region
	return after(600);
}

Result region_recording_started() {
	if (auto wait = act_when(server->capture_ui.picking(), [] { drag(150, 100, 550, 400); },
			{.budget_ms = 3000})) {
		return *wait;
	}
	if (auto wait = wait_for(server->capture_ui.recording(), {.budget_ms = 3000})) {
		return *wait;
	}
	check(server->capture_ui.recording(), "the region recording started");
	return after(2200); // some footage
}

Result region_stop_asked() {
	run_msg("capture.stop", "", "stop-region");
	return after(400);
}

Result region_recording_stopped() {
	if (auto wait = wait_for(command_done("stop-region") && !server->capture_ui.recording(),
			{.budget_ms = 3000})) {
		return *wait;
	}
	check(command_code("stop-region") == 0, "capture.stop answered true");
	check(!server->capture_ui.recording(), "the region recording stopped");
	std::vector<std::string> files = files_with(videos_dir, "Screen Recording");
	check(files.size() == 2, "two recordings, one per run");
	if (files.size() == 2) {
		probe_recording(files.front(), "ffprobe-region.txt");
		std::string probe = read_text(scratch() + "/ffprobe-region.txt");
		check(probe_matches(probe, 400, 300, 0.5),
			std::format("ffprobe reads the region recording as 400x300 ({})", probe));
	}
	return after(300);
}

// Keys pressed while the picker is up never reach a client.
Result keys_before() {
	move_to(40, 40); // the cursor stays out of the compared crop
	wlr_box f = window_frame();
	check(f.width > 0, "the window is still there to type at");
	term_geometry = std::format("{},{} {}x{}", f.x, f.y, f.width, f.height);
	shot("term-before", term_geometry);
	return after(400);
}

Result keys_while_the_picker_is_up() {
	// Everything the picker consumes: open, Space, letters, Esc, twice.
	chord(KEY_4, mods::super_shift);
	tap(KEY_SPACE);
	tap(KEY_A);
	tap(KEY_ESC);
	tap(KEY_PRINT);
	tap(KEY_ESC);
	shot("term-after", term_geometry);
	return after(400);
}

Result keys_did_not_reach_the_window() {
	std::optional<Image> before = read_shot("term-before");
	std::optional<Image> after_keys = read_shot("term-after");
	long changed = before.has_value() && after_keys.has_value()
		? before->differing(*after_keys, {.tolerance = shot_tolerance}) : -1;
	long total = before.has_value() && after_keys.has_value()
		? static_cast<long>(before->width()) * before->height() : 0;
	// The caret may blink; nothing else may change: no key reached foot.
	check(changed >= 0 && total > 0 && changed * 1000 <= total,
		std::format("no key the picker saw reached the window ({}/{} px)", changed, total));
	return after(300);
}

// A window recording, the rect frozen at pick time.
Result window_recording_starts() {
	chord(KEY_5, mods::super_shift);
	click_mode({ToolbarTarget::window, true}); // record a window
	return after(600);
}

Result window_recording_started() {
	if (auto wait = act_when(server->capture_ui.picking(),
			[] { click_title_bar(window_frame()); }, {.budget_ms = 3000})) {
		return *wait;
	}
	if (auto wait = wait_for(server->capture_ui.recording(), {.budget_ms = 3000})) {
		return *wait;
	}
	check(server->capture_ui.recording(), "the window recording started");
	return after(2200); // some footage
}

Result window_stop_asked() {
	run_msg("capture.stop", "", "stop-window");
	return after(400);
}

Result window_recording_stopped() {
	if (auto wait = wait_for(command_done("stop-window") && !server->capture_ui.recording(),
			{.budget_ms = 3000})) {
		return *wait;
	}
	check(command_code("stop-window") == 0, "capture.stop closed the window recording");
	std::vector<std::string> files = files_with(videos_dir, "Screen Recording");
	check(files.size() == 3, "three recordings, one per run");
	if (files.size() == 3) {
		probe_recording(files.front(), "ffprobe-window.txt");
		std::string probe = read_text(scratch() + "/ffprobe-window.txt");
		View *view = first_mapped_view();
		wlr_box f = window_frame();
		// the encoder crops odd edges to whole yuv420p macroblocks
		check(view != nullptr && probe_matches(probe, f.width - f.width % 2,
				f.height - f.height % 2, 0.5),
			std::format("ffprobe reads the window recording as its frame ({})", probe));
	}
	return after(300);
}

// An encoder that never reads: the compositor must not wait for it.
Result stalled_recording_starts() {
	std::string path = std::getenv("PATH");
	put_first_on_path(stall_bin);
	chord(KEY_5, mods::super_shift);
	click_mode({ToolbarTarget::screen, true}); // record the screen
	setenv("PATH", path.c_str(), 1); // the encoder is started by now
	return after(600);
}

Result stalled_recording_started() {
	if (auto wait = wait_for(server->capture_ui.recording(), {.budget_ms = 3000})) {
		return *wait;
	}
	check(server->capture_ui.recording(),
		"the recording with an encoder that reads nothing started");
	return after(2500); // 150 frames come due; the pipe takes 64 KiB of the first
}

// The compositor still answers.
Result state_answers_while_stalled() {
	act_when(true, [] {
		run_msg("state", "", "state-stalled");
		state_asked = std::chrono::steady_clock::now();
	});
	if (auto wait = wait_for(command_done("state-stalled"), {.poll_ms = 20, .budget_ms = 3000})) {
		return *wait;
	}
	long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - state_asked).count();
	check(command_done("state-stalled") && ms <= 500, std::format(
		"IPC state answered in {} ms while the encoder read nothing", ms));
	std::string rec = state_recording("state-stalled");
	check(rec.starts_with(videos_dir) && rec.ends_with(".mp4"),
		std::format("the state reports the stalled recording ({})", rec));
	std::size_t dropped = server->capture_ui.recording_dropped_frames();
	check(dropped > 0, std::format("the frames that came due behind the "
		"encoder were dropped and counted ({})", dropped));
	shot("stalled");
	return after(400);
}

// The screen is still drawn: grim gets its frame.
Result grim_gets_its_frame() {
	check(file_exists(shot_path("stalled")),
		"grim captured the screen while the encoder stalled");
	run_msg("capture.stop", "", "stop-stalled");
	return after(400);
}

// The stop ends it, the encoder not having read a byte.
Result stop_ends_the_stalled_recording() {
	if (auto wait = wait_for(command_done("stop-stalled") && !server->capture_ui.recording(),
			{.budget_ms = 10'000})) {
		return *wait;
	}
	check(command_code("stop-stalled") == 0, "capture.stop answered true");
	check(!server->capture_ui.recording(),
		"the stop ended the recording whose encoder never read");
	std::string report = read_text(scratch() + "/recorder-report.txt");
	check(report.find("started args=") != std::string::npos
			&& report.find("-f rawvideo") != std::string::npos,
		"the stand-in was the encoder, started as ffmpeg raw video");
	int unread = 0, capacity = 0;
	std::size_t at = report.find("unread=");
	check(at != std::string::npos
			&& std::sscanf(report.c_str() + at, "unread=%d capacity=%d",
				&unread, &capacity) == 2 && unread > 0 && unread <= capacity,
		std::format("the pipe was closed on the encoder with the written "
			"frames unread ({} of {} bytes)", unread, capacity));
	return after(300);
}

// The end: nothing recording, the event closed it.
Result the_end() {
	check(!server->capture_ui.recording(), "no recording is left running");
	check(events.said().find("\"active\":false") != std::string_view::npos,
		"the capture.recording event closed the recording");
	return after(0);
}

Result quit() {
	return quit_by_msg();
}

} // namespace

int main() {
	if (!begin("capture", {"WAYLAND_CANVAS_BIN", "CANVAS_RECORDER_STANDIN"})) {
		return 2;
	}
	// Nothing touches the real Pictures, Videos or clipboard: both user
	// dirs point into the scratch tree, and xdg-open is a stub that logs
	// what it was asked to open. glib caches the user dirs, so this has to
	// happen before the server exists.
	pictures_dir = scratch_dir("pictures");
	videos_dir = scratch_dir("videos");
	write_file(scratch_dir("xdg-config") + "/user-dirs.dirs", std::format(
		"XDG_PICTURES_DIR=\"{}\"\nXDG_VIDEOS_DIR=\"{}\"\n", pictures_dir, videos_dir));
	std::string bin = scratch_dir("bin");
	write_script(bin + "/xdg-open", std::format(
		"#!/bin/sh\nprintf '%s\\n' \"$1\" > {}/xdg-open.args\n", scratch()));
	put_first_on_path(bin);
	// The stand-in encoder the stalled recording runs: an ffmpeg that never
	// reads its stdin (tests/scenarios/recorder-standins/ffmpeg), first on
	// PATH only while that recording starts, and a report it writes.
	stall_bin = scratch_dir("stall-bin");
	if (symlink(std::getenv("CANVAS_RECORDER_STANDIN"), (stall_bin + "/ffmpeg").c_str()) != 0) {
		std::fprintf(stderr, "capture: cannot link the stand-in encoder\n");
		return 2;
	}
	setenv("CANVAS_RECORDER_REPORT", (scratch() + "/recorder-report.txt").c_str(), 1);
	Options options;
	options.start_bar = false; // the suites never spawn the shell
	return run(options, {
		{"window and subscriber", window_and_subscriber},
		{"window mapped", window_mapped},
		{"reference shot", reference_shot},
		{"whole screen chord", whole_screen_chord},
		{"thumbnail shot", thumbnail_shot},
		{"thumbnail shown", thumbnail_shown},
		{"swipe on the thumbnail", swipe_on_thumbnail},
		{"thumbnail gone shot", thumbnail_gone_shot},
		{"thumbnail dismissed", thumbnail_dismissed},
		{"clipboard chord", clipboard_chord},
		{"clipboard pasted", clipboard_pasted},
		{"region reference", region_reference},
		{"region picker", region_picker},
		{"region dragged", region_dragged},
		{"dismiss the region thumbnail", dismiss_region_thumbnail},
		{"window reference", window_reference},
		{"window click", window_click},
		{"dismiss the window thumbnail", dismiss_window_thumbnail},
		{"space picks windows", space_picks_windows},
		{"dismiss the space thumbnail", dismiss_space_thumbnail},
		{"esc cancels", esc_cancels},
		{"print opens the picker", print_opens_the_picker},
		{"shift print", shift_print},
		{"click opens the shot", click_opens_the_shot},
		{"xdg-open ran", xdg_open_ran},
		{"toolbar opens", toolbar_opens},
		{"toolbar clipboard", toolbar_clipboard},
		{"toolbar clipboard pasted", toolbar_clipboard_pasted},
		{"countdown starts", countdown_starts},
		{"countdown saved nothing", countdown_saved_nothing},
		{"countdown saved it", countdown_saved_it},
		{"pick region asked", pick_region_asked},
		{"pick region answered", pick_region_answered},
		{"pick window asked", pick_window_asked},
		{"pick window answered", pick_window_answered},
		{"pick output asked", pick_output_asked},
		{"pick output answered", pick_output_answered},
		{"pick any, empty canvas", pick_any_empty_answered},
		{"pick any, window", pick_any_window_answered},
		{"second pick asked", second_pick_asked},
		{"second pick refused", second_pick_refused},
		{"pick cancel asked", pick_cancel_asked},
		{"pick cancel answered", pick_cancel_answered},
		{"bad pick asked", bad_pick_asked},
		{"bad pick refused", bad_pick_refused},
		{"bridge output asked", bridge_output_asked},
		{"bridge output answered", bridge_output_answered},
		{"bridge window answered", bridge_window_answered},
		{"bridge cancel answered", bridge_cancel_answered},
		{"recording starts", recording_starts},
		{"recording started", recording_started},
		{"state reports recording", state_reports_recording},
		{"spawned command holds nothing", spawned_command_holds_nothing},
		{"alt print stopped it", alt_print_stopped_it},
		{"state reports stopped", state_reports_stopped},
		{"stop idle asked", stop_idle_asked},
		{"stop idle refused", stop_idle_refused},
		{"region recording starts", region_recording_starts},
		{"region recording started", region_recording_started},
		{"region stop asked", region_stop_asked},
		{"region recording stopped", region_recording_stopped},
		{"keys before", keys_before},
		{"keys while the picker is up", keys_while_the_picker_is_up},
		{"keys did not reach the window", keys_did_not_reach_the_window},
		{"window recording starts", window_recording_starts},
		{"window recording started", window_recording_started},
		{"window stop asked", window_stop_asked},
		{"window recording stopped", window_recording_stopped},
		{"stalled recording starts", stalled_recording_starts},
		{"stalled recording started", stalled_recording_started},
		{"state answers while stalled", state_answers_while_stalled},
		{"grim gets its frame", grim_gets_its_frame},
		{"stop ends the stalled recording", stop_ends_the_stalled_recording},
		{"the end", the_end},
		{"quit", quit},
	});
}
