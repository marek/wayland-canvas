// The sticky notes plugin end to end.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

constexpr const char *note_green = "#a6e3a1";

// The output-local point the pointer is parked at before a note is made,
// so its centring on the cursor can be checked: comfortably inside the
// free area of the 1280x720 screen under its bar, so nothing moves the
// note from where the cursor centres it.
constexpr double note_cursor_x = 500;

constexpr double note_cursor_y = 350;

std::string second_note; // the note the formatting is checked on

int note_ink_plain = 0;

int note_ink_word = 0; // the line's ink before the bold
int note_plain_extent = 0; // the line's ink height at the note's own size
uint32_t note_b_idle = 0; // the strip's B, off, just left of its glyph
wlr_box note_before{}; // the note's rectangle before a drag

// The plugin's notes, one plain-HTML file each in its scratch storage:
// `<id>.html` (docs/intents/shell/notes.md).
std::string notes_dir() {
	return shot_dir + "/xdg/state/wayland-canvas/plugins/canvas.notes";
}

// A note's file read back: the rectangle from its canvas-note meta
// element, the colour, font and size from the page's style, the text from
// its body.
NoteEntry note_from_file(const std::string &id, const std::string &file) {
	NoteEntry entry;
	entry.id = id;
	std::smatch found;
	static const std::regex meta{R"re(<meta name="canvas-note" content="([^"]*)">)re"};
	if (std::regex_search(file, found, meta)) {
		std::string content = found[1];
		static const std::regex pair{R"((\w+)=(-?\d+))"};
		for (std::sregex_iterator it(content.begin(), content.end(), pair), end;
				it != end; ++it) {
			std::string key = (*it)[1];
			long long value = std::stoll((*it)[2]);
			(key == "x" ? entry.x : key == "y" ? entry.y
				: key == "width" ? entry.width : entry.height) = value;
		}
	}
	static const std::regex background{R"(background:\s*(#[0-9a-f]{6}))"};
	if (std::regex_search(file, found, background)) {
		entry.color = found[1];
	}
	static const std::regex family{R"re(font-family:\s*"([^"]*)")re"};
	if (std::regex_search(file, found, family)) {
		entry.font = found[1];
	}
	static const std::regex size{R"(font-size:\s*(\d+)px)"};
	if (std::regex_search(file, found, size)) {
		entry.size = std::stoll(found[1]);
	}
	static const std::regex body{R"(<body>\n?([\s\S]*?)\n?</body>)"};
	if (std::regex_search(file, found, body)) {
		entry.text = found[1];
	}
	return entry;
}

std::vector<NoteEntry> stored_notes() {
	std::vector<NoteEntry> out;
	std::error_code error;
	for (const auto &file : std::filesystem::directory_iterator(notes_dir(), error)) {
		std::string name = file.path().filename().string();
		static const std::regex html{R"(^(\d+)\.html$)"};
		std::smatch found;
		if (std::regex_match(name, found, html)) {
			out.push_back(note_from_file(found[1], read_file(file.path().string())));
		}
	}
	std::ranges::sort(out, {}, [](const NoteEntry &e) { return std::stoll(e.id); });
	return out;
}

std::optional<NoteEntry> stored_note(const std::string &id) {
	for (const NoteEntry &entry : stored_notes()) {
		if (entry.id == id) {
			return entry;
		}
	}
	return std::nullopt;
}

int note_surfaces() {
	return static_cast<int>(std::ranges::count_if(server->canvas_surfaces.bottom_up(),
		[](const LayerSurface *l) { return is_note_surface(*l); }));
}

std::vector<Run> note_runs; // the second note's strip controls, off its last shot

void click_note_body(std::string_view id) {
	Point p = note_local(id, 30, note_strip + 10);
	click(p.x, p.y);
}

void click_note_dot(std::string_view id, int index) {
	Point p = note_local(id, 6 + 12 * index + 4, note_strip / 2.0);
	click(p.x, p.y);
}

// The ink of the strip read off a shot, left to right past the dots: the
// bold, italic and underline buttons, the font, the size and the ×, each
// a run of dark columns along the strip's middle.
std::vector<Run> note_strip_runs(cairo_surface_t *s, std::string_view id) {
	std::vector<Run> runs;
	Point from = note_local(id, 6 + 6 * 12, 7);
	Point to = note_local(id, -1, note_strip - 7);
	if (s == nullptr) {
		return runs;
	}
	int start = -1, last = -1;
	for (int x = int(from.x); x <= int(to.x) + 1; x++) {
		bool inked = false;
		for (int y = int(from.y); y <= int(to.y) && x <= int(to.x); y++) {
			uint32_t px = pixel_at(s, x, y);
			inked = inked || ((px >> 16) & 0xff) + ((px >> 8) & 0xff) + (px & 0xff) < 250;
		}
		if (inked) {
			start = start < 0 ? x : start;
			last = x;
		} else if (start >= 0 && x - last > 3) {
			runs.push_back({double(start), double(last + 1)});
			start = -1;
		}
	}
	return runs;
}

// A strip control, as `note_strip_runs` found it: clicked at its centre.
void click_strip_run(std::string_view id, const std::vector<Run> &runs,
		std::size_t index) {
	if (index < runs.size()) {
		click(runs[index].centre(), note_local(id, 0, note_strip / 2.0).y);
	}
}

// Dark pixels — the ink of the note's text — inside a rectangle of a shot.
int ink_count(cairo_surface_t *s, const wlr_box &rect) {
	int n = 0;
	for (int y = rect.y; s != nullptr && y < rect.y + rect.height; y++) {
		for (int x = rect.x; x < rect.x + rect.width; x++) {
			uint32_t px = pixel_at(s, x, y);
			n += ((px >> 16) & 0xff) + ((px >> 8) & 0xff) + (px & 0xff) < 300 ? 1 : 0;
		}
	}
	return n;
}

// How many rows of a rectangle of a shot hold ink, from the first to the
// last: the height the text there is drawn at.
int ink_extent(cairo_surface_t *s, const wlr_box &rect) {
	int top = -1, bottom = -1;
	for (int y = rect.y; s != nullptr && y < rect.y + rect.height; y++) {
		if (ink_count(s, wlr_box{rect.x, y, rect.width, 1}) > 0) {
			top = top < 0 ? y : top;
			bottom = y;
		}
	}
	return top < 0 ? 0 : bottom - top + 1;
}

// The strip just left of a control's glyph: inside its button, whose
// background is darker while the button is on.
uint32_t button_background(cairo_surface_t *s, std::string_view id, const Run &run) {
	if (s == nullptr) {
		return 0;
	}
	return pixel_at(s, int(run.x0) - 2, int(note_local(id, 0, note_strip / 2.0).y));
}

int channel_sum(uint32_t px) {
	return ((px >> 16) & 0xff) + ((px >> 8) & 0xff) + (px & 0xff);
}

// The compositor runs a drag the plugin handed it.
bool surface_drag_running() {
	return std::holds_alternative<PointerGrab::SurfaceDrag>(server->input.grab.state());
}

// Super and a 2-finger pinch, through the pointer device as a touchpad
// sends it: the desktop on screen scales to `scale` and settles there.
void super_pinch(double scale) {
	key(KEY_LEFTMETA, true);
	harness::touchpad_pinch_begin(2);
	for (int i = 1; i <= 10; i++) {
		harness::touchpad_pinch_update(2, 1 + (scale - 1) * i / 10.0);
	}
	harness::touchpad_pinch_end(false);
	key(KEY_LEFTMETA, false);
}

bool camera_settled() {
	Output *o = out();
	return o != nullptr && !o->camera.panning() && !o->camera.zooming();
}

// A save of the scratch shell.json as it stands: the shell reloads on it.
void touch_shell_config() {
	write_file(shell_config, read_file(shell_config));
}

} // namespace

std::vector<harness::Step> notes_steps() {
	return {
		{"notes_note_created_the_way_any", [] {
			// The note goes centred on the cursor: the pointer is parked at a
			// known point first, on a settled camera, so the check that follows
			// reads the centring alone.
			if (auto wait = wait_for(camera_settled(), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			move_to(note_cursor_x, note_cursor_y);
			run_msg("action.run",
				R"({"name":"canvas.notes/create","args":{"color":")" + std::string{note_yellow}
					+ R"("}})",
				"note-create");
			return after(600);
		}},
		{"notes_note_is_a_surface_of", [] {
			LayerSurface *surface = note_surface();
			if (auto wait = wait_for(!((surface == nullptr || stored_notes().empty())), {.poll_ms = 200, .budget_ms = 20000})) {
				return *wait;
			}
			check(surface != nullptr, "a note is one surface of the plugin's own");
			if (surface == nullptr) {
				return go_to("notes_runs_end", 100);
			}
			check(wl_resource_get_client(surface->wlr->surface->resource) == bar_client(),
				"drawn by the shell, not by the compositor");
			check(surface->canvas_box.has_value(),
				"placed on the canvas, not on a screen");
			std::vector<NoteEntry> stored = stored_notes();
			check(stored.size() == 1, std::format(
				"the plugin kept exactly one note, one HTML file ({})", stored.size()));
			if (!stored.empty()) {
				note = stored.front();
				wlr_box box = note_box(note.id);
				check(note.color == note_yellow, "in the colour the action asked for");
				check(note.x == box.x && note.y == box.y && note.width == box.width
					&& note.height == box.height,
					"its file holds the canvas rectangle the compositor placed it at");
				// The cursor's canvas point at the parked pointer, and the
				// note's centre: the same point, within the half-pixel the
				// plugin's rounding of the note's corner can move it.
				Point aimed = out()->local_to_canvas(note_cursor_x, note_cursor_y);
				check(std::abs(box.x + box.width / 2.0 - aimed.x) <= 0.5
						&& std::abs(box.y + box.height / 2.0 - aimed.y) <= 0.5,
					std::format("the note is centred on the cursor's canvas point "
						"(centre {},{}, cursor at {},{})",
						box.x + box.width / 2.0, box.y + box.height / 2.0,
						aimed.x, aimed.y));
			}
			check(server->seat->keyboard_state.focused_surface == surface->wlr->surface,
				"the new note holds the keyboard");
			return after(400);
		}},
		{"notes_typing_goes_into_the_note", [] {
			click_note_body(note.id);
			type_text("hello");
			return after(800);
		}},
		{"notes_typed_text_stored", [] {
			std::optional<NoteEntry> stored = stored_note(note.id);
			if (auto wait = wait_for(!((!stored.has_value() || stored->text.find("hello") == std::string::npos)), {.poll_ms = 300, .budget_ms = 18000})) {
				return *wait;
			}
			check(stored.has_value() && stored->text.find("hello") != std::string::npos,
				std::format("what is typed goes into the note ('{}')",
					stored.has_value() ? stored->text : std::string{}));
			return after(300);
		}},
		{"notes_pan_moves_the_note_with", [] {
			note_before = note_box(note.id);
			chord(KEY_RIGHT, mods::super_alt);
			return after(1200);
		}},
		{"notes_pan_leaves_exactly_one_card", [] {
			if (auto wait = wait_for(camera_settled(), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(note_surfaces() == 1, std::format(
				"a pan leaves exactly one card on the plane ({})", note_surfaces()));
			wlr_box later = note_box(note.id);
			check(later.x == note_before.x && later.y == note_before.y,
				"the note stays where it was put on the canvas while the view moves");
			std::optional<NoteEntry> stored = stored_note(note.id);
			check(stored.has_value() && stored->text.find("hello") != std::string::npos,
				"with its text, which no camera move touches");
			chord(KEY_LEFT, mods::super_alt); // the view back where it was
			return after(1200);
		}},
		{"notes_colour_dot_on_the_notes", [] {
			if (auto wait = wait_for(camera_settled(), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			click_note_dot(note.id, 1); // the second of the manifest's colours: green
			return after(800);
		}},
		{"notes_dot_on_the_strip_recolours", [] {
			std::optional<NoteEntry> stored = stored_note(note.id);
			if (auto wait = wait_for(!((!stored.has_value() || stored->color != note_green)), {.poll_ms = 300, .budget_ms = 18000})) {
				return *wait;
			}
			check(stored.has_value() && stored->color == note_green, std::format(
				"a dot on the strip recolours the note's file ('{}')",
				stored.has_value() ? stored->color : std::string{}));
			return after(0);
		}},
		{"notes_and_the_card_itself_on", [] {
			Point paper = note_local(note.id, -24, note_strip + 30);
			if (auto wait = shot_holds("note-green", pixel_shows(int(paper.x), int(paper.y), 0xa6e3a1ff))) {
				return *wait;
			}
			cairo_surface_t *s = load_png("note-green");
			check(pixel_is(s, int(paper.x), int(paper.y), 0xa6e3a1ff), std::format(
				"the card turned green on screen ({:06x})",
				s != nullptr ? pixel_at(s, int(paper.x), int(paper.y)) : 0));
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			touch_shell_config(); // the shell reloads: the note comes back, once
			return after(1500);
		}},
		{"notes_note_comes_back_as_one", [] {
			if (auto wait = wait_for(!((note_surfaces() != 1 || !stored_note(note.id).has_value())), {.poll_ms = 300, .budget_ms = 45000})) {
				return *wait;
			}
			check(note_surfaces() == 1, std::format(
				"the note comes back as one card after a reload ({})", note_surfaces()));
			std::optional<NoteEntry> stored = stored_note(note.id);
			check(stored.has_value() && stored->text.find("hello") != std::string::npos
					&& stored->color == note_green,
				"with the text it was written in and its colour");
			return after(400);
		}},
		{"notes_card_grows_to_hold_what", [] {
			note_before = note_box(note.id);
			click_note_body(note.id);
			for (int line = 0; line < 8; line++) {
				type_text("a note long enough to need another line of its own ");
			}
			return after(1500);
		}},
		{"notes_note_grows_as_it_is", [] {
			wlr_box grown = note_box(note.id);
			std::optional<NoteEntry> stored = stored_note(note.id);
			// typed over several ticks: the card stops growing with the eighth
			// line, and its file then holds the height it ended on
			std::size_t typed = 0;
			for (std::size_t at = 0; stored.has_value()
					&& (at = stored->text.find("another line of its own", at)) != std::string::npos; at++) {
				typed++;
			}
			bool grown_and_kept = grown.height > note_before.height && stored.has_value()
				&& stored->height == grown.height && typed == 8;
			if (auto wait = wait_for(grown_and_kept, {.poll_ms = 300, .budget_ms = 18000})) {
				return *wait;
			}
			check(grown.height > note_before.height, std::format(
				"a note grows as it is written in ({} px, was {})", grown.height,
				note_before.height));
			check(stored.has_value() && stored->height == grown.height,
				"and the new height is kept in its file");
			return after(0);
		}},
		{"notes_taller_card_on_screen", [] {
			Point below = note_local(note.id, -24, note_before.height + 10);
			if (auto wait = shot_holds("note-grown", pixel_shows(int(below.x), int(below.y), 0xa6e3a1ff))) {
				return *wait;
			}
			cairo_surface_t *s = load_png("note-grown");
			check(pixel_is(s, int(below.x), int(below.y), 0xa6e3a1ff),
				"the grown card is drawn to its new height");
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			// The strip held down: its client hands the drag to the compositor.
			note_before = note_box(note.id);
			Point strip = note_local(note.id, -30, note_strip / 2.0);
			move_to(strip.x, strip.y);
			button(BTN_LEFT, true);
			return after(300);
		}},
		{"notes_press_on_the_strip_hands", [] {
			if (auto wait = wait_for(surface_drag_running(), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(surface_drag_running(),
				"a press on the strip hands the drag to the compositor");
			Point strip = note_local(note.id, -30, note_strip / 2.0);
			travel(strip.x, strip.y, strip.x + 120, strip.y + 60);
			return after(100);
		}},
		{"notes_note_under_the_pointer_while", [] {
			wlr_box during = note_box(note.id);
			check(during.x == note_before.x + 120 && during.y == note_before.y + 60,
				std::format("the note follows the pointer while its strip is dragged "
					"({},{}, from {},{})", during.x, during.y, note_before.x, note_before.y));
			button(BTN_LEFT, false);
			return after(600);
		}},
		{"notes_dropped", [] {
			std::optional<NoteEntry> stored = stored_note(note.id);
			wlr_box box = note_box(note.id);
			if (auto wait = wait_for(!((!stored.has_value() || stored->x != box.x || stored->y != box.y)), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(!surface_drag_running() && box.x == note_before.x + 120
					&& box.y == note_before.y + 60,
				"the note stays where the strip was let go");
			check(stored.has_value() && stored->x == box.x && stored->y == box.y,
				"and its file keeps where it was dropped");
			return after(0);
		}},
		{"notes_card_is_drawn_where_it", [] {
			Point now = note_local(note.id, -24, note_strip + 30);
			Point was = out()->canvas_to_local(note_before.x + 10, note_before.y + 40);
			if (auto wait = shot_holds("note-moved", [&](cairo_surface_t *s) {
					return pixel_is(s, int(now.x), int(now.y), 0xa6e3a1ff)
						&& !pixel_is(s, int(was.x), int(was.y), 0xa6e3a1ff);
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("note-moved");
			check(pixel_is(s, int(now.x), int(now.y), 0xa6e3a1ff)
					&& !pixel_is(s, int(was.x), int(was.y), 0xa6e3a1ff),
				"the card is drawn where it was dropped, and not where it was");
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			// The grip held down, at 100 %.
			note_before = note_box(note.id);
			Point grip = note_local(note.id, -6, -6);
			move_to(grip.x, grip.y);
			button(BTN_LEFT, true);
			return after(300);
		}},
		{"notes_press_on_the_grip_hands", [] {
			if (auto wait = wait_for(surface_drag_running(), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(surface_drag_running(),
				"a press on the grip hands the corner to the compositor");
			Point grip = note_local(note.id, -6, -6);
			travel(grip.x, grip.y, grip.x + 90, grip.y + 50);
			return after(100);
		}},
		{"notes_corner_follows_the_pointer_at", [] {
			wlr_box during = note_box(note.id);
			check(during.x == note_before.x && during.y == note_before.y
					&& during.width == note_before.width + 90
					&& during.height == note_before.height + 50,
				std::format("the corner follows the pointer at 100 % ({}x{}, from {}x{})",
					during.width, during.height, note_before.width, note_before.height));
			button(BTN_LEFT, false);
			return after(700);
		}},
		{"notes_size_the_corner_gave_is", [] {
			std::optional<NoteEntry> stored = stored_note(note.id);
			wlr_box box = note_box(note.id);
			if (auto wait = wait_for(!((!stored.has_value() || stored->width != box.width)), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(stored.has_value() && stored->width == note_before.width + 90
					&& stored->height == box.height && box.height >= note_before.height + 50,
				"the size the corner gave is kept in its file");
			return after(0);
		}},
		{"notes_drawn_to_the_new_corner", [] {
			Point corner = note_local(note.id, -30, -30);
			if (auto wait = shot_holds("note-sized", pixel_shows(int(corner.x), int(corner.y), 0xa6e3a1ff))) {
				return *wait;
			}
			cairo_surface_t *s = load_png("note-sized");
			check(pixel_is(s, int(corner.x), int(corner.y), 0xa6e3a1ff),
				"the sized card is drawn out to its new corner");
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			super_pinch(0.75); // the desktop on screen to 75 %, the whole note in view
			return after(1200);
		}},
		{"notes_corner_at_a_scaled_desktop", [] {
			if (auto wait = wait_for(camera_settled(), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(std::fabs(out()->camera.drawn().zoom - 0.75) < 0.01, std::format(
				"Super + a pinch scaled the desktop on screen ({:.2f})",
				out()->camera.drawn().zoom));
			note_before = note_box(note.id);
			Point grip = note_local(note.id, -6, -6);
			move_to(grip.x, grip.y);
			button(BTN_LEFT, true);
			return after(300);
		}},
		{"notes_at_75_the_grip_hands", [] {
			if (auto wait = wait_for(surface_drag_running(), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(surface_drag_running(), "at 75 % the grip hands the corner over too");
			Point grip = note_local(note.id, -6, -6);
			travel(grip.x, grip.y, grip.x + 90, grip.y + 60);
			return after(100);
		}},
		{"notes_at_75_the_corner_follows", [] {
			wlr_box during = note_box(note.id);
			check(during.width == note_before.width + 120
					&& during.height == note_before.height + 80,
				std::format("at 75 % the corner follows the pointer in canvas pixels "
					"({}x{}, from {}x{})", during.width, during.height,
					note_before.width, note_before.height));
			button(BTN_LEFT, false);
			chord(KEY_0, mods::super); // the desktop back to 100 %
			return after(1200);
		}},
		{"notes_strip_dragged_in_the_overview", [] {
			if (auto wait = wait_for(camera_settled(), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			chord(KEY_TAB, mods::super);
			return after(1200);
		}},
		{"notes_strip_drag_in_overview", [] {
			if (auto wait = wait_for(overview_settled(true), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			note_before = note_box(note.id);
			Point strip = note_local(note.id, -30, note_strip / 2.0);
			move_to(strip.x, strip.y);
			button(BTN_LEFT, true);
			return after(300);
		}},
		{"notes_in_the_overview_the_strip", [] {
			if (auto wait = wait_for(surface_drag_running(), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(surface_drag_running(), "in the overview the strip hands the drag over too");
			Point strip = note_local(note.id, -30, note_strip / 2.0);
			travel(strip.x, strip.y, strip.x + 40, strip.y + 20);
			return after(100);
		}},
		{"notes_in_the_overview_the_note", [] {
			double zoom = out()->camera.drawn().zoom;
			wlr_box during = note_box(note.id);
			check(std::abs(during.x - (note_before.x + std::lround(40 / zoom))) <= 1
					&& std::abs(during.y - (note_before.y + std::lround(20 / zoom))) <= 1,
				std::format("in the overview the note follows the pointer at its zoom "
					"({:.2f}: {},{} from {},{})", zoom, during.x, during.y,
					note_before.x, note_before.y));
			button(BTN_LEFT, false);
			return after(600);
		}},
		{"notes_second_note_for_the_formatting", [] {
			if (out()->overview.active()) {
				chord(KEY_TAB, mods::super);
			}
			return after(400);
		}},
		{"notes_note_goes_centred_on_the", [] {
			// compositor answers through the camera as drawn: the camera has
			// stopped first, or the note lands where the overview showed the cursor
			if (auto wait = wait_for(overview_settled(false), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			run_msg("action.run",
				R"({"name":"canvas.notes/create","args":{"color":")" + std::string{note_yellow}
					+ R"("}})",
				"note-create-2");
			return after(1200);
		}},
		{"notes_second_note_comes_up_with", [] {
			LayerSurface *made = nullptr;
			for (const NoteEntry &entry : stored_notes()) {
				if (entry.id != note.id) {
					second_note = entry.id;
					made = note_surface(entry.id);
				}
			}
			if (auto wait = wait_for(!((made == nullptr || !overview_settled(false))), {.poll_ms = 200, .budget_ms = 20000})) {
				return *wait;
			}
			check(made != nullptr && server->seat->keyboard_state.focused_surface
					== made->wlr->surface,
				"a second note comes up with the keyboard");
			type_text("plain bold");
			return after(800);
		}},
		{"notes_line_as_typed", [] {
			std::optional<NoteEntry> stored = stored_note(second_note);
			if (auto wait = wait_for(!((!stored.has_value() || stored->text.find("plain bold") == std::string::npos)), {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			return after(0);
		}},
		{"notes_last_word_selected", [] {
			Point text = note_local(second_note, 8, note_strip + 4);
			if (auto wait = shot_holds("note-plain", [&](cairo_surface_t *s) {
					return ink_count(s, wlr_box{int(text.x), int(text.y), 30, 14}) > 0
						&& note_strip_runs(s, second_note).size() == 8;
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("note-plain");
			note_ink_plain = ink_count(s, wlr_box{int(text.x), int(text.y), 30, 14});
			note_ink_word = ink_count(s, wlr_box{int(text.x) + 45, int(text.y), 27, 14});
			note_plain_extent = ink_extent(s, wlr_box{int(text.x), int(text.y), 100, 40});
			std::vector<Run> runs = note_strip_runs(s, second_note);
			note_b_idle = runs.empty() ? 0 : button_background(s, second_note, runs[0]);
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			check(runs.size() == 8, std::format("the strip shows bold, italic, underline, "
				"the font, the size between − and +, and the × ({} controls)", runs.size()));
			chord(KEY_LEFT, mods::ctrl_shift);
			click_strip_run(second_note, runs, 0);
			return after(600);
		}},
		{"notes_bold_word_stored", [] {
			std::optional<NoteEntry> stored = stored_note(second_note);
			if (auto wait = wait_for(!((!stored.has_value() || stored->text.find("<b>bold</b>") == std::string::npos)), {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			// Nothing selected afterwards, so the line is drawn as it is: the
			// key goes in a step of its own, a client may take keys before a
			// click sent with them.
			tap(KEY_END);
			check(stored.has_value() && stored->text.find("<b>bold</b>") != std::string::npos
					&& stored->text.find("plain") != std::string::npos
					&& stored->text.find("<b>plain") == std::string::npos,
				std::format("the selected word is bold in the file, the first left plain ({})",
					stored.has_value() ? stored->text : std::string{}));
			return after(500);
		}},
		{"notes_and_on_screen_read", [] {
			Point text = note_local(second_note, 8, note_strip + 4);
			auto bold_drawn = [&](cairo_surface_t *s) {
				int plain = ink_count(s, wlr_box{int(text.x), int(text.y), 30, 14});
				int word = ink_count(s, wlr_box{int(text.x) + 45, int(text.y), 27, 14});
				return word > note_ink_word * 6 / 5
					&& std::abs(plain - note_ink_plain) * 10 <= note_ink_plain;
			};
			if (auto wait = shot_holds("note-bold", bold_drawn)) {
				return *wait;
			}
			cairo_surface_t *s = load_png("note-bold");
			int plain = ink_count(s, wlr_box{int(text.x), int(text.y), 30, 14});
			int word = ink_count(s, wlr_box{int(text.x) + 45, int(text.y), 27, 14});
			check(word > note_ink_word * 6 / 5 && std::abs(plain - note_ink_plain) * 10
					<= note_ink_plain,
				std::format("the bold word is drawn heavier ({} ink, was {}), the plain one "
					"the same ({}, was {})", word, note_ink_word, plain, note_ink_plain));
			note_runs = note_strip_runs(s, second_note);
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			// The last word again, two pixels larger by the strip's +.
			chord(KEY_LEFT, mods::ctrl_shift);
			click_strip_run(second_note, note_runs, 6);
			click_strip_run(second_note, note_runs, 6);
			return after(800);
		}},
		{"notes_size_is_the_spans", [] {
			std::optional<NoteEntry> stored = stored_note(second_note);
			std::string sized = std::format("<span style=\"font-size: {}px\"><b>bold</b></span>",
				stored.has_value() ? stored->size + 2 : 0);
			if (auto wait = wait_for(!((!stored.has_value() || stored->text.find(sized) == std::string::npos)), {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(stored.has_value() && stored->text.find(sized) != std::string::npos
					&& stored->text.starts_with("<p>plain "),
				std::format("+ sizes the selected word alone, the rest at the note's size ({})",
					stored.has_value() ? stored->text : std::string{}));
			return after(0);
		}},
		{"notes_strip_shows_the_selections_own", [] {
			if (auto wait = shot_holds("note-word-selected", [&](cairo_surface_t *s) {
					return !note_runs.empty() && channel_sum(button_background(s, second_note,
						note_runs[0])) + 40 < channel_sum(note_b_idle);
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("note-word-selected");
			uint32_t on = note_runs.empty() ? 0 : button_background(s, second_note, note_runs[0]);
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			check(channel_sum(on) + 40 < channel_sum(note_b_idle), std::format(
				"a bold word selected backwards shows the strip's B on (#{:06x}, off #{:06x})",
				on & 0xffffff, note_b_idle & 0xffffff));
			// A selection dragged from the first word out past the note's edge.
			Point from = note_local(second_note, 9, note_strip + 4 + 6);
			move_to(from.x, from.y);
			button(BTN_LEFT, true);
			return after(300);
		}},
		{"notes_button_held_in_the_note", [] {
			Point from = note_local(second_note, 9, note_strip + 4 + 6);
			Point below = note_local(second_note, 60, note_box(second_note).height + 40);
			travel(from.x, from.y, below.x, below.y);
			LayerSurface *surface = note_surface(second_note);
			check(surface != nullptr
					&& server->seat->pointer_state.focused_surface == surface->wlr->surface,
				"a button held in the note keeps the pointer on it past its edge");
			button(BTN_LEFT, false);
			return after(400);
		}},
		{"notes_18_typed_into_the_size", [] {
			click_strip_run(second_note, note_runs, 5);
			type_text("18");
			tap(KEY_ENTER);
			return after(800);
		}},
		{"notes_size_typed_into_the_strip", [] {
			std::optional<NoteEntry> stored = stored_note(second_note);
			bool both = stored.has_value()
				&& stored->text.find("<span style=\"font-size: 18px\">plain") != std::string::npos
				&& stored->text.find("<span style=\"font-size: 18px\"><b>bold</b></span>")
					!= std::string::npos;
			if (auto wait = wait_for(both, {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(both, std::format("a size typed into the strip sizes the dragged selection, "
				"each part keeping its weight ({})",
				stored.has_value() ? stored->text : std::string{}));
			return after(0);
		}},
		{"notes_and_the_line_stands_taller", [] {
			Point text = note_local(second_note, 8, note_strip + 4);
			if (auto wait = shot_holds("note-sized-span", [&](cairo_surface_t *s) {
					return ink_extent(s, wlr_box{int(text.x), int(text.y), 100, 40}) * 10
						>= note_plain_extent * 13;
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("note-sized-span");
			int extent = ink_extent(s, wlr_box{int(text.x), int(text.y), 100, 40});
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			check(extent * 10 >= note_plain_extent * 13, std::format(
				"the sized line is drawn taller ({} px of ink, was {})", extent,
				note_plain_extent));
			return after(100);
		}},
		{"notes_font_menu_shown", [] {
			if (auto wait = shot_holds("note-sized-text", [&](cairo_surface_t *s) {
					return note_strip_runs(s, second_note).size() == 8;
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("note-sized-text");
			note_runs = note_strip_runs(s, second_note);
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			click_strip_run(second_note, note_runs, 3);
			return after(600);
		}},
		{"notes_type_font_name", [] {
			type_text("mono");
			tap(KEY_ENTER);
			return after(800);
		}},
		{"notes_font_stored", [] {
			std::optional<NoteEntry> stored = stored_note(second_note);
			if (auto wait = wait_for(!((!stored.has_value() || stored->font == "monospace")), {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(stored.has_value() && stored->font != "monospace"
					&& stored->font.find("ono") != std::string::npos,
				std::format("the font menu sets an installed family for the whole note ('{}')",
					stored.has_value() ? stored->font : std::string{}));
			check(stored.has_value() && stored->text.find("<b>bold</b>") != std::string::npos,
				"and the bold span keeps its weight in the new font");
			// The first word: italic by Ctrl+I, underlined by the strip's U.
			tap(KEY_HOME);
			chord(KEY_RIGHT, mods::ctrl_shift);
			chord(KEY_I, mods::ctrl);
			return after(600);
		}},
		{"notes_click_italic", [] {
			click_strip_run(second_note, note_runs, 2);
			return after(600);
		}},
		{"notes_italic_stored", [] {
			std::optional<NoteEntry> stored = stored_note(second_note);
			if (auto wait = wait_for(!((!stored.has_value() || stored->text.find("<u>") == std::string::npos)), {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(stored.has_value() && stored->text.find("<i><u>plain") != std::string::npos
					&& stored->text.find("<b>bold</b>") != std::string::npos,
				std::format("Ctrl+I and the strip's U set their span, the bold one kept ({})",
					stored.has_value() ? stored->text : std::string{}));
			tap(KEY_ESC); // the keyboard back to the window
			return after(600);
		}},
		{"notes_escape_gives_the_keyboard_back", [] {
			LayerSurface *second = note_surface(second_note);
			wlr_surface *focus = server->seat->keyboard_state.focused_surface;
			if (auto wait = wait_for(!(second != nullptr && focus == second->wlr->surface), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(second != nullptr && focus != second->wlr->surface,
				"Escape gives the keyboard back");
			click_note_body(second_note);
			return after(600);
		}},
		{"notes_third_note_left_empty_when", [] {
			LayerSurface *second = note_surface(second_note);
			check(second != nullptr
					&& server->seat->keyboard_state.focused_surface == second->wlr->surface,
				"and a click in the note takes it again");
			// A third note, left empty when the keyboard goes: it goes too.
			run_msg("action.run",
				R"({"name":"canvas.notes/create","args":{"color":")" + std::string{note_green}
					+ R"("}})",
				"note-create-3");
			return after(1200);
		}},
		{"notes_third_note_came_up", [] {
			if (auto wait = wait_for(note_surfaces() == 3, {.poll_ms = 200, .budget_ms = 20000})) {
				return *wait;
			}
			check(note_surfaces() == 3, "a third note came up");
			tap(KEY_ESC);
			return after(800);
		}},
		{"notes_note_left_empty_when_the", [] {
			if (auto wait = wait_for(!((note_surfaces() != 2 || stored_notes().size() != 2)), {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(note_surfaces() == 2 && stored_notes().size() == 2,
				"a note left empty when the keyboard goes is deleted, file and all");
			// A second output, its view on the first note.
			wlr_backend *headless = nullptr;
			wlr_multi_for_each_backend(server->backend,
				[](wlr_backend *backend, void *data) {
					if (wlr_backend_is_headless(backend)) {
						*static_cast<wlr_backend **>(data) = backend;
					}
				}, &headless);
			check(headless != nullptr && wlr_headless_add_output(headless, 640, 480) != nullptr,
				"a second output was added");
			return after(800);
		}},
		{"notes_second_output_added", [] {
			if (auto wait = wait_for(server->outputs.size() >= 2, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			Output *second = server->outputs.back().get();
			wlr_box box = note_box(note.id);
			Camera cam = second->camera.current();
			cam.cx = box.x + box.width / 2.0;
			cam.cy = box.y + box.height / 2.0;
			cam.zoom = 1;
			second->camera.jump(cam);
			return after(800);
		}},
		{"notes_note_made_on_the_first", [] {
			Output *second = server->outputs.back().get();
			std::optional<LayerHit> hit = second->canvas_surface_at(second->width / 2.0,
				second->height / 2.0);
			check(hit.has_value() && hit->layer == note_surface(note.id),
				"the second output hit-tests the note where its view shows it");
			return after(0);
		}},
		{"notes_second_output_draws_the_note", [] {
			Output *second = server->outputs.back().get();
			int x = int(second->lx + second->width / 2 + 40);
			int y = int(second->ly + second->height / 2 + 40);
			if (auto wait = shot_holds("note-two-outputs", pixel_shows(x, y, 0xa6e3a1ff))) {
				return *wait;
			}
			cairo_surface_t *s = load_png("note-two-outputs");
			check(pixel_is(s, x, y, 0xa6e3a1ff), std::format(
				"the second output draws the note its view shows ({:06x})",
				s != nullptr ? pixel_at(s, x, y) : 0));
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			wlr_output_destroy(second->wlr);
			return after(800);
		}},
		{"notes_s_take_both_notes_away", [] {
			if (auto wait = wait_for(server->outputs.size() == 1, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(note_surfaces() == 2, "the notes stay when an output goes");
			return after(0);
		}},
		{"notes_second_close_clicked", [] {
			if (auto wait = shot_holds("note-close", [&](cairo_surface_t *s) {
					return note_strip_runs(s, note.id).size() == 8;
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("note-close");
			std::vector<Run> first = note_strip_runs(s, note.id);
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			click_strip_run(note.id, first, first.size() - 1); // the ×, rightmost
			return after(800);
		}},
		{"notes_takes_its_note_off_the", [] {
			if (auto wait = wait_for(!((note_surfaces() != 1 || stored_note(note.id).has_value())), {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(note_surfaces() == 1 && !stored_note(note.id).has_value(),
				"the × takes its note off the plane and its file away");
			return after(0);
		}},
		{"notes_second_close_checked", [] {
			if (auto wait = shot_holds("note-close-2", [&](cairo_surface_t *s) {
					return note_strip_runs(s, second_note).size() == 8;
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("note-close-2");
			std::vector<Run> runs = note_strip_runs(s, second_note);
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			click_strip_run(second_note, runs, runs.size() - 1); // the ×, rightmost
			return after(800);
		}},
		{"notes_and_the_others", [] {
			if (auto wait = wait_for(!((note_surfaces() != 0 || !stored_notes().empty())), {.poll_ms = 200, .budget_ms = 15000})) {
				return *wait;
			}
			check(note_surfaces() == 0 && stored_notes().empty(),
				"and the other's: no note and no file left");
			return after(100);
		}},
		{"notes_runs_end", [] {
			if (auto wait = wait_for(overview_settled(false), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(overview_settled(false), "the overview closed behind the notes");
			check(toolbar_layer() == nullptr, "the toolbar slid out with the overview");
			// panel's (180+)
			return after(100);
		}},
	};
}

} // namespace bar
