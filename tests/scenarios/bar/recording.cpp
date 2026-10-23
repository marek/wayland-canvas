// The recording dot.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

constexpr uint32_t attention_light = 0xf52a65ff; // UiTheme.attention, light

// This phase's one recording, and where its dot sat on the bar.
std::string dot_recording;

bool dot_acted = false; // a step's chord or click has happened (retry-safe)

// The capture toolbar's mode button, its click point: the layout the real
// toolbar draws itself with.
std::pair<double, double> capture_mode_click(ToolbarMode mode) {
	wlr_box box = toolbar_layout(screen_w, screen_h).mode_box(mode);
	return {box.x + box.width / 2.0, box.y + box.height / 2.0};
}

// The attention-coloured pixels inside the bar strip: how many, and their
// bounding box (empty when none). The recording dot is the only thing that
// wears the attention colour on this bar — a widget it displaces can slide
// under where it sits, so runs are read by colour, not by what moved.
std::pair<int, wlr_box> attention_span(cairo_surface_t *s, const wlr_box &bar,
		uint32_t want) {
	int count = 0;
	wlr_box box{0, 0, 0, 0};
	if (s == nullptr) {
		return {count, box};
	}
	for (int y = bar.y; y < bar.y + bar.height; y++) {
		for (int x = bar.x; x < bar.x + bar.width; x++) {
			if (!pixel_is(s, x, y, want)) {
				continue;
			}
			if (count == 0) {
				box = {x, y, 1, 1};
			} else {
				if (x < box.x) {
					box.width += box.x - x;
					box.x = x;
				}
				if (y < box.y) {
					box.height += box.y - y;
					box.y = y;
				}
				if (x + 1 > box.x + box.width) {
					box.width = x + 1 - box.x;
				}
				if (y + 1 > box.y + box.height) {
					box.height = y + 1 - box.y;
				}
			}
			count++;
		}
	}
	return {count, box};
}

// The recording dot on a shot of the bar: its pixels and box. The palette
// this run ended in, read off the bar's own background at its right edge,
// picks the attention colour the dot must wear.
std::pair<int, wlr_box> dot_in(cairo_surface_t *s) {
	LayerSurface *bar = bar_layer();
	if (s == nullptr || bar == nullptr) {
		return {0, wlr_box{0, 0, 0, 0}};
	}
	bool dark = pixel_is(s, screen_w - 3, bar->geo.y + bar->geo.height / 2, shell_bg_dark);
	return attention_span(s, bar->geo, dark ? attention_dark : attention_light);
}

// Whether ffprobe reads the recording as a playing file of the screen's
// size; `probe` is what it printed.
bool recording_plays(std::string &probe) {
	bool probed = std::system(std::format(
		"ffprobe -v error -select_streams v:0 "
			"-show_entries stream=width,height -show_entries format=duration "
			"-of csv=p=0 \"{}\" > {}/dot-probe.txt 2>&1",
		dot_recording, shot_dir).c_str()) == 0;
	int w = 0, h = 0;
	double dur = 0;
	probe = read_file(std::format("{}/dot-probe.txt", shot_dir));
	return probed && std::sscanf(probe.c_str(), "%d,%d %lf", &w, &h, &dur) == 3
		&& w == screen_w && h == screen_h && dur >= 0.5;
}

// Shots `name` on the first call and reports true on the next, a tick later, when grim has
// finished; the call after that shoots afresh, so a retry loop re-shoots by itself.
std::string shot_pending;

} // namespace

double dot_x = 0;

bool shot_landed(const char *name) {
	if (shot_pending != name) {
		shot(name);
		shot_pending = name;
		return false;
	}
	shot_pending.clear();
	return true;
}

