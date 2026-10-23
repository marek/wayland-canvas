// The indicator walk: every indicator opened by a click, its panel checked, typed into where it takes
// typing, then closed.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

constexpr const char *BT_NAME = "org.bluez";
constexpr const char *BT_HEADPHONES = "/org/bluez/hci0/dev_11_22_33_44_55_66";
constexpr const char *BT_SPEAKER = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF";
constexpr const char *PPD_NAME = "org.freedesktop.UPower.PowerProfiles";
constexpr const char *PPD_PATH = "/org/freedesktop/UPower/PowerProfiles";
constexpr const char *PPD_IFACE = "org.freedesktop.UPower.PowerProfiles";

// The keyboard is on a surface of the bar client (so panel keystrokes land).
bool keyboard_on_bar() {
	wlr_surface *s = server->seat->keyboard_state.focused_surface;
	return s != nullptr && bar_client() != nullptr
		&& wl_resource_get_client(s->resource) == bar_client();
}

// The retries of a loop that goes back to an earlier step: a wait cannot count them.
int walk_attempts = 0;

} // namespace

std::vector<harness::Step> walk_steps() {
	return {
		{"walk_open_panel", [] {
			if (current >= indicators.size()) {
				return go_to("walk_done", 100);
			}
			Indicator &ind = indicators[current];
			click(ind.x, bar_mid_y(), ind.button);
			return go_to("walk_panel_opened", 400);
		}},
		{"walk_panel_opened", [] {
			Indicator &ind = indicators[current];
			LayerSurface *panel = panel_layer(ind.name);
			// Service-backed panels take longer than one tick to build their
			// content: give each a generous window to map.
			if (auto wait = wait_for(panel != nullptr, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			walk_attempts = 0;
			if (panel == nullptr && ind.button == BTN_RIGHT) {
				// A tray item without a menu secondary-activates instead: by
				// design, nothing to check — but say so.
				std::println("note: the tray item at x={} has no menu of its own; "
					"its panel is not checked", static_cast<int>(ind.x));
				current++;
				return go_to("walk_open_panel", 300);
			}
			check(panel != nullptr, std::format("the {} panel appeared as a layer surface",
				ind.name));
			if (panel != nullptr) {
				check(wl_resource_get_client(panel->wlr->surface->resource) == bar_client(),
					std::format("the {} panel belongs to the bar client", ind.name));
				// The panel's surface holds the keyboard the moment it maps (the
				// seat enters an on-demand layer surface on its own): no click may
				// be needed before its field is typeable, so check before poking.
				if (auto wait = wait_for(server->seat->keyboard_state.focused_surface == panel->wlr->surface, {.poll_ms = 100, .budget_ms = 15000})) {
					return *wait;
				}
				check(server->seat->keyboard_state.focused_surface == panel->wlr->surface,
					std::format("the {} panel holds the keyboard as it opens", ind.name));
				popup_box = card_box(ind.x, ind.width);
				shot(ind.name);
			}
			if (ind.connect_flow) {
				return go_to("walk_re_shoot_the_panel", 700);
			}
			if (ind.bt_flow) {
				return go_to("walk_bluetooth_panel", 700);
			}
			if (ind.profile_flow) {
				return go_to("walk_power_panel", 700);
			}
			if (ind.wx_flow) {
				return go_to("walk_forecast_rows", 700);
			}
			return go_to("walk_close_panel", 700);
		}},
		{"walk_re_shoot_the_panel", [] {
			shot("network");
			return go_to("walk_click_the_stand_ins_unknown", 700);
		}},
		{"walk_click_the_stand_ins_unknown", [] {
			// in "Other networks" — the row that asks for a password
			WifiRows wifi = wifi_rows(false, false);
			std::vector<Band> rows = panel_rows("network");
			// The head, four figure rows, the band header, the DNS header and
			// pills, "Known networks" with HomeNet and OldCafe, then "Other
			// networks" with CafeGuest, CorpNet and LibraryFree: 15 rows.
			if (rows.size() < 15 && walk_attempts++ < 40) {
				return go_to("walk_re_shoot_the_panel", 400);
			}
			walk_attempts = 0;
			check(rows.size() >= 15, std::format("the network panel lists the "
				"stand-in's five networks ({} rows: head, figures, band, DNS, "
				"two headers, 5 rows)", rows.size()));
			if (rows.size() < 15) {
				return go_to("walk_close_panel", 300);
			}
			wifi_base = int(rows.size());
			wifi_last_top = rows.back().top;
			click(popup_box.x + popup_box.width / 2.0, popup_box.y + wifi.cafe);
			// only after the click was delivered
			return go_to("walk_shot_network_selected", 400);
		}},
		{"walk_shot_network_selected", [] {
			shot("network-selected");
			return go_to("walk_click_asked_for_a_password", 300);
		}},
		{"walk_click_asked_for_a_password", [] {
			// inline under the row — focus it and type into it
			check(keyboard_on_bar(), "the keyboard is on the bar client while "
				"the network panel is open");
			WifiRows wifi = wifi_rows(false, false);
			std::vector<Band> rows = panel_rows("network-selected");
			// The prompt's field lands inline under the row: every row below
			// shifts by its height, and the row count grows when the field's
			// own ink is lit enough to read — either way the shift proves it.
			bool armed = rows.size() > wifi_base
				|| (rows.size() == wifi_base && rows.back().top > wifi_last_top + 25);
			if (!armed && walk_attempts++ < 40) { // the prompt settles
				return go_to("walk_shot_network_selected", 400);
			}
			walk_attempts = 0;
			check(armed, std::format("clicking the secured network armed its "
				"password field ({} rows, was {}; the rows below shifted)",
				rows.size(), wifi_base));
			if (!armed) {
				return go_to("walk_close_panel", 300);
			}
			field_box = wlr_box{popup_box.x,
				static_cast<int>(std::round(popup_box.y + wifi.cafe + 22)),
				popup_box.width, 30};
			click(popup_box.x + popup_box.width / 2.0, popup_box.y + wifi.cafe + 37);
			return go_to("walk_phase_16", 400);
		}},
		{"walk_phase_16", [] {
			type_text("correctbattery"); // never Enter yet: nothing is submitted
			return go_to("walk_shot_network_typed", 400);
		}},
		{"walk_shot_network_typed", [] {
			shot("network-typed");
			return go_to("walk_dots_must_be_on_screen", 400);
		}},
		{"walk_dots_must_be_on_screen", [] {
			cairo_surface_t *typed = load_png("network-typed");
			wlr_box rect{
				field_box.x, field_box.y - 10,
				field_box.width, field_box.height + 20,
			};
			int dots = bright_count(typed, rect);
			cairo_surface_destroy(typed);
			if (dots < 80 && walk_attempts++ < 40) { // the panel may render the dots late
				return go_to("walk_shot_network_typed", 400);
			}
			// each of the 14 dots lights ~9 px; a bare cursor lights under 40
			check(dots >= 80, std::format("typing reached the Wi-Fi password "
				"field ({} bright pixels in it; a bare cursor is under 40)", dots));
			return go_to("walk_submit_the_typed_password", 300);
		}},
		{"walk_submit_the_typed_password", [] {
			// holds it only once the panel has stopped changing
			if (!settle_advance("network-typed")) {
				return go_to("walk_submit_the_typed_password", 400); // the panel is still settling under load
			}
			tap(KEY_ENTER);
			return go_to("walk_enter_submitted", 400);
		}},
		{"walk_enter_submitted", [] {
			// activation with the typed password, and nothing connected
			// before it was typed
			bool sent = mock_called(NM_NAME, NM_MANAGER, "AddAndActivateConnection",
				"correctbattery");
			if (!sent && walk_attempts++ < 150) { // in flight under load, not lost
				return go_to("walk_enter_submitted", 400);
			}
			walk_attempts = 0;
			check(sent, sent ? "Enter in CafeGuest's field sent its activation "
				"with the typed password to the stand-in NetworkManager"
				: "the Enter submit was lost under load");
			shot("network-connected");
			return go_to("walk_connected_networks_row_click_disconnects", 400);
		}},
		{"walk_connected_networks_row_click_disconnects", [] {
			// click lands only once the prompt has closed
			if (!settle_advance("network-connected")) {
				return go_to("walk_connected_networks_row_click_disconnects", 400); // the panel is still settling under load
			}
			WifiRows wifi = wifi_rows(false, false);
			click(popup_box.x + popup_box.width / 2.0, popup_box.y + wifi.home);
			return go_to("walk_shot_network_home", 400);
		}},
		{"walk_shot_network_home", [] {
			shot("network-home");
			return go_to("walk_let_the_panel_settle_on", 400);
		}},
		{"walk_let_the_panel_settle_on", [] {
			if (!settle_advance("network-home")) {
				return go_to("walk_let_the_panel_settle_on", 400); // the panel is still settling under load
			}
			return go_to("walk_phase_22", 300);
		}},
		{"walk_phase_22", [] {
			bool dropped = mock_called(NM_NAME, NM_DEVICE, "Disconnect");
			if (!dropped && walk_attempts++ < 150) { // in flight under load, not lost
				return go_to("walk_phase_22", 400);
			}
			walk_attempts = 0;
			check(dropped, dropped ? "a click on the connected network's row "
				"disconnected it on the stand-in's Wi-Fi device"
				: "the row click was lost under load");
			if (dropped) {
				fixture_online(false); // the reads go offline with the device
			}
			return go_to("walk_close_panel", 300);
		}},
		{"walk_bluetooth_panel", [] {
			shot("bluetooth-devices");
			return go_to("walk_paired_devices_row", 400);
		}},
		{"walk_paired_devices_row", [] {
			if (!settle_advance("bluetooth-devices")) {
				return go_to("walk_paired_devices_row", 400); // the rows are still settling after the scan
			}
			std::vector<Band> rows = panel_rows("bluetooth-devices");
			walk_attempts = 0;
			check(rows.size() >= 4, std::format("the bluetooth panel lists the "
				"stand-in's two devices ({} rows: switch, scan, 2 devices)",
				rows.size()));
			if (rows.size() < 4) {
				return go_to("walk_close_panel", 300);
			}
			// known devices come first, so the paired one is the second row
			// from the bottom; the found one is last
			click(popup_box.x + popup_box.width / 2.0, rows[rows.size() - 2].centre());
			return go_to("walk_connect_went_to_the_mock", 400);
		}},
		{"walk_connect_went_to_the_mock", [] {
			bool connected = mock_property(BT_NAME, BT_HEADPHONES, "org.bluez.Device1",
				"Connected").find("true") != std::string::npos;
			bool called = mock_called(BT_NAME, BT_HEADPHONES, "Connect");
			if (!(called && connected) && walk_attempts++ < 150) { // in flight, not lost
				return go_to("walk_connect_went_to_the_mock", 400);
			}
			walk_attempts = 0;
			check(called && connected, called || connected
				? "clicking the paired device connected it on the stand-in "
				"(Connect called, Connected reported)"
				: "the paired-device click was lost under load");
			shot("bluetooth-connected");
			return go_to("walk_same_row_again", 400);
		}},
		{"walk_same_row_again", [] {
			if (!settle_advance("bluetooth-connected")) {
				return go_to("walk_same_row_again", 400); // the row is still settling after the connect
			}
			std::vector<Band> rows = panel_rows("bluetooth-connected");
			if (rows.size() < 4) { // the panel moved on without us
				return go_to("walk_close_panel", 300);
			}
			click(popup_box.x + popup_box.width / 2.0, rows[rows.size() - 2].centre());
			return go_to("walk_shot_bluetooth_after", 400);
		}},
		{"walk_shot_bluetooth_after", [] {
			bool off = mock_called(BT_NAME, BT_HEADPHONES, "Disconnect")
				&& mock_property(BT_NAME, BT_HEADPHONES, "org.bluez.Device1",
					"Connected").find("false") != std::string::npos;
			if (!off && walk_attempts++ < 150) { // in flight under load, not lost
				return go_to("walk_shot_bluetooth_after", 400);
			}
			walk_attempts = 0;
			check(off, off ? "clicking the connected device disconnected it on "
				"the stand-in (Disconnect called, Connected false)"
				: "the disconnect click was lost under load");
			shot("bluetooth-after");
			return go_to("walk_found_devices_row", 400);
		}},
		{"walk_found_devices_row", [] {
			if (!settle_advance("bluetooth-after")) {
				return go_to("walk_found_devices_row", 400); // the rows are still settling after the scan
			}
			std::vector<Band> rows = panel_rows("bluetooth-after");
			if (rows.size() < 4) { // the panel moved on without us
				return go_to("walk_close_panel", 300);
			}
			click(popup_box.x + popup_box.width / 2.0, rows[rows.size() - 1].centre());
			return go_to("walk_phase_36", 400);
		}},
		{"walk_phase_36", [] {
			bool paired = mock_called(BT_NAME, BT_SPEAKER, "Pair");
			if (!paired && walk_attempts++ < 150) { // in flight under load, not lost
				return go_to("walk_phase_36", 400);
			}
			walk_attempts = 0;
			check(paired, paired ? "clicking the found device called Pair on "
				"the stand-in" : "the found-device click was lost under load");
			return go_to("walk_close_panel", 300);
		}},
		{"walk_power_panel", [] {
			shot("power-profiles");
			return go_to("walk_power_saver_is_the_first_profile", 400);
		}},
		{"walk_power_saver_is_the_first_profile", [] {
			// the battery hero and the "Power profile" header sit above it, and
			// nothing below it is ever clicked — the Session rows (Suspend,
			// Restart, Power off, Log out) sit below the other two profiles.
			if (!settle_advance("power-profiles")) {
				return go_to("walk_power_saver_is_the_first_profile", 400); // the rows are still settling under load
			}
			std::vector<Band> rows = panel_rows("power-profiles");
			if (rows.size() < 8) { // the panel moved on without us
				return go_to("walk_close_panel", 300);
			}
			// the card's padding, the hero, the header, the gaps and half a row
			constexpr int power_saver_y = 10 + 44 + 6 + 18 + 6 + 16;
			click(popup_box.x + popup_box.width / 2.0, popup_box.y + power_saver_y);
			return go_to("walk_phase_42", 400);
		}},
		{"walk_phase_42", [] {
			std::string profile = mock_property(PPD_NAME, PPD_PATH, PPD_IFACE, "ActiveProfile");
			if (profile.find("power-saver") == std::string::npos && walk_attempts++ < 150) {
				return go_to("walk_phase_42", 400); // in flight under load, not lost
			}
			walk_attempts = 0;
			check(profile.find("power-saver") != std::string::npos,
				"clicking Power saver made the stand-in's active profile power-saver");
			return go_to("walk_close_panel", 300);
		}},
		{"walk_forecast_rows", [] {
			Indicator &ind = indicators[current];
			std::vector<Band> rows = panel_rows(ind.name);
			// the shot may render late
			if (auto wait = wait_for(rows.size() >= 3, {.poll_ms = 400, .budget_ms = 15000})) {
				shot(ind.name);
				return *wait;
			}
			walk_attempts = 0;
			check(rows.size() >= 3, std::format("the weather panel lists the "
				"fixture's forecast days ({} rows)", rows.size()));
			return go_to("walk_close_panel", 400);
		}},
		{"walk_close_panel", [] {
			Indicator &ind = indicators[current];
			switch (ind.close_mode) {
				case close_escape:
				tap(KEY_ESC);
				break;
				case close_icon:
				click(ind.x, bar_mid_y(), ind.button);
				break;
				default:
				click_outside_cards(); // corner of the output, outside every card
				break;
			}
			walk_attempts = 0;
			return go_to("walk_panel_closed", 400);
		}},
		{"walk_panel_closed", [] {
			Indicator &ind = indicators[current];
			if (auto wait = wait_for(panel_layers() <= 0, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layers() == 0, std::format("the {} panel closed ({})", ind.name,
				ind.close_mode == close_escape ? "Escape"
					: ind.close_mode == close_icon ? "a second click of its icon"
					: "a click outside"));
			current++;
			walk_attempts = 0;
			return go_to("walk_open_panel", 300);
		}},
		{"walk_done", [] {
			return after(0);
		}},
		{"walk_one_panel_at_a_time", [] {
			if (clock_x > 0 && network_x > 0) {
				click(clock_x, bar_mid_y());
				return after(400);
			}
			return end_run("the walk found no clock and network indicator to click");
		}},
		{"walk_second_panel_opens", [] {
			if (auto wait = wait_for(panel_layer("clock") != nullptr, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layer("clock") != nullptr, "the clock panel opened");
			click(network_x, bar_mid_y());
			return after(400);
		}},
		{"walk_shot_one_at_a_time", [] {
			if (auto wait = wait_for(!(!(panel_layer("network") != nullptr && panel_layer("clock") == nullptr)), {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layer("network") != nullptr && panel_layer("clock") == nullptr,
				"opening the network panel closed the clock panel (one at a time)");
			shot("one-at-a-time");
			return after(700);
		}},
		{"walk_click_outside", [] {
			click_outside_cards();
			return after(400);
		}},
		{"walk_last_panel_closed_on_a", [] {
			if (auto wait = wait_for(panel_layers() <= 0, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layers() == 0, "the last panel closed on a click outside");
			return after(100); // on to the plugin world: the bar as a plugin host
		}},
	};
}

} // namespace bar
