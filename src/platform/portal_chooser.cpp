#include "platform/portal_chooser.h"

#include <cstdio>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
#include <unistd.h>

#include "core/json.h"
#include "core/text.h"
#include "platform/ipc_client.h"

namespace {

// One line of xdg-desktop-portal-wlr's chooser list: `Monitor: <name>
// <description>` or `Window: <title> (<identifier>)`.
struct ChooserEntry {
	std::string line; // echoed byte-exactly when it is the choice
	std::string monitor; // the monitor's name token; empty for a window
	std::string identifier; // the window's parenthesized identifier; else empty
};

std::vector<ChooserEntry> read_chooser_list() {
	std::vector<ChooserEntry> entries;
	for (std::string line; std::getline(std::cin, line);) {
		if (line.ends_with('\r')) {
			line.pop_back();
		}
		if (line.starts_with("Monitor: ")) {
			std::string rest = line.substr(9);
			entries.push_back({.line = line, .monitor = rest.substr(0, rest.find(' '))});
		} else if (line.starts_with("Window: ") && line.ends_with(')')
				&& line.rfind('(') != std::string::npos) {
			std::size_t open = line.rfind('(');
			entries.push_back({.line = line,
				.identifier = line.substr(open + 1, line.size() - open - 2)});
		}
	}
	return entries;
}

// The object's string `member`, or empty when absent or not a string.
std::string string_member(JsonObject *object, const char *member) {
	const char *text = object != nullptr
		? node_string(json_object_get_member(object, member)) : nullptr;
	return text != nullptr ? std::string{text} : std::string{};
}

} // namespace

int capture_pick_bridge() {
	if (isatty(0)) {
		std::fputs("usage: wayland-canvas msg capture.pick < chooser list\n"
			"       (xdg-desktop-portal-wlr's chooser: the monitors and\n"
			"        windows arrive on stdin, the choice goes to stdout)\n", stderr);
		return 2;
	}
	std::vector<ChooserEntry> entries = read_chooser_list();
	bool windows = false, monitors = false;
	for (const ChooserEntry &entry : entries) {
		windows = windows || !entry.identifier.empty();
		monitors = monitors || !entry.monitor.empty();
	}
	if (!windows && !monitors) {
		return 0; // nothing to choose between: no answer is a cancel
	}
	const char *what = windows && monitors ? "any" : windows ? "window" : "output";
	std::optional<Exchange> exchange = round_trip("capture.pick",
		std::format(R"({{"what":{}}})", text::json_quote(what)));
	if (!exchange.has_value()) {
		return 1;
	}
	JsonObject *answer = exchange->answer;
	if (answer == nullptr || json_object_has_member(answer, "error")
			|| !json_object_has_member(answer, "result")) {
		return 0; // an error answer is the user cancelling: silence, exit 0
	}
	JsonObject *result = node_object(json_object_get_member(answer, "result"));
	std::string want_monitor = string_member(result, "output");
	std::string want_identifier = string_member(result, "identifier");
	for (const ChooserEntry &entry : entries) {
		bool chosen = (!want_identifier.empty() && entry.identifier == want_identifier)
			|| (!want_monitor.empty() && entry.monitor == want_monitor);
		if (chosen) {
			std::printf("%s\n", entry.line.c_str());
			return 0;
		}
	}
	return 0; // a result matching no line is as good as no answer
}
