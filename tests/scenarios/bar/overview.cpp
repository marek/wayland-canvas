// The overview over the bar: Super+Tab, clicks on the bar and a panel inside it, Escape.
#include "scenarios/bar/world.h"

namespace bar {

std::vector<harness::Step> overview_steps() {
	return {
		wifi_offline_step("overview_wifi_offline"),
		{"overview_super_tab_the_overviews_chord", [] {
			key(KEY_LEFTMETA, true);
			key(KEY_TAB, true);
			key(KEY_TAB, false);
			key(KEY_LEFTMETA, false);
			return after(400);
		}},
		{"overview_super_tab_opened_the_overview", [] {
			Output &o = *server->outputs.front();
			check(o.overview.active(), "Super+Tab opened the overview over the bar");
			shot("overview");
			return after(400);
		}},
		{"overview_click_on_the_bars_own", [] {
			// sends launcher.toggle over the control socket
			click(300.0, bar_mid_y());
			return after(400);
		}},
		{"overview_click_on_the_bar_did", [] {
			check(server->outputs.front()->overview.active(),
				"a click on the bar did not leave the overview");
			return after(300);
		}},
		{"overview_network_indicator_opens_its_panel", [] {
			click(network_x, bar_mid_y());
			return after(400);
		}},
		{"overview_then_a_click_outside_the", [] {
			LayerSurface *panel = panel_layer("network");
			// in flight under load
			if (auto wait = wait_for(panel != nullptr, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel != nullptr, "the bar opens a panel inside the overview");
			if (panel == nullptr) {
				return end_run("the overview's network panel did not open");
			}
			shot("overview-network-first");
			click_outside_cards(); // the panel's overlay takes it, not the canvas
			return after(400);
		}},
		{"overview_and_the_same_indicator_opens", [] {
			// itself is forgotten as the open one, so the next click reopens it
			if (auto wait = wait_for(panel_layers() <= 0, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layers() == 0 && server->outputs.front()->overview.active(),
				"a click outside closed the panel and the overview stayed");
			click(network_x, bar_mid_y());
			return after(400);
		}},
		{"overview_network_indicator_reopened_its_panel", [] {
			LayerSurface *panel = panel_layer("network");
			// in flight under load
			if (auto wait = wait_for(panel != nullptr, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel != nullptr, "the network indicator reopened its panel after "
				"the panel closed itself");
			if (panel == nullptr) {
				return end_run("the network panel did not reopen");
			}
			check(wl_resource_get_client(panel->wlr->surface->resource) == bar_client(),
				"the overview's network panel belongs to the bar client");
			if (auto wait = wait_for(server->seat->keyboard_state.focused_surface == panel->wlr->surface, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(server->seat->keyboard_state.focused_surface == panel->wlr->surface,
				"the panel holds the keyboard while the overview is active");
			popup_box = card_box(network_x, network_panel);
			// The overview's toolbar sits over this card's right edge, the
			// card drawn above it: keep the scan column short of the strip,
			// or the toolbar's glyphs pose as the card's rows.
			popup_box.width = std::min(popup_box.width, strip_left - popup_box.x);
			shot("overview-network");
			return after(700);
		}},
		{"overview_click_cafeguest", [] {
			// "Other networks" — the row that asks for a password
			if (!settle_advance("overview-network")) {
				return again(400); // the rows are still settling under load
			}
			WifiOffRows off = wifi_off_rows();
			std::vector<Band> rows = panel_rows("overview-network");
			// The head, "Known networks" with HomeNet and OldCafe, "Other
			// networks" with CafeGuest, CorpNet and LibraryFree: 8 rows.
			// a taller panel means the walk's disconnect never landed: the
			// fixture is still connected, and no row can be trusted; a
			// shorter one is a list the shell has not finished drawing
			if (rows.size() < 8 || rows.size() > 9) {
				if (auto wait = wait_for(false, {.poll_ms = 400, .budget_ms = 15000})) {
					return *wait;
				}
				return end_run(std::format("the overview's panel is not the "
					"disconnected stand-in's list ({} rows)", rows.size()));
			}
			check(rows.size() >= 8 && rows.size() <= 9, std::format("the panel "
				"lists the stand-in's five networks in the overview ({} rows: "
				"head, two headers, 5 rows)", rows.size()));
			wifi_base = int(rows.size());
			wifi_last_top = rows.back().top;
			click(popup_box.x + popup_box.width / 2.0, popup_box.y + off.cafe);
			return after(500);
		}},
		{"overview_shot_must_come_a_tick", [] {
			shot("overview-selected");
			return after(400);
		}},
		{"overview_as_in_the_walk", [] {
			if (!settle_advance("overview-selected")) {
				return again(400); // the panel is still settling under load
			}
			WifiOffRows off = wifi_off_rows();
			std::vector<Band> rows = panel_rows("overview-selected");
			// as in the walk: the field lands under the row, and the rows
			// below shift by its height
			bool armed = rows.size() > wifi_base
				|| (rows.size() == wifi_base && rows.back().top > wifi_last_top + 25);
			if (!armed) { // the prompt may still be arriving
				if (auto wait = wait_for(false, {.poll_ms = 400, .budget_ms = 15000})) {
					return *wait;
				}
				return end_run("the secured network's password field did not arm");
			}
			check(armed, "clicking the secured network armed its password field "
				"in the overview (the rows below shifted)");
			field_box = wlr_box{popup_box.x,
				static_cast<int>(std::round(popup_box.y + off.cafe + 22)),
				popup_box.width, 30};
			click(popup_box.x + popup_box.width / 2.0, popup_box.y + off.cafe + 37);
			return after(500);
		}},
		{"overview_type_only_once_the_click", [] {
			type_text("correctbattery"); // never Enter: nothing is submitted
			return after(400);
		}},
		{"overview_shot_overview_typed", [] {
			shot("overview-typed");
			return after(400);
		}},
		{"overview_typing_reached_the_wi_fi", [] {
			cairo_surface_t *typed = load_png("overview-typed");
			wlr_box rect{
				field_box.x, field_box.y - 10,
				field_box.width, field_box.height + 20,
			};
			int dots = bright_count(typed, rect);
			cairo_surface_destroy(typed);
			// the panel may render the dots late
			if (auto wait = wait_for(dots >= 80, {.poll_ms = 400, .budget_ms = 15000})) {
				shot("overview-typed");
				return *wait;
			}
			// each of the 14 dots lights ~9 px; a bare cursor lights under 40
			check(dots >= 80, std::format("typing reached the Wi-Fi password "
				"field inside the overview ({} bright pixels in it)", dots));
			return after(300);
		}},
		{"overview_panel_holds_the_keyboard", [] {
			tap(KEY_ESC);
			return after(400);
		}},
		{"overview_escape_closed_the_panel_while", [] {
			if (auto wait = wait_for(panel_layers() <= 0, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layers() == 0,
				"Escape closed the panel while the overview stayed");
			check(server->outputs.front()->overview.active(),
				"the panel took Escape: the overview is still active");
			return after(300);
		}},
		{"overview_with_the_panel_gone_the", [] {
			tap(KEY_ESC);
			return after(400);
		}},
		{"overview_second_escape_left_the_overview", [] {
			Output &o = *server->outputs.front();
			if (auto wait = wait_for(!o.overview.active(), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(!o.overview.active(), "the second Escape left the overview");
			return after(100);
		}},
	};
}

} // namespace bar
