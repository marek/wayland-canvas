// The overview's toolbar: its buttons, its panel card and its menu's flyout.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

// shifts every row below it, so the shift proves the prompt opened
double tb_x = 0;

double tb_y = 0; // the toolbar's own re-issued click point: a

std::vector<Band> tb_buttons; // the toolbar's button bands, from the shot

} // namespace

std::vector<harness::Step> toolbar_steps() {
	return {
		{"toolbar_shows_only_in_the", [] {
			check(toolbar_layer() == nullptr,
				"the toolbar is not mapped while the output is out of the overview");
			chord(KEY_TAB, mods::super); // Super+Tab enters the overview again
			return after(400);
		}},
		{"toolbar_entering_the_overview_mapped_the", [] {
			LayerSurface *toolbar = toolbar_layer();
			if (auto wait = wait_for(toolbar != nullptr, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(toolbar != nullptr, "entering the overview mapped the toolbar "
				"(a layer surface of the shell client)");
			if (toolbar != nullptr) {
				check(wl_resource_get_client(toolbar->wlr->surface->resource) == bar_client(),
					"the toolbar belongs to the shell client");
			}
			shot("toolbar");
			return after(700);
		}},
		{"toolbar_buttons_read_off_the_screenshot", [] {
			// is opaque — its own glyphs are the only lit pixels in it — and
			// the strip is the cluster of bands closest to the output's
			// vertical centre, so stray lit pixels cannot pose as buttons.
			cairo_surface_t *s = load_png("toolbar");
			std::vector<Band> all = bands_in(s, {strip_left, 0, toolbar_button, screen_h});
			cairo_surface_destroy(s);
			std::vector<Band> best;
			double best_distance = screen_h;
			for (std::size_t i = 0; i < all.size();) {
				std::vector<Band> cluster{all[i]};
				for (std::size_t j = i + 1; j < all.size()
						&& all[j].top - cluster.back().bottom <= toolbar_button; j++) {
					cluster.push_back(all[j]);
				}
				double distance = std::abs((cluster.front().top + cluster.back().bottom) / 2.0
					- screen_h / 2.0);
				if (cluster.size() > best.size()
						|| (cluster.size() == best.size() && distance < best_distance)) {
					best = cluster;
					best_distance = distance;
				}
				i += cluster.size();
			}
			// the slide-in may still finish
			if (auto wait = wait_for(best.size() >= 3, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("toolbar");
				return *wait;
			}
			check(best.size() >= 3, std::format("the toolbar holds every registered "
				"button ({} glyph bands in its column: act, panel, menu)", best.size()));
			if (best.size() < 3) {
				return end_run("the toolbar does not hold its three buttons, so no "
					"position can be trusted");
			}
			check(std::abs((best.front().top + best.back().bottom) / 2.0
					- screen_h / 2.0) <= toolbar_button,
				"the strip is vertically centred on the output");
			tb_buttons = best;
			// The action button is the first (order 0): click it.
			tb_x = strip_left + toolbar_button / 2.0;
			tb_y = tb_buttons[0].centre();
			click(tb_x, tb_y);
			return after(400);
		}},
		{"toolbar_proof_the_action_ran", [] {
			std::string marks = read_storage("marks");
			if (auto wait = wait_for(marks.find("\"kind\": \"button\"") != std::string::npos, {.poll_ms = 300, .budget_ms = 30000})) {
				if (polls() % 10 == 3) {
					click(tb_x, tb_y); // a lost click: send it again
				}
				return *wait;
			}
			check(marks.find("\"kind\": \"button\"") != std::string::npos,
				"the action button ran its action with its args (the plugin's "
				"storage says so)");
			check(toolbar_layer() != nullptr, "the toolbar stays after an action");
			// The pointer rests on the action button: its label shows beside
			// it, left of the strip.
			return after(0);
		}},
		{"toolbar_label_beside_the_strip_on", [] {
			wlr_box label_box{strip_left - 160, int(tb_buttons[0].top) - 20, 160 - beside_gap, 40};
			if (auto wait = shot_holds("toolbar-hover", [&](cairo_surface_t *s) {
					return !bands_in(s, label_box).empty();
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("toolbar-hover");
			std::vector<Band> labels = bands_in(s, label_box);
			cairo_surface_destroy(s);
			check(!labels.empty(), "hovering a button shows its label beside the strip");
			// The panel button is the second (order 1).
			click(strip_left + toolbar_button / 2.0, tb_buttons[1].centre());
			return after(400);
		}},
		{"toolbar_panel_button_opened_the_panel", [] {
			LayerSurface *panel = panel_layer("scratch");
			if (auto wait = wait_for(panel != nullptr, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel != nullptr, "the panel button opened the panel through the "
				"common trigger");
			if (panel != nullptr) {
				check(wl_resource_get_client(panel->wlr->surface->resource) == bar_client(),
					"the toolbar's panel belongs to the shell client");
			}
			shot("toolbar-panel");
			return after(700);
		}},
		{"toolbar_panel_card", [] {
			// The card sits left of the strip, centred on the panel button;
			// scan that window, so overview content elsewhere cannot pose as
			// rows.
			cairo_surface_t *s = load_png("toolbar-panel");
			wlr_box card{strip_left - beside_gap - scratch_card,
				int(tb_buttons[1].centre()) - 80, scratch_card, 160};
			std::vector<Band> rows = bands_in(s, card);
			cairo_surface_destroy(s);
			if (auto wait = wait_for(rows.size() >= 2, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("toolbar-panel");
				return *wait;
			}
			check(rows.size() >= 2, "the panel opened beside the toolbar (its two rows "
				"are left of the strip)");
			if (!rows.empty()) {
				double level = (rows.front().top + rows.back().bottom) / 2.0;
				check(std::abs(level - tb_buttons[1].centre()) <= 32,
					"the panel is level with the panel button");
			}
			// A click above the card is outside it: it closes the card and
			// nothing more, the overview and the toolbar staying as they are.
			tb_x = strip_left - beside_gap - scratch_card / 2.0;
			tb_y = tb_buttons[1].centre() - 80 - 30;
			click(tb_x, tb_y);
			return after(400);
		}},
		{"toolbar_click_above_the_card_closed", [] {
			if (auto wait = wait_for(panel_layers() <= 0, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			Output *o = out();
			check(panel_layers() == 0, "a click above the toolbar's card closed it");
			check(toolbar_layer() != nullptr && o != nullptr && o->overview.active(),
				"and only the card: the toolbar stays and the overview stays open");
			// The panel again, for Escape.
			click(strip_left + toolbar_button / 2.0, tb_buttons[1].centre());
			return after(400);
		}},
		{"toolbar_panel_reopened", [] {
			if (auto wait = wait_for(panel_layer("scratch") != nullptr, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layer("scratch") != nullptr, "the panel button opened the panel again");
			tap(KEY_ESC);
			return after(400);
		}},
		{"toolbar_menu_button_is_the_third", [] {
			if (auto wait = wait_for(panel_layers() <= 0, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layers() == 0, "the toolbar's panel closed on Escape");
			// The menu button is the third (order 2).
			click(strip_left + toolbar_button / 2.0, tb_buttons[2].centre());
			return after(400);
		}},
		{"toolbar_flyout_out_of_the_toolbar", [] {
			shot("toolbar-flyout");
			return after(700);
		}},
		{"toolbar_its_rows", [] {
			// The flyout sits left of the strip, centred on the menu button.
			cairo_surface_t *s = load_png("toolbar-flyout");
			wlr_box card{strip_left - beside_gap - scratch_card,
				int(tb_buttons[2].centre()) - 80, scratch_card, 160};
			std::vector<Band> rows = bands_in(s, card);
			cairo_surface_destroy(s);
			if (auto wait = wait_for(rows.size() >= 2, {.poll_ms = 700, .budget_ms = 15000})) {
				shot("toolbar-flyout");
				return *wait;
			}
			check(rows.size() >= 2, "the menu button expanded a flyout beside the "
				"toolbar (a glyph row and a swatch row)");
			if (rows.size() < 2) {
				// Nothing was clicked: the fold shot still happens, the
				// choice proof is skipped.
				return go_to("toolbar_flyout_must_have_folded");
			}
			// Choose the first entry: its action runs and the flyout folds.
			tb_x = strip_left - beside_gap - scratch_card / 2.0;
			tb_y = rows[0].centre();
			click(tb_x, tb_y);
			return after(400);
		}},
		{"toolbar_choices_proof", [] {
			std::string marks = read_storage("marks");
			if (auto wait = wait_for(marks.find("\"kind\": \"menu-one\"") != std::string::npos, {.poll_ms = 300, .budget_ms = 30000})) {
				if (polls() % 10 == 3) {
					click(tb_x, tb_y); // a lost click: send it again
				}
				return *wait;
			}
			check(marks.find("\"kind\": \"menu-one\"") != std::string::npos,
				"choosing a flyout entry ran its action with its args (the "
				"plugin's storage says so)");
			return after(0);
		}},
		{"toolbar_flyout_must_have_folded", [] {
			wlr_box card{strip_left - beside_gap - scratch_card,
				int(tb_buttons[2].centre()) - 80, scratch_card, 160};
			if (auto wait = shot_holds("toolbar-folded", [&](cairo_surface_t *s) {
					return bands_in(s, card).empty();
				})) {
				return *wait;
			}
			cairo_surface_t *s = load_png("toolbar-folded");
			std::vector<Band> rows = bands_in(s, card);
			cairo_surface_destroy(s);
			check(rows.empty(), "choosing an entry folded the flyout");
			check(toolbar_layer() != nullptr, "the toolbar stays after a menu choice");
			tap(KEY_ESC); // Escape leaves the overview, folding everything
			return after(400);
		}},
		{"toolbar_slide_out_unmaps_the_toolbar", [] {
			bool gone = toolbar_layer() == nullptr && panel_layers() == 0;
			if (auto wait = wait_for(gone, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(toolbar_layer() == nullptr, "leaving the overview slid the toolbar "
				"out (unmapped)");
			check(panel_layers() == 0, "no panel and no flyout outlived the toolbar");
			return after(100);
		}},
	};
}

} // namespace bar
