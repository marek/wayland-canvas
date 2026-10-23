// End-to-end foreign-toplevel scenario: what the compositor publishes of a
// window to a taskbar (tests/clients/foreign-client.c, a client of
// wlr-foreign-toplevel-management), and what it does with the taskbar's
// requests. The title and app id once, the state after each change made by
// the compositor or by a request - maximized, minimized, activated,
// fullscreen - the outputs the window enters and leaves, a title that
// changes, and the window's end. Writes only under $CANVAS_TMP.
#include <cstddef>
#include <fcntl.h>
#include <format>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include "core/server.h"
#include "harness/check.h"
#include "harness/child.h"
#include "harness/devices.h"
#include "harness/files.h"
#include "harness/run.h"
#include "windows/view.h"

using namespace harness;

namespace {

Child taskbar; // the foreign-toplevel client
int requests = -1; // the write end of the FIFO the taskbar reads its requests from
std::size_t mark = 0; // what the taskbar had said when the step under way acted

View *titled(std::string_view title) {
	for (View *view : server->views) {
		const char *name = view->shell_title();
		if (view->mapped && name != nullptr && title == name) {
			return view;
		}
	}
	return nullptr;
}

// Remembers where the taskbar's report stands.
void note_report() {
	mark = taskbar.said().size();
}

// Asks the taskbar to send `action` for the window `title`.
void ask(std::string_view action, std::string_view title) {
	note_report();
	std::string line = std::format("{} {}\n", action, title);
	check(write(requests, line.data(), line.size()) == static_cast<ssize_t>(line.size()),
		std::format("the taskbar was asked to {} {}", action, title));
}

// Whether the taskbar has said `line` since the step acted.
bool said_since(std::string_view line) {
	return taskbar.said_line(line, mark);
}

Result taskbar_starts() {
	attach_keyboard();
	std::string fifo = scratch() + "/requests";
	check(mkfifo(fifo.c_str(), 0600) == 0, "the request FIFO was made");
	requests = open(fifo.c_str(), O_RDWR | O_NONBLOCK);
	check(requests >= 0, "the request FIFO opened");
	check(taskbar.start_client("CANVAS_FOREIGN_CLIENT", {fifo}), "the taskbar started");
	return after(300);
}

Result window_opens() {
	if (auto wait = wait_for(taskbar.said_line("READY"))) {
		return *wait;
	}
	check(taskbar.said_line("READY"), "the taskbar bound foreign-toplevel management");
	spawn("foot -T tb sh -c 'exec sleep 600'");
	return after(500);
}

Result window_is_listed() {
	if (auto wait = wait_for(taskbar.said_line("STATE tb foot activated"))) {
		return *wait;
	}
	check(taskbar.said_line("STATE tb foot activated"),
		"the window is listed with its title and app id, and activated");
	check(taskbar.count("TITLE tb") == 1, "its title was sent once");
	return after(0);
}

Result window_enters_the_output() {
	if (auto wait = wait_for(taskbar.said_line("OUTPUT enter tb"))) {
		return *wait;
	}
	check(taskbar.said_line("OUTPUT enter tb"), "the taskbar is told it is on the output");
	return after(0);
}

Result compositor_maximizes() {
	act_when(true, [] {
		note_report();
		titled("tb")->set_maximized(true);
	});
	if (auto wait = wait_for(said_since("STATE tb foot maximized,activated"))) {
		return *wait;
	}
	check(said_since("STATE tb foot maximized,activated"), "maximizing it is published");
	return after(0);
}

Result taskbar_unmaximizes() {
	act_when(true, [] { ask("unmaximize", "tb"); });
	if (auto wait = wait_for(said_since("STATE tb foot activated"))) {
		return *wait;
	}
	check(titled("tb")->mode == WindowMode::normal, "an unmaximize request unmaximizes it");
	check(said_since("STATE tb foot activated"), "and the taskbar is told");
	return after(0);
}

Result taskbar_maximizes() {
	act_when(true, [] { ask("maximize", "tb"); });
	if (auto wait = wait_for(said_since("STATE tb foot maximized,activated"))) {
		return *wait;
	}
	check(titled("tb")->mode == WindowMode::maximized, "a maximize request maximizes it");
	check(said_since("STATE tb foot maximized,activated"), "and the taskbar is told");
	return after(0);
}

Result compositor_unmaximizes() {
	act_when(true, [] {
		note_report();
		titled("tb")->set_maximized(false);
	});
	if (auto wait = wait_for(said_since("STATE tb foot activated"))) {
		return *wait;
	}
	check(said_since("STATE tb foot activated"), "unmaximizing it is published");
	return after(0);
}

Result taskbar_minimizes() {
	act_when(true, [] { ask("minimize", "tb"); });
	if (auto wait = wait_for(said_since("STATE tb foot minimized"))) {
		return *wait;
	}
	check(titled("tb")->minimized, "a minimize request minimizes it");
	check(said_since("STATE tb foot minimized"),
		"the taskbar is told it is minimized and no longer activated");
	return after(0);
}

Result taskbar_activates() {
	act_when(true, [] { ask("activate", "tb"); });
	if (auto wait = wait_for(said_since("STATE tb foot activated"))) {
		return *wait;
	}
	check(!titled("tb")->minimized && server->focused_view == titled("tb"),
		"an activate request restores and focuses it");
	check(said_since("STATE tb foot activated"), "and the taskbar is told");
	return after(0);
}

Result taskbar_fullscreens() {
	act_when(true, [] { ask("fullscreen", "tb"); });
	if (auto wait = wait_for(said_since("STATE tb foot activated,fullscreen"))) {
		return *wait;
	}
	check(titled("tb")->mode == WindowMode::fullscreen, "a fullscreen request fills the output");
	check(said_since("STATE tb foot activated,fullscreen"), "and the taskbar is told");
	return after(0);
}

Result taskbar_leaves_fullscreen() {
	act_when(true, [] { ask("unfullscreen", "tb"); });
	if (auto wait = wait_for(said_since("STATE tb foot activated"))) {
		return *wait;
	}
	check(titled("tb")->mode == WindowMode::normal, "an unfullscreen request ends it");
	check(said_since("STATE tb foot activated"), "and the taskbar is told");
	return after(0);
}

// Whatever changed, a title already sent is not sent again.
Result changes_resend_no_title() {
	check(taskbar.count("TITLE tb") == 1, "changes of state sent the title no more");
	return after(0);
}

Result window_leaves_the_output() {
	act_when(true, [] {
		note_report();
		View *view = titled("tb");
		view->move(view->x + 4000, view->y);
	});
	if (auto wait = wait_for(said_since("OUTPUT leave tb"))) {
		return *wait;
	}
	check(said_since("OUTPUT leave tb"), "moved off every output, the taskbar is told it left");
	return after(0);
}

Result window_returns() {
	act_when(true, [] {
		note_report();
		View *view = titled("tb");
		view->move(view->x - 4000, view->y);
	});
	if (auto wait = wait_for(said_since("OUTPUT enter tb"))) {
		return *wait;
	}
	check(said_since("OUTPUT enter tb"), "moved back, the taskbar is told it entered");
	return after(0);
}

Result taskbar_closes() {
	act_when(true, [] { ask("close", "tb"); });
	if (auto wait = wait_for(said_since("CLOSED tb"))) {
		return *wait;
	}
	check(said_since("CLOSED tb"), "a close request ends the window and the taskbar is told");
	return after(0);
}

Result second_window_opens() {
	act_when(true, [] {
		note_report();
		spawn("foot -T first sh -c 'sleep 2; printf \"\\033]2;second\\007\"; exec sleep 600'");
	});
	if (auto wait = wait_for(said_since("STATE first foot activated"))) {
		return *wait;
	}
	check(said_since("STATE first foot activated"), "a window opened later is listed");
	return after(0);
}

Result title_change_is_published() {
	if (auto wait = wait_for(said_since("STATE second foot activated"))) {
		return *wait;
	}
	check(said_since("STATE second foot activated"), "its new title is published");
	check(taskbar.count("TITLE first") == 1 && taskbar.count("TITLE second") == 1,
		"one title event for each title");
	check(taskbar.running(), "the taskbar is still connected");
	close(requests);
	return finish();
}

} // namespace

int main() {
	if (!begin("foreign", {"WAYLAND_CANVAS_BIN", "CANVAS_FOREIGN_CLIENT"})) {
		return 2;
	}
	Options options;
	options.start_bar = false;
	return run(options, {
		{"taskbar starts", taskbar_starts},
		{"window opens", window_opens},
		{"window is listed", window_is_listed},
		{"window enters the output", window_enters_the_output},
		{"compositor maximizes", compositor_maximizes},
		{"taskbar unmaximizes", taskbar_unmaximizes},
		{"taskbar maximizes", taskbar_maximizes},
		{"compositor unmaximizes", compositor_unmaximizes},
		{"taskbar minimizes", taskbar_minimizes},
		{"taskbar activates", taskbar_activates},
		{"taskbar fullscreens", taskbar_fullscreens},
		{"taskbar leaves fullscreen", taskbar_leaves_fullscreen},
		{"changes resend no title", changes_resend_no_title},
		{"window leaves the output", window_leaves_the_output},
		{"window returns", window_returns},
		{"taskbar closes", taskbar_closes},
		{"second window opens", second_window_opens},
		{"title change is published", title_change_is_published},
	});
}
