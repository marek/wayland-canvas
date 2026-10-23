// A note made on one output and the output that is left.
#include "scenarios/bar/world.h"

namespace bar {

std::vector<harness::Step> handover_steps() {
	return {
		second_output_step("handover_second_output"),
		{"handover_a_note_on_the_plane", [] {
			run_msg("action.run",
				R"({"name":"canvas.notes/create","args":{"color":")" + std::string{note_yellow}
					+ R"("}})",
				"note-create-handover");
			return after(1200);
		}},
		{"handover_note_made_on_the_first", [] {
			LayerSurface *made = note_surface();
			if (auto wait = wait_for(made != nullptr, {.poll_ms = 200, .budget_ms = 20000})) {
				return *wait;
			}
			check(made != nullptr && made->output == server->outputs.front().get(),
				"a note made on the first output");
			if (made == nullptr) {
				return after(100);
			}
			std::string id = made->name().substr(note_surface_prefix.size());
			note = NoteEntry{.id = id};
			wlr_output_destroy(server->outputs.front()->wlr);
			return after(1000);
		}},
		{"handover_note_stays_on_the_plane", [] {
			if (auto wait = wait_for(server->outputs.size() == 1, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			LayerSurface *kept = note_surface(note.id);
			check(kept != nullptr && kept->output == server->outputs.front().get(),
				"the note stays on the plane when its output goes, taken over by the one left");
			wlr_box box = note_box(note.id);
			Camera cam = out()->camera.current();
			cam.cx = box.x + box.width / 2.0;
			cam.cy = box.y + box.height / 2.0;
			cam.zoom = 1;
			out()->camera.jump(cam);
			return after(800);
		}},
		{"handover_and_the_output_that_is_read", [] {
			Point paper = note_local(note.id, -24, note_strip + 30);
			if (auto wait = shot_holds("note-handover", pixel_shows(int(paper.x), int(paper.y), 0xffd866ffu))) {
				return *wait;
			}
			cairo_surface_t *s = load_png("note-handover");
			check(pixel_is(s, int(paper.x), int(paper.y), 0xffd866ffu), std::format(
				"and the output that is left draws it ({:06x})",
				s != nullptr ? pixel_at(s, int(paper.x), int(paper.y)) : 0));
			if (s != nullptr) {
				cairo_surface_destroy(s);
			}
			return after(100); // done: the summary prints and the run ends
		}},
	};
}

} // namespace bar