std::vector<harness::Step> recording_steps() {
	return {
		{"recording_quiet_bar_the_pointer_parked", [] {
			move_to(640, 400);
			dot_acted = false;
			return shot_landed("norec-bar") ? after(400) : again(100);
		}},
		{"recording_alt_print", [] {
			if (!dot_acted) {
				chord(KEY_SYSRQ, mods::alt);
				dot_acted = true;
			}
			if (auto wait = wait_for(!(!server->capture_ui.owns_keyboard()), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(server->capture_ui.owns_keyboard(),
				"Alt+Print opened the capture toolbar");
			auto [mx, my] = capture_mode_click({ToolbarTarget::screen, true});
			click(mx, my); // record the screen
			dot_acted = false; // the flag next guards the dot's click, not this one
			return after(600);
		}},
		{"recording_runs_into_the_scratch", [] {
			if (auto wait = wait_for(!(!server->capture_ui.recording()), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			dot_recording = server->capture_ui.recording_path();
			check(server->capture_ui.recording() && !dot_recording.empty()
					&& dot_recording.rfind(scratch_home, 0) == 0
					&& dot_recording.find("/Videos/") != std::string::npos,
				std::format("the toolbar's click records the screen, into the "
					"bar world's scratch Videos ({})", dot_recording));
			return after(0);
		}},
		{"recording_dot", [] {
			if (auto wait = shot_holds("rec-bar", [](cairo_surface_t *s) { return dot_in(s).first > 15; })) {
				return *wait;
			}
			cairo_surface_t *later = load_png("rec-bar");
			auto [red, dot] = dot_in(later);
			check(red > 15,
				std::format("the recording dot is on the bar, in the attention "
					"colour ({} px)", red));
			check(red > 15 && dot.x + dot.width / 2 > screen_w / 2,
				"the dot sits in the bar's right section");
			if (red > 0) {
				dot_x = dot.x + dot.width / 2.0;
			}
			if (later != nullptr) {
				cairo_surface_destroy(later);
			}
			return after(400);
		}},
		{"recording_click_on_the_dot_stops", [] {
			if (!dot_acted) { // once: a re-click could open an indicator's panel
				click(dot_x, bar_mid_y());
				dot_acted = true;
			}
			if (auto wait = wait_for(!(server->capture_ui.recording()), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(!server->capture_ui.recording(),
				"a click on the dot stopped the recording");
			run_msg("state", "", "dot-state");
			return after(400);
		}},
		{"recording_state_agrees", [] {
			if (auto wait = wait_for(msg_done("dot-state"), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			JsonParser *dot_state_parser = json_parser_new();
			bool dot_state_read = json_parser_load_from_file(dot_state_parser,
				(shot_dir + "/dot-state.out").c_str(), nullptr);
			JsonObject *dot_state = dot_state_read
				&& json_node_get_node_type(json_parser_get_root(dot_state_parser))
					== JSON_NODE_OBJECT
				? json_node_get_object(json_parser_get_root(dot_state_parser))
				: nullptr;
			check(msg_code("dot-state") == 0 && dot_state != nullptr
					&& json_object_has_member(dot_state, "recording")
					&& json_node_get_node_type(json_object_get_member(dot_state,
						"recording")) == JSON_NODE_NULL,
				"the state reports no recording any more");
			g_object_unref(dot_state_parser);
			return after(0);
		}},
		{"recording_dot_gone", [] {
			if (auto wait = shot_holds("stopped-bar", [](cairo_surface_t *s) { return dot_in(s).first == 0; })) {
				return *wait;
			}
			cairo_surface_t *later = load_png("stopped-bar");
			int red = dot_in(later).first;
			check(red == 0,
				std::format("the dot left the bar with the recording ({} red px)",
					red));
			if (later != nullptr) {
				cairo_surface_destroy(later);
			}
			return after(0);
		}},
		{"recording_file_playable", [] {
			std::string probe;
			bool played = recording_plays(probe);
			if (auto wait = wait_for(played, {.poll_ms = 400, .budget_ms = 15000})) {
				return *wait;
			}
			check(played,
				std::format("ffprobe reads the recording as a playing {}x{} file ({})",
					screen_w, screen_h, probe));
			return after(100);
		}},
	};
}

} // namespace bar
