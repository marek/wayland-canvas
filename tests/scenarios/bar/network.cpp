// The network panel on the stand-in NetworkManager: Wi-Fi, DNS, band, share, speed, enterprise.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

// The network panel's fixture world, as the tool stand-ins below answer
// it and tests/scenarios/bar-standins/nm.py's mock matches by name:
// HomeNet connected on 2.4 GHz with 5 and 6 reachable, the interface,
// address and gateway the status read reports, and the PSK the secrets
// read returns.
constexpr const char *wifi_iface = "wlan0";

// What this machine's own Wi-Fi is called, asked of the real nmcli in a
// child of its own with the run's private bus undone: the names stay in
// memory for the one check that the panel never reached the real
// NetworkManager, and are written nowhere. Empty when the machine has no
// Wi-Fi or no nmcli.
std::string machine_wifi(const char *field) {
	std::string out = shot_dir + "/machine-wifi.txt";
	std::string cmd = std::format(
		"env -u DBUS_SYSTEM_BUS_ADDRESS -u DBUS_SESSION_BUS_ADDRESS "
		"nmcli -t -f {} device status 2>/dev/null | "
		"awk -F: '$2 == \"wifi\" {{ print $NF }}' | head -1 > '{}'",
		field, out);
	bool listed = std::system(cmd.c_str()) == 0;
	std::string value = listed ? read_file(out) : std::string{};
	unlink(out.c_str());
	while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
		value.pop_back();
	}
	return value;
}

std::string machine_wifi_device() {
	return machine_wifi("DEVICE,TYPE");
}

std::string machine_wifi_ssid() {
	return machine_wifi("CONNECTION,TYPE");
}

// The lines of a tool log.
std::vector<std::string_view> split_log(std::string_view log) {
	std::vector<std::string_view> lines;
	std::size_t at = 0;
	while (at < log.size()) {
		std::size_t end = log.find('\n', at);
		if (end == std::string_view::npos) {
			end = log.size();
		}
		lines.push_back(log.substr(at, end - at));
		at = end + 1;
	}
	return lines;
}

// True when the log holds exactly this line.
bool log_has_line(std::string_view log, std::string_view line) {
	for (std::string_view each : split_log(log)) {
		if (each == line) {
			return true;
		}
	}
	return false;
}

// True when a line of the log that starts with `prefix` also holds `needle`.
bool log_line_names(std::string_view log, std::string_view prefix,
	std::string_view needle) {
	for (std::string_view each : split_log(log)) {
		if (each.starts_with(prefix) && each.find(needle) != std::string_view::npos) {
			return true;
		}
	}
	return false;
}

// The x of a pill, measured from the card's left edge, in the four-up DNS
// row (DHCP, Cloudflare, Google, Custom) and the three-up band row
// (2.4, 5, 6 GHz): the rows run from the padding to the far edge with
// 6 px between pills.
double wifi_dns_pill_x(int index) {
	return 10 + (320.0 - 3 * 6) / 4 * index + (320.0 - 3 * 6) / 8;
}

double wifi_band_pill_x(int index) {
	return 10 + (320.0 - 2 * 6) / 3 * index + (320.0 - 2 * 6) / 6;
}

// Whether a DNS pill carries the highlight on a shot: the active pill
// carries the accent's tint over the card's flat background, and its
// clear top-left corner (the pill's text is centred, lower) stands off
// the background exactly there.
bool dns_pill_lit(const char *name, int pill) {
	cairo_surface_t *s = load_png(name);
	int x = int(std::round(popup_box.x + 10
		+ ((320.0 - 18) / 4 + 6) * pill + 3)); // the stride counts the gap
	int y = int(std::round(popup_box.y + wifi_rows(false, false).dns_pills - 9));
	bool lit = s != nullptr && !pixel_is(s, x, y, shell_bg_dark);
	if (s != nullptr) {
		cairo_surface_destroy(s);
	}
	return lit;
}

// The stand-in world's move to another network: the nmcli stand-in reads
// `roam_ssid`, `roam_freq` and `roam_psk` from its state file, and the
// NetworkManager mock moves the device under the method named, so the
// panel sees the same network through both. The names are raw, as the
// daemon holds them; the nmcli stand-in escapes them as terse mode does.
// An empty `ssid` puts the world back on HomeNet.
int roam_list_base = 0; // the nmcli stand-in's list reads before a roam

void roam_to(const char *method, const char *ssid, int freq, const char *psk) {
	FILE *f = std::fopen((shot_dir + "/nmcli-state").c_str(), "a");
	if (f != nullptr) {
		std::fprintf(f, "roam_ssid=%s\nroam_freq=%d\nroam_psk=%s\n", ssid, freq, psk);
		std::fclose(f);
	}
	busctl("call", NM_NAME, NM_MANAGER, "org.waylandcanvas.barstandins.Wifi", method);
}

} // namespace

