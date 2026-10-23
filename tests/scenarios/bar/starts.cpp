// What a phase needs at its start, made when the phases before it have not.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

constexpr const char *nm_device_iface = "org.freedesktop.NetworkManager.Device";

bool themed_spawned = false; // themed_window_step has started the window and awaits its map
bool overview_pressed = false; // overview_open_step has sent the chord and awaits the overview
bool second_output_added = false; // second_output_step has added the output and awaits its bar
bool app_window_spawned = false; // app_windows_step has started its window and awaits its map

bool wifi_connected() {
	return mock_property(NM_NAME, NM_DEVICE, nm_device_iface, "State").find("100")
		!= std::string::npos;
}

} // namespace

harness::Step themed_window_step(std::string_view name) {
	return {name, [] {
		if (titled("themed") == nullptr && !themed_spawned) {
			::spawn("foot -T themed sh -c 'exec sleep 600'"); // the compositor's double-forked spawn
			themed_spawned = true;
		}
		if (auto wait = wait_for(titled("themed") != nullptr, {.poll_ms = 300, .budget_ms = 30000})) {
			return *wait;
		}
		themed_spawned = false;
		return after(300);
	}};
}

View *first_mapped_toplevel() {
	for (const std::unique_ptr<View> &view : server->toplevels) {
		if (view->mapped) {
			return view.get();
		}
	}
	return nullptr;
}

harness::Step app_windows_step(std::string_view name, int count) {
	return {name, [count] {
		View *first = first_mapped_toplevel();
		if (app_bar_windows_of(first) < count && !app_window_spawned) {
			::spawn("foot -T sibling sh -c 'exec sleep 600'"); // a window of the first one's app
			app_window_spawned = true;
		}
		if (auto wait = wait_for(app_bar_windows_of(first) >= count, {.poll_ms = 300, .budget_ms = 30000})) {
			return *wait;
		}
		app_window_spawned = false;
		return after(300);
	}};
}

harness::Step wifi_offline_step(std::string_view name) {
	return {name, [] {
		bool connected = wifi_connected();
		if (connected) {
			busctl("call", NM_NAME, NM_DEVICE, nm_device_iface, "Disconnect");
			fixture_online(false); // the nmcli reads go offline with the device
		}
		if (auto wait = wait_for(!connected, {.poll_ms = 300, .budget_ms = 20000})) {
			return *wait;
		}
		return after(300);
	}};
}

harness::Step overview_open_step(std::string_view name) {
	return {name, [] {
		Output *output = out();
		if (output != nullptr && !output->overview.active() && !overview_pressed) {
			chord(KEY_TAB, mods::super);
			overview_pressed = true;
		}
		if (auto wait = wait_for(overview_settled(true) && toolbar_layer() != nullptr,
				{.poll_ms = 400, .budget_ms = 60000})) {
			return *wait;
		}
		overview_pressed = false;
		return after(300);
	}};
}

int bar_layer_count() {
	int n = 0;
	for (const std::unique_ptr<Output> &output : server->outputs) {
		for (auto &vec : output->layers) {
			for (const std::unique_ptr<LayerSurface> &l : vec) {
				if (l->mapped && l->wlr->namespace_ != nullptr
						&& std::strcmp(l->wlr->namespace_, bar_namespace) == 0) {
					n++;
				}
			}
		}
	}
	return n;
}

bool add_headless_output() {
	wlr_backend *headless = nullptr;
	wlr_multi_for_each_backend(server->backend,
		[](wlr_backend *backend, void *data) {
			if (wlr_backend_is_headless(backend)) {
				*static_cast<wlr_backend **>(data) = backend;
			}
		}, &headless);
	return headless != nullptr
		&& wlr_headless_add_output(headless, screen_w, screen_h) != nullptr;
}

harness::Step second_output_step(std::string_view name) {
	return {name, [] {
		if (server->outputs.size() < 2 && !second_output_added) {
			check(add_headless_output(), "a second output was added for the handover");
			second_output_added = true;
		}
		if (auto wait = wait_for(bar_layer_count() >= 2, {.poll_ms = 300, .budget_ms = 30000})) {
			return *wait;
		}
		second_output_added = false;
		return after(500);
	}};
}

double toolbar_gear_centre() {
	LayerSurface *toolbar = toolbar_layer();
	if (toolbar == nullptr) {
		return 0;
	}
	wlr_box strip = toolbar_strip_box(toolbar->geo);
	return strip.y + 4 * toolbar_button + toolbar_button / 2.0; // the settings gear, the strip's fifth button
}

} // namespace bar