std::vector<harness::Step> network_steps() {
	return {
		wifi_offline_step("network_wifi_offline"),
		{"network_open_the_network_panel_on", [] {
			click(network_x, bar_mid_y());
			return after(400);
		}},
		{"network_panel_opened_on_its", [] {
			LayerSurface *panel = panel_layer("network");
			if (auto wait = wait_for(panel != nullptr, {.poll_ms = 100, .budget_ms = 4000})) {
				return *wait;
			}
			check(panel != nullptr, "the network panel opened on its indicator");
			if (auto wait = wait_for(!(panel != nullptr && server->seat->keyboard_state.focused_surface != panel->wlr->surface), {.poll_ms = 100, .budget_ms = 2000})) {
				return *wait;
			}
			check(server->seat->keyboard_state.focused_surface
					== (panel != nullptr ? panel->wlr->surface : nullptr),
				"the network panel holds the keyboard as it opens");
			popup_box = card_box(network_x, network_panel);
			shot("wifi-open");
			return after(700);
		}},
		{"network_disconnected_shape", [] {
			if (!settle_advance("wifi-open")) {
				return again(400);
			}
			WifiOffRows off = wifi_off_rows();
			std::vector<Band> rows = panel_rows("wifi-open");
			// disconnected: the head, two headers, 5 rows — 8 (a 15-row panel
			// means the walk's disconnect never landed)
			check(rows.size() >= 8 && rows.size() <= 9, std::format("the panel "
				"opened on the disconnected stand-in with its five networks "
				"({} rows: head, two headers, 5 rows)", rows.size()));
			if (rows.size() < 8 || rows.size() > 9) {
				return end_run("the panel does not list the disconnected stand-in's "
					"networks, so no row can be clicked safely");
			}
			click(popup_box.x + popup_box.width / 2.0, popup_box.y + off.home);
			return after(500);
		}},
		{"network_known_join_reconnected_the_stand", [] {
			// figures came with the connection
			std::string state = mock_property(NM_NAME, NM_DEVICE,
				"org.freedesktop.NetworkManager.Device", "State");
			// the activation is in flight under load
			if (auto wait = wait_for(state.find("100") != std::string::npos, {.poll_ms = 400, .budget_ms = 40000})) {
				return *wait;
			}
			// (the shape wait below shares this counter: a reset here would
			// let a lagging shape wait forever, as every re-entry resets it)
			check(state.find("100") != std::string::npos,
				"clicking the known network's row reconnected the stand-in's Wi-Fi");
			if (state.find("100") != std::string::npos) {
				fixture_online(true); // the reads come back with the device
			}
			if (!settle_advance("wifi-connected")) {
				return again(400); // the connected shape settles: figures, band, DNS
			}
			wifi_base = panel_rows("wifi-connected").size();
			// The figures and the DNS section come from the panel's status
			// read of the fixture, which lags the activation by a poll or
			// more — and the known list's own read lands later still. Keep
			// settling until the fully grown shape is up: every later click's
			// position, and the row counts the later checks compare against,
			// are modelled on it. Fifteen rows of the card itself: head, four
			// figure rows, band, DNS header and pills, two list headers, five
			// networks; what the desktop shows below the card is not one.
			std::size_t card_n = panel_card_rows("wifi-connected").size();
			// the status poll lags the activation
			if (auto wait = wait_for(card_n >= 15, {.poll_ms = 400, .budget_ms = 64000})) {
				return *wait;
			}
			wifi_base = panel_rows("wifi-connected").size();
			wifi_last_top = panel_rows("wifi-connected").back().top;
			check(card_n >= 15, std::format("the panel grew into its "
				"connected shape ({} rows: head, figures, band, DNS, "
				"headers, five networks)",
				card_n));
			std::string log = standin_log();
			check(log.find("device show " + std::string(wifi_iface))
					!= std::string::npos,
				"the figures read the fixture interface through its tools");
			check(log.find(std::format("ping -n -c 1 -W 1 {}", wifi_gateway))
					!= std::string::npos,
				"the ping figure probes the fixture gateway");
			return after(400);
		}},
		{"network_figures_copy", [] {
			WifiRows wifi = wifi_rows(false, false);
			click(popup_box.x + 120, popup_box.y + wifi.figures[3]);
			return after(400);
		}},
		{"network_copy_reached_the_clipboard_under", [] {
			// The wl-copy stand-in logs the call before the real one under
			// this display has actually set the clipboard: paste until it
			// holds the figure, not just until the call was logged. The
			// paste writes to a file rather than a pipe: reading a child's
			// pipe blocks this process, and this process is also the
			// compositor — its loop must keep answering the paste client
			// or the paste can never answer back. Each entry reaps the
			// previous paste, reads what it wrote, and starts the next.
			static Spawned paste_child{};
			static bool paste_live = false;
			std::string out;
			if (paste_live) {
				if (paste_child.out >= 0) {
					close(paste_child.out);
					paste_child.out = -1;
				}
				reap(paste_child.pid);
				paste_live = false;
				out = read_file(std::format("{}/paste.out", shot_dir));
				while (!out.empty()
						&& (out.back() == '\n' || out.back() == '\r')) {
					out.pop_back();
				}
			}
			// the copy lands on the clipboard a beat later
			if (auto wait = wait_for(out == wifi_ip, {.poll_ms = 400, .budget_ms = 16000})) {
				const std::string redirect = std::format(
					"wl-paste > \"{}/paste.out\" 2>/dev/null", shot_dir);
				const char *const paste[] = {"sh", "-c",
					redirect.c_str(), nullptr};
				paste_child = spawn(paste);
				paste_live = paste_child.pid > 0;
				return *wait;
			}
			paste_live = false;
			check(out == wifi_ip, std::format("the IP address copied itself "
				"(the clipboard holds {})",
				out.empty() ? "nothing" : out.c_str()));
			return after(400);
		}},
		{"network_band_lock", [] {
			WifiRows wifi = wifi_rows(false, false);
			click(popup_box.x + 311, popup_box.y + wifi.band);
			return after(500);
		}},
		{"network_pin_went_to_networkmanager", [] {
			std::string log = standin_log();
			bool applied = log.find("connection modify " + std::string(wifi_ssid)
					+ " 802-11-wireless.band bg") != std::string::npos
				&& log.find("connection up " + std::string(wifi_ssid))
					!= std::string::npos;
			// the pin is still in flight under load
			if (auto wait = wait_for(applied, {.poll_ms = 400, .budget_ms = 40000})) {
				return *wait;
			}
			check(applied, "switching Automatic off pinned the connection to the "
				"2.4 GHz band and reassociated it");
			if (!settle_advance("wifi-band")) {
				return again(400); // the pills settle under the header
			}
			std::vector<Band> rows = panel_rows("wifi-band");
			check(rows.size() > wifi_base, std::format("the band pills appeared "
				"under the header ({} rows, {} before)", rows.size(), wifi_base));
			WifiRows pinned = wifi_rows(true, false);
			click(popup_box.x + wifi_band_pill_x(1), popup_box.y + pinned.pills);
			return after(500);
		}},
		{"network_5_ghz_pill_re_pinned", [] {
			std::string log = standin_log();
			bool applied = log.find("connection modify " + std::string(wifi_ssid)
					+ " 802-11-wireless.band a") != std::string::npos;
			// the re-pin is still in flight under load
			if (auto wait = wait_for(applied, {.poll_ms = 400, .budget_ms = 40000})) {
				return *wait;
			}
			check(applied, "the 5 GHz pill re-pinned the connection's band");
			if (!settle_advance("wifi-band-5")) {
				return again(400); // the highlighted pill settles
			}
			WifiRows wifi = wifi_rows(false, false); // the header's row is fixed
			click(popup_box.x + 311, popup_box.y + wifi.band); // Automatic on
			return after(500);
		}},
		{"network_automatic_restored", [] {
			std::string log = standin_log();
			int pins = standin_calls(log, "connection modify " + std::string(wifi_ssid)
				+ " 802-11-wireless.band");
			// the third pin is still in flight under load
			if (auto wait = wait_for(pins >= 3, {.poll_ms = 400, .budget_ms = 40000})) {
				return *wait;
			}
			check(pins >= 3, std::format("Automatic came back on ({} band pins "
				"in the log: 2.4, 5, auto)", pins));
			if (!settle_advance("wifi-auto")) {
				return again(400); // the pills fold away as the shape settles
			}
			std::vector<Band> rows = panel_rows("wifi-auto");
			check(rows.size() <= wifi_base, std::format("the band pills folded "
				"away with Automatic ({} rows, {} with them)", rows.size(),
				wifi_base));
			WifiRows wifi = wifi_rows(false, false);
			click(popup_box.x + wifi_dns_pill_x(1), popup_box.y + wifi.dns_pills);
			return after(500);
		}},
		{"network_cloudflare_applied_to_the_active", [] {
			std::string log = standin_log();
			bool applied = log.find("ipv4.dns 1.1.1.1 1.0.0.1") != std::string::npos
				&& log.find("device reapply " + std::string(wifi_iface))
					!= std::string::npos;
			// the DNS change is still in flight under load
			if (auto wait = wait_for(applied, {.poll_ms = 400, .budget_ms = 40000})) {
				return *wait;
			}
			// (the highlight wait below shares this counter: a reset here
			// would let a lagging highlight wait forever, as every re-entry
			// resets it)
			check(applied, "the Cloudflare pill set the connection's DNS and "
				"reapplied it to the interface");
			if (!settle_advance("wifi-dns")) {
				return again(400); // the highlighted pill settles
			}
			// The pill's highlight follows the panel's DNS read, a slow poll:
			// re-shot until the tint has moved to the chosen pill.
			bool lit = dns_pill_lit("wifi-dns", 1) && !dns_pill_lit("wifi-dns", 0);
			// shares the applied counter above
			// the DNS read poll lags the click by seconds
			if (auto wait = wait_for(lit, {.poll_ms = 400, .budget_ms = 52000})) {
				shot("wifi-dns");
				return *wait;
			}
			check(lit, "the Cloudflare pill carries the highlight, DHCP lost it");
			WifiRows wifi = wifi_rows(false, false);
			click(popup_box.x + wifi_dns_pill_x(2), popup_box.y + wifi.dns_pills);
			return after(500);
		}},
		{"network_google_applied", [] {
			std::string log = standin_log();
			bool applied = log.find("ipv4.dns 8.8.8.8 8.8.4.4") != std::string::npos;
			// the DNS change is still in flight under load
			if (auto wait = wait_for(applied, {.poll_ms = 400, .budget_ms = 40000})) {
				return *wait;
			}
			// (the highlight wait below shares this counter, as in step 160)
			check(applied, "the Google pill set the connection's DNS");
			if (!settle_advance("wifi-dns")) {
				return again(400); // the highlighted pill settles
			}
			bool lit = dns_pill_lit("wifi-dns", 2) && !dns_pill_lit("wifi-dns", 1);
			// shares the applied counter above
			// the DNS read poll lags the click by seconds
			if (auto wait = wait_for(lit, {.poll_ms = 400, .budget_ms = 52000})) {
				shot("wifi-dns");
				return *wait;
			}
			check(lit, "the Google pill carries the highlight, Cloudflare lost it");
			wifi_last_top = panel_rows("wifi-dns").back().top;
			WifiRows wifi = wifi_rows(false, false);
			click(popup_box.x + wifi_dns_pill_x(3), popup_box.y + wifi.dns_pills);
			return after(500);
		}},
		{"network_custom_pill_opens_its_servers", [] {
			if (!settle_advance("wifi-custom")) {
				return again(400); // the servers field settles under the pills
			}
			std::vector<Band> rows = panel_rows("wifi-custom");
			// the servers field lands under the pills: the rows below shift
			bool armed = rows.size() > wifi_base
				|| (rows.size() == wifi_base && rows.back().top > wifi_last_top + 25);
			check(armed, std::format("the Custom pill opened its servers field "
				"({} rows, was {}; the rows below shifted)", rows.size(), wifi_base));
			if (!armed) {
				return end_run("the Custom pill did not open its servers field");
			}
			WifiRows editing = wifi_rows(false, true);
			click(popup_box.x + popup_box.width / 2.0,
				popup_box.y + editing.dns_field);
			return after(400);
		}},
		{"network_type_the_servers_and_submit", [] {
			type_text("9.9.9.9 149.112.112.112");
			tap(KEY_ENTER);
			return after(400);
		}},
		{"network_typed_servers_applied", [] {
			std::string log = standin_log();
			bool applied = log.find("ipv4.dns 9.9.9.9 149.112.112.112")
					!= std::string::npos;
			// the typed servers are still in flight
			if (auto wait = wait_for(applied, {.poll_ms = 400, .budget_ms = 40000})) {
				return *wait;
			}
			// (the highlight wait below shares this counter, as in step 160)
			check(applied,
				"Enter applied the typed DNS servers to the connection");
			if (!settle_advance("wifi-custom-set")) {
				return again(400); // the folded shape settles
			}
			std::vector<Band> rows = panel_rows("wifi-custom-set");
			check(rows.size() <= wifi_base, std::format("the servers field folded "
				"away ({} rows, {} with it)", rows.size(), wifi_base));
			bool lit = dns_pill_lit("wifi-custom-set", 3);
			// shares the applied counter above
			// the DNS read poll lags the submit by seconds
			if (auto wait = wait_for(lit, {.poll_ms = 400, .budget_ms = 52000})) {
				shot("wifi-custom-set");
				return *wait;
			}
			check(lit, "the Custom pill carries the highlight once its servers "
				"are set");
			WifiRows wifi = wifi_rows(false, false);
			click(popup_box.x + wifi_dns_pill_x(0), popup_box.y + wifi.dns_pills);
			return after(500);
		}},
		{"network_dhcp", [] {
			std::string log = standin_log();
			bool applied = log.find("ipv4.ignore-auto-dns no") != std::string::npos;
			// the DHCP change is still in flight
			if (auto wait = wait_for(applied, {.poll_ms = 400, .budget_ms = 40000})) {
				return *wait;
			}
			// (the highlight wait below shares this counter, as in step 160)
			check(applied, "the DHCP pill gave the DNS back to the connection");
			if (!settle_advance("wifi-dhcp")) {
				return again(400); // the highlighted pill settles
			}
			bool lit = dns_pill_lit("wifi-dhcp", 0) && !dns_pill_lit("wifi-dhcp", 3);
			// shares the applied counter above
			// the DNS read poll lags the click by seconds
			if (auto wait = wait_for(lit, {.poll_ms = 400, .budget_ms = 52000})) {
				shot("wifi-dhcp");
				return *wait;
			}
			check(lit, "the DHCP pill carries the highlight, Custom lost it");
			click(popup_box.x + 241, popup_box.y + wifi_rows(false, false).hero);
			return after(500);
		}},
		{"network_share_view", [] {
			// as its modules — and the matrix has to be a real QR of the
			// fixture's payload, so the ink proves both the light card and
			// the dark modules on it, not just any bright patch
			std::string log = standin_log();
			bool shared = log.find("-s -g 802-11-wireless-security.psk connection show "
					+ std::string(wifi_ssid)) != std::string::npos
				&& log_has_line(log, "qrencode -t ASCII -m 4")
				&& log_has_line(log, std::format(
					"qrencode-stdin WIFI:T:WPA;S:{};P:{};;", wifi_ssid, wifi_psk));
			// the secrets read and the encode are in flight
			if (auto wait = wait_for(shared, {.poll_ms = 400, .budget_ms = 40000})) {
				return *wait;
			}
			check(shared, "the share button fetched the PSK through the "
				"connection's secrets and handed qrencode the WIFI: payload on "
				"its stdin");
			check(!log_line_names(log, "qrencode ", wifi_psk),
				"the password never travelled on qrencode's command line");
			if (!settle_advance("wifi-qr")) {
				return again(400); // the QR settles into the panel
			}
			cairo_surface_t *s = load_png("wifi-qr");
			wlr_box qr{popup_box.x + 70, popup_box.y + 45, 200, 180};
			int ink = bright_count(s, qr);
			cairo_surface_destroy(s);
			check(ink > 10000 && ink < 34000, std::format("the QR is drawn "
				"as its modules on the light card ({} bright pixels: card "
				"and quiet zone light, the modules dark)", ink));
			std::vector<Band> rows = panel_card_rows("wifi-qr");
			if (rows.size() < 2) { // no Back row to click: nothing more to drive
				return end_run("the QR view has no Back row to click");
			}
			click(popup_box.x + popup_box.width / 2.0, rows.back().centre());
			return after(500);
		}},
		{"network_normal_view_back", [] {
			if (!settle_advance("wifi-open2")) {
				return again(400); // the normal view settles back
			}
			check(panel_rows("wifi-open2").size() >= wifi_base,
				std::format("the normal view came back behind the share ({} rows, {} "
					"before)", panel_rows("wifi-open2").size(), wifi_base));
			click(popup_box.x + 273, popup_box.y + wifi_rows(false, false).hero);
			return after(500);
		}},
		{"network_speed_view", [] {
			std::string log = standin_log();
			bool ran = log.find("speedtest --accept-license --accept-gdpr -f json")
					!= std::string::npos;
			// the tool is still running under load
			if (auto wait = wait_for(ran, {.poll_ms = 400, .budget_ms = 40000})) {
				return *wait;
			}
			check(ran, "the speed-test button ran the tool");
			if (!settle_advance("wifi-speed")) {
				return again(400); // the tool's answer settles as rows
			}
			std::vector<Band> rows = panel_card_rows("wifi-speed");
			check(rows.size() >= 6, std::format("the tool's answer is drawn as "
				"rows ({} rows: header, download, upload, ping, back)",
				rows.size()));
			if (rows.size() < 2) { // no Back row to click
				return end_run("the speed test view has no Back row to click");
			}
			click(popup_box.x + popup_box.width / 2.0, rows.back().centre());
			return after(500);
		}},
		{"network_normal_view_back_2", [] {
			if (!settle_advance("wifi-open3")) {
				return again(400); // the normal view settles back
			}
			check(panel_rows("wifi-open3").size() >= wifi_base,
				std::format("the normal view came back behind the speed test ({} rows, "
					"{} before)", panel_rows("wifi-open3").size(), wifi_base));
			wifi_last_top = panel_rows("wifi-open3").back().top;
			WifiRows wifi = wifi_rows(false, false);
			click(popup_box.x + popup_box.width / 2.0, popup_box.y + wifi.cafe);
			return after(500);
		}},
		{"network_prompts_field_inline_under_the", [] {
			if (!settle_advance("wifi-join")) {
				return again(400); // the prompt settles under its row
			}
			std::vector<Band> rows = panel_rows("wifi-join");
			// as before: the field lands under the row, the rows below shift
			bool armed = rows.size() > wifi_base
				|| (rows.size() == wifi_base && rows.back().top > wifi_last_top + 25);
			check(armed, std::format("clicking the secured network armed its "
				"password field ({} rows, was {}; the rows below shifted)",
				rows.size(), wifi_base));
			if (!armed) {
				return end_run("the secured network's password field did not arm");
			}
			WifiRows wifi = wifi_rows(false, false);
			field_box = wlr_box{popup_box.x,
				static_cast<int>(std::round(popup_box.y + wifi.cafe + 22)),
				popup_box.width, 30};
			click(popup_box.x + popup_box.width / 2.0, popup_box.y + wifi.cafe + 37);
			return after(400);
		}},
		{"network_type_the_password_and_submit", [] {
			type_text("correctbattery");
			tap(KEY_ENTER);
			return after(400);
		}},
		{"network_join_went_out_with_the", [] {
			bool sent = mock_called(NM_NAME, NM_MANAGER, "AddAndActivateConnection",
				"correctbattery");
			// the activation is in flight under load
			if (auto wait = wait_for(sent, {.poll_ms = 400, .budget_ms = 60000})) {
				return *wait;
			}
			check(sent, "Enter in the field sent the activation with the typed "
				"password to the stand-in NetworkManager");
			WifiRows wifi = wifi_rows(false, false);
			click(popup_box.x + popup_box.width / 2.0, popup_box.y + wifi.corp);
			return after(500);
		}},
		{"network_enterprise_prompt_asks_for_the", [] {
			if (!settle_advance("wifi-corp")) {
				return again(400); // the identity field settles under the row
			}
			std::vector<Band> rows = panel_rows("wifi-corp");
			// the prompt arms the identity field; the passphrase field
			// takes over only after the identity's Enter (step 174)
			bool armed = rows.size() > wifi_base
				|| (rows.size() == wifi_base
					&& rows.back().top > wifi_last_top + 25);
			check(armed, std::format("clicking the enterprise network armed "
				"its identity field ({} rows, {} before; the rows below "
				"shifted)",
				rows.size(), wifi_base));
			if (!armed) {
				return end_run("the enterprise network's identity field did not arm");
			}
			WifiRows wifi = wifi_rows(false, false);
			click(popup_box.x + popup_box.width / 2.0, popup_box.y + wifi.corp + 37);
			return after(400);
		}},
		{"network_identity", [] {
			type_text("marek");
			tap(KEY_ENTER);
			return after(300);
		}},
		{"network_passphrase", [] {
			// passphrase never travels on a command line
			type_text("vagrantpass");
			tap(KEY_ENTER);
			return after(400);
		}},
		{"network_enterprise_join", [] {
			// the secret fed over stdin, the connection activated
			std::string log = standin_log();
			bool joined = log.find("nmcli connection add") != std::string::npos
				&& log.find("wpa-eap") != std::string::npos
				&& log.find("802-1x.identity marek") != std::string::npos
				&& log.find("nmcli-eap-secret-set") != std::string::npos
				&& log.find("nmcli connection up") != std::string::npos
				&& log.find("vagrantpass") == std::string::npos;
			// the profile creation and activation are in flight
			if (auto wait = wait_for(joined, {.poll_ms = 400, .budget_ms = 60000})) {
				return *wait;
			}
			check(joined, "the enterprise join created its profile with the "
				"identity, fed the passphrase to nmcli over stdin (never on the "
				"command line) and activated it");
			// the prompt must fold away first: until it does, the rows below
			// sit shifted and the open network's row is not where the model
			// puts it. A settled shape the size of the base one is the fold.
			if (!settle_advance("wifi-enterprise")) {
				return again(400); // the shape stops changing first
			}
			std::vector<Band> rows = panel_card_rows("wifi-enterprise");
			bool folded = rows.size() <= wifi_base + 1
				&& rows.back().top < wifi_last_top + 15;
			// the prompt folds as the join's write lands
			if (auto wait = wait_for(folded, {.poll_ms = 400, .budget_ms = 84000})) {
				return *wait;
			}
			// the join made CorpNet known: the list re-sorted and the open
			// network is no longer at its modelled place — it is the last
			// row of the folded shape
			click(popup_box.x + popup_box.width / 2.0, rows.back().centre());
			return after(500);
		}},
		{"network_open_network_joins_on_its", [] {
			// the mock logs the call with the access point's object path,
			// not its SSID
			bool sent = mock_called(NM_NAME, NM_MANAGER,
				"AddAndActivateConnection", "/AccessPoint/library");
			// the activation is in flight under load
			if (auto wait = wait_for(sent, {.poll_ms = 400, .budget_ms = 60000})) {
				return *wait;
			}
			check(sent, "clicking the open network joined it without asking for "
				"anything");
			if (!settle_advance("wifi-library")) {
				return again(400); // the joined row settles
			}
			// the known network's profile is not named after its SSID, so
			// Forget has to find it by what it holds
			std::string profile = busctl("call", NM_NAME,
				"/org/freedesktop/NetworkManager/Settings/oldcafe",
				"org.freedesktop.NetworkManager.Settings.Connection", "GetSettings");
			check(profile.find("Old Cafe profile") != std::string::npos,
				"the forgotten network's profile has a name other than its SSID");
			WifiRows wifi = wifi_rows(false, false);
			click(popup_box.x + 315, popup_box.y + wifi.oldcafe);
			return after(500);
		}},
		{"network_known_network_was_forgotten", [] {
			// NetworkManager was never asked anything
			// The Delete call removes the connection object, so its own call
			// log is gone with it: the settings' connection list is the proof.
			std::string connections = busctl("call", NM_NAME,
				"/org/freedesktop/NetworkManager/Settings",
				"org.freedesktop.NetworkManager.Settings", "ListConnections");
			bool gone = connections.find("oldcafe") == std::string::npos;
			// the Forget is in flight under load
			if (auto wait = wait_for(gone, {.poll_ms = 400, .budget_ms = 60000})) {
				return *wait;
			}
			check(gone, "Forget removed the known network from the settings");
			check(standin_log().find("nmcli connection delete") == std::string::npos,
				"Forget went to NetworkManager itself, not to nmcli by a name");
			return after(300);
		}},
		{"network_roam_to_the_colon_network", [] {
			roam_list_base = standin_calls(standin_log(), "dev wifi list");
			roam_to("RoamColon", "cafe:5G", 5180, wifi_psk);
			return after(500);
		}},
		{"network_status_read_names_the_profile", [] {
			// the band: Automatic off then pins it
			bool moved = mock_called(NM_NAME, NM_MANAGER, "RoamColon");
			int reads = standin_calls(standin_log(), "dev wifi list")
				- roam_list_base;
			// the reads of the new world are in flight
			if (auto wait = wait_for(!((!moved || reads < 3)), {.poll_ms = 500, .budget_ms = 30000})) {
				return *wait;
			}
			check(moved, "the machine roamed to the network named cafe:5G");
			click(popup_box.x + 311, popup_box.y + wifi_rows(false, false).band);
			return after(600);
		}},
		{"network_pin_names_the_profile_as", [] {
			// with the band the list read found beside the colon name
			bool pinned = standin_log().find(
				"connection modify cafe:5G 802-11-wireless.band a")
				!= std::string::npos;
			if (auto wait = wait_for(pinned, {.poll_ms = 1500, .budget_ms = 18000})) {
				// a click before the list read named the band does nothing
				click(popup_box.x + 311, popup_box.y + wifi_rows(false, false).band);
				return *wait;
			}
			check(pinned, "the colon network's band was read beside its escaped "
				"name: Automatic off pinned cafe:5G to 5 GHz");
			click(popup_box.x + 241, popup_box.y + wifi_rows(false, false).hero);
			return after(500);
		}},
		{"network_share_view_2", [] {
			// name, the payload on stdin with the colon escaped
			std::string log = standin_log();
			bool shared = log.find("-s -g 802-11-wireless-security.psk "
					"connection show cafe:5G") != std::string::npos
				&& log_has_line(log, std::format(
					"qrencode-stdin WIFI:T:WPA;S:cafe\\:5G;P:{};;", wifi_psk));
			// the secrets read and the encode are in flight
			if (auto wait = wait_for(shared, {.poll_ms = 400, .budget_ms = 40000})) {
				return *wait;
			}
			check(shared, "the colon network shared as WIFI:T:WPA;S:cafe\\:5G on "
				"qrencode's stdin, its secret read by the profile cafe:5G");
			if (!settle_advance("wifi-qr-colon")) {
				return again(400);
			}
			std::vector<Band> rows = panel_card_rows("wifi-qr-colon");
			if (rows.size() < 2) { // no Back row to click: close, nothing more
				tap(KEY_ESC);
				return go_to("network_panel_closed_behind_the_run", 400);
			}
			click(popup_box.x + popup_box.width / 2.0, rows.back().centre());
			return after(500);
		}},
		{"network_normal_view_back_3", [] {
			if (!settle_advance("wifi-open-colon")) {
				return again(400);
			}
			click(popup_box.x + 311, popup_box.y + wifi_rows(false, false).band);
			return after(500);
		}},
		{"network_pin_released", [] {
			int modifies = standin_calls(standin_log(),
				"connection modify cafe:5G 802-11-wireless.band");
			// the release is in flight under load
			if (auto wait = wait_for(modifies >= 2, {.poll_ms = 400, .budget_ms = 40000})) {
				return *wait;
			}
			check(modifies >= 2, "Automatic back on released the colon "
				"network's pin");
			roam_list_base = standin_calls(standin_log(), "dev wifi list");
			roam_to("RoamSemicolon", "Lounge;Guest", 2437, R"(pa;ss:w"rd\x)");
			return after(500);
		}},
		{"network_semicolon_network_shares", [] {
			bool moved = mock_called(NM_NAME, NM_MANAGER, "RoamSemicolon");
			int reads = standin_calls(standin_log(), "dev wifi list")
				- roam_list_base;
			// the reads of the new world are in flight
			if (auto wait = wait_for(!((!moved || reads < 3)), {.poll_ms = 500, .budget_ms = 30000})) {
				return *wait;
			}
			check(moved, "the machine roamed to the network named Lounge;Guest");
			click(popup_box.x + 241, popup_box.y + wifi_rows(false, false).hero);
			return after(500);
		}},
		{"network_payload_escaped_in_both_fields", [] {
			std::string log = standin_log();
			bool shared = log.find("-s -g 802-11-wireless-security.psk "
					"connection show Lounge;Guest") != std::string::npos
				&& log_has_line(log, R"(qrencode-stdin WIFI:T:WPA;S:Lounge\;Guest;)"
					R"(P:pa\;ss\:w\"rd\\x;;)");
			// the secrets read and the encode are in flight
			if (auto wait = wait_for(shared, {.poll_ms = 400, .budget_ms = 40000})) {
				return *wait;
			}
			check(shared, "the semicolon network shared with its name and its "
				"password escaped, the password read back unescaped from "
				"nmcli's terse output");
			check(!log_line_names(log, "qrencode ", "Lounge")
					&& !log_line_names(log, "qrencode ", "pa;ss"),
				"neither the name nor the password travelled on qrencode's "
				"command line");
			if (!settle_advance("wifi-qr-lounge")) {
				return again(400);
			}
			std::vector<Band> rows = panel_card_rows("wifi-qr-lounge");
			if (rows.size() < 2) { // no Back row to click: close, nothing more
				tap(KEY_ESC);
				return go_to("network_panel_closed_behind_the_run", 400);
			}
			click(popup_box.x + popup_box.width / 2.0, rows.back().centre());
			return after(500);
		}},
		{"network_normal_view_back_4", [] {
			if (!settle_advance("wifi-open-lounge")) {
				return again(400);
			}
			roam_to("RoamHome", "", 0, "");
			return after(1500);
		}},
		{"network_walks_last_proofs", [] {
			// was never asked anything
			check(mock_called(NM_NAME, NM_MANAGER, "RoamHome"),
				"the machine returned to HomeNet");
			std::string log = standin_log();
			// The machine's own interface and the SSID it is on, read here and
			// never written down: had the panel reached the real
			// NetworkManager, either would be all over this log.
			std::string real_device = machine_wifi_device();
			std::string real_ssid = machine_wifi_ssid();
			check((real_device.empty() || log.find(real_device) == std::string::npos)
					&& (real_ssid.empty() || log.find(real_ssid) == std::string::npos),
				"the panel's tools were only ever asked about the fixture world: "
				"no real interface or SSID in any call");
			shot("wifi-final");
			tap(KEY_ESC); // close the panel: the run leaves nothing open
			return after(400);
		}},
		{"network_panel_closed_behind_the_run", [] {
			if (auto wait = wait_for(panel_layers() <= 0, {.poll_ms = 100, .budget_ms = 2000})) {
				return *wait;
			}
			check(panel_layers() == 0, "the network panel closed behind the run");
			return after(100);
		}},
	};
}

} // namespace bar
