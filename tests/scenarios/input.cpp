// End-to-end device-level input scenario: drives the compositor through fake
// wlr_pointer / wlr_keyboard devices announced via backend new_input, i.e.
// the same path libinput takes. Writes only under $CANVAS_TMP (meson passes
// the repo's tmp/).
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <linux/input-event-codes.h>
#include <optional>
#include <print>
#include <ranges>
#include <signal.h>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include "core/box.h"
#include "core/config.h"
#include "platform/output.h"
#include "render/render.h"
#include "core/server.h"
#include "core/wlr.h"

namespace {

std::optional<Server> server;
wl_event_source *timer = nullptr;
int step = 0, failures = 0, retries = 0;
bool finished = false; // the run reached its end; see main()
uint32_t t = 50000;
pid_t grim_pid = -1;
wlr_pointer ptr{};
wlr_keyboard kbd{};
const wlr_pointer_impl ptr_impl = {.name = "fake-pointer"};
const wlr_keyboard_impl kbd_impl = {.name = "fake-keyboard"};
wlr_box saved{};
int saved_x = 0;
View *closed_view = nullptr; // compared by address only, after it is freed
View *second = nullptr; // the window beside "a" on Desktop 1
View *held_view = nullptr; // the window a held button was pressed on
std::optional<DesktopId> right_made; // the desktop a right click made in the overview
std::string shot_dir;

std::string canvas_conf; // this run's scratch canvas.conf
pid_t raw_pid = -1;      // a forked raw socket client, reaped by a later step
pid_t cancel_pid = -1;   // the second forked client of a step that needs two
pid_t x11_pid = -1;      // the scenario's X11 client, its stdout piped here
int x11_out = -1;
std::string x11_lines;   // everything that client has said so far

// Prints PASS/FAIL with the note; a failure counts and names its source line.
void check(bool cond, std::string_view note,
	std::source_location where = std::source_location::current()) {
	std::println("{}: {}", cond ? "PASS" : "FAIL", note);
	std::fflush(stdout);
	if (!cond) {
		failures++;
		std::println(stderr, "  at {}:{} in {}", where.file_name(), where.line(),
			where.function_name());
	}
}

void mkdir_p(const std::string &path) {
	for (std::size_t i = 1; i < path.size(); i++) {
		if (path[i] == '/') {
			mkdir(path.substr(0, i).c_str(), 0755);
		}
	}
	if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
		std::fprintf(stderr, "mkdir %s: %s\n", path.c_str(), std::strerror(errno));
	}
}

Output *out() {
	return server->outputs.front().get();
}

View *titled(const char *title) {
	for (View *v : server->views) {
		const char *name = v->shell_title();
		if (v->mapped && name != nullptr && std::strcmp(name, title) == 0) {
			return v;
		}
	}
	return nullptr;
}

int mapped() {
	int n = 0;
	for (View *v : server->views) {
		n += v->mapped;
	}
	return n;
}

void shot(const char *name) {
	std::string path = std::format("{}/{}.png", shot_dir, name);
	grim_pid = fork();
	if (grim_pid == 0) {
		execlp("grim", "grim", path.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
}

// The board's slices, as fractions of its area.
constexpr Slice WHOLE{0, 0, 1, 1};
constexpr Slice LEFT{0, 0, 0.5, 1}, RIGHT{0.5, 0, 1, 1};
constexpr Slice TOP{0, 0, 1, 0.5}, BOTTOM{0, 0.5, 1, 1};
constexpr Slice TL{0, 0, 0.5, 0.5}, BL{0, 0.5, 0.5, 1};

// A slice's expected frame on a desktop's board: the slice laid into the
// desktop's frame, rounded the way the compositor rounds it (the scenarios
// run with no bar, so the frame is the whole tiling area).
wlr_box slice_frame_of(const Desktop &desktop, Slice slice) {
	return box_snapped(slice_box(rect_of_box(desktop.frame()), slice, config::tile_gap));
}

// The view's frame lies on its slice (clients snap to their own units, so a
// side may sit a few px short of the slice's box).
bool on_slice(View *view, const wlr_box &expected) {
	Rect f = view->frame_rect();
	Rect e = rect_of_box(expected);
	return std::fabs(f.x - e.x) < 3 && std::fabs(f.y - e.y) < 3
		&& e.w - f.w < 12 && e.h - f.h < 12;
}

// Pointer at output-local logical (x, y) on the single 1280x720 output.
void move_to(double x, double y) {
	t += 8;
	wlr_pointer_motion_absolute_event ev = {
		.pointer = &ptr, .time_msec = t, .x = x / 1280.0, .y = y / 720.0};
	wl_signal_emit_mutable(&ptr.events.motion_absolute, &ev);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
}

void button(uint32_t code, bool pressed) {
	t += 8;
	wlr_pointer_button_event ev = {.pointer = &ptr, .time_msec = t, .button = code,
		.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED};
	wl_signal_emit_mutable(&ptr.events.button, &ev);
	wl_signal_emit_mutable(&ptr.events.frame, &ptr);
}

void click(double x, double y, uint32_t code) {
	move_to(x, y);
	button(code, true);
	button(code, false);
}

void drag(double x0, double y0, double x1, double y1) {
	move_to(x0, y0);
	button(BTN_LEFT, true);
	for (int i = 1; i <= 10; i++) {
		move_to(x0 + (x1 - x0) * i / 10, y0 + (y1 - y0) * i / 10);
	}
	button(BTN_LEFT, false);
}

void key(uint32_t code, bool pressed) {
	t += 8;
	wlr_keyboard_key_event ev = {.time_msec = t, .keycode = code, .update_state = true,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED};
	wlr_keyboard_notify_key(&kbd, &ev);
}

// Press mods..., tap k, release mods.
void chord(uint32_t k, std::span<const uint32_t> mods) {
	for (uint32_t m : mods) {
		key(m, true);
	}
	key(k, true);
	key(k, false);
	for (uint32_t m : std::views::reverse(mods)) {
		key(m, false);
	}
}

// --------------------------------------------------- canvas.conf helpers

// A whole file's text, or nullopt when it is missing.
std::optional<std::string> read_file(const std::string &path) {
	FILE *f = std::fopen(path.c_str(), "rb");
	if (f == nullptr) {
		return std::nullopt;
	}
	std::string text;
	char chunk[4096];
	std::size_t n = 0;
	while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) {
		text.append(chunk, n);
	}
	std::fclose(f);
	return text;
}

bool file_exists(const std::string &path) {
	struct stat st{};
	return stat(path.c_str(), &st) == 0;
}

// Saves `text` over canvas.conf the way an editor does: written beside,
// renamed over, so the inotify reload sees one whole file.
bool save_conf(const std::string &text) {
	std::string tmp = canvas_conf + ".hand";
	FILE *f = std::fopen(tmp.c_str(), "wb");
	if (f == nullptr) {
		return false;
	}
	bool ok = text.empty()
		|| std::fwrite(text.data(), 1, text.size(), f) == text.size();
	std::fclose(f);
	return ok && rename(tmp.c_str(), canvas_conf.c_str()) == 0;
}

// True when the running session's resolved keys for `action` (global
// scope) hold `chord`: the observable of a reload that applied.
bool holds_chord(std::string_view action, const settings::Chord &chord) {
	const std::vector<settings::ActionDef> &registry = settings::registry();
	for (std::size_t i = 0; i < registry.size(); i++) {
		if (registry[i].scope != settings::Scope::global
				|| registry[i].name != action) {
			continue;
		}
		for (settings::Chord held : server->conf.keys[i]) {
			if (held == chord) {
				return true;
			}
		}
	}
	return false;
}

// ------------------------------------------------------ raw ipc clients

// Connects to the control socket from a child process (the compositor
// runs in this one, so nothing here may block the parent). -1 on failure.
int child_connect() {
	const char *path = std::getenv("WAYLAND_CANVAS_SOCKET");
	if (path == nullptr || *path == '\0') {
		return -1;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(addr.sun_path)) {
		close(fd);
		return -1;
	}
	strcpy(addr.sun_path, path);
	if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

bool child_write_all(int fd, std::string_view bytes) {
	std::size_t done = 0;
	while (done < bytes.size()) {
		ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
		if (n > 0) {
			done += static_cast<std::size_t>(n);
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			return false;
		}
	}
	return true;
}

// Forks a child that sends `to_send` as raw lines and captures everything
// the compositor sends back, until `quiet_polls` quiet 400 ms polls, EOF
// or 10 s pass; whatever arrived is written under <shot_dir>/<out>. A
// shortcut capture's answer can wait for keys the parent presses only
// after the child is forked, so it gets the long quiet count.
pid_t fork_ipc_client(const std::string &to_send, const char *out,
		int quiet_polls) {
	pid_t pid = fork();
	if (pid != 0) {
		return pid;
	}
	int fd = child_connect();
	FILE *f = std::fopen((shot_dir + "/" + out).c_str(), "w");
	if (fd < 0 || f == nullptr) {
		if (f != nullptr) {
			std::fclose(f);
		}
		_exit(125);
	}
	if (!child_write_all(fd, to_send)) {
		std::fclose(f);
		close(fd);
		_exit(124);
	}
	char buf[1024];
	int idle = 0;
	bool eof = false;
	for (int elapsed = 0; !eof && idle < quiet_polls && elapsed < 10000;
			elapsed += 400) {
		pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
		if (poll(&pfd, 1, 400) == 0) {
			idle++;
			continue;
		}
		idle = 0;
		if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
			break;
		}
		ssize_t n = read(fd, buf, sizeof buf);
		if (n <= 0) {
			eof = true;
			break;
		}
		std::fwrite(buf, 1, static_cast<std::size_t>(n), f);
		std::fflush(f); // the parent reads the file while this child lives
	}
	if (eof) {
		std::fputs("\nEOF\n", f);
	}
	std::fflush(f);
	std::fclose(f);
	close(fd);
	_exit(0);
}

// Reaps a forked client of an earlier step (the last one by default), if it
// has exited.
void reap_client(pid_t &pid = raw_pid) {
	if (pid > 0) {
		waitpid(pid, nullptr, WNOHANG);
		pid = -1;
	}
}

// ------------------------------------------------------ the X11 client

// The scenarios' X11 client (tests/clients/x11-client.c, built as
// CANVAS_X11_CLIENT), stdout piped: it says what keys reach it, so the
// clipboard chords are proven on the far side of Xwayland.
bool start_x11_client() {
	const char *bin = std::getenv("CANVAS_X11_CLIENT");
	if (bin == nullptr || *bin == '\0') {
		return false;
	}
	int fds[2];
	if (pipe(fds) != 0) {
		return false;
	}
	x11_pid = fork();
	if (x11_pid == 0) {
		close(fds[0]);
		dup2(fds[1], 1);
		close(fds[1]);
		execl(bin, "x11-client", "--size=400x300", static_cast<char *>(nullptr));
		_exit(127);
	}
	close(fds[1]);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	x11_out = fds[0];
	x11_lines.clear();
	return x11_pid > 0;
}

std::string_view x11_said() {
	char buf[512];
	ssize_t got = 0;
	while (x11_out >= 0 && (got = read(x11_out, buf, sizeof buf)) > 0) {
		x11_lines.append(buf, static_cast<std::size_t>(got));
	}
	return x11_lines;
}

void reap_x11_client() {
	if (x11_pid > 0) {
		kill(x11_pid, SIGKILL);
		waitpid(x11_pid, nullptr, 0);
		x11_pid = -1;
	}
	if (x11_out >= 0) {
		close(x11_out);
		x11_out = -1;
	}
}

View *x11_view() {
	for (View *view : server->views) {
		if (view->mapped && view->as_x11() != nullptr) {
			return view;
		}
	}
	return nullptr;
}

void swipe(uint32_t fingers, double dx, double dy) {
	t += 8;
	wlr_pointer_swipe_begin_event b = {.pointer = &ptr, .time_msec = t, .fingers = fingers};
	wl_signal_emit_mutable(&ptr.events.swipe_begin, &b);
	for (int i = 0; i < 20; i++) {
		t += 8;
		wlr_pointer_swipe_update_event u = {.pointer = &ptr, .time_msec = t,
			.fingers = fingers, .dx = dx / 20, .dy = dy / 20};
		wl_signal_emit_mutable(&ptr.events.swipe_update, &u);
	}
	t += 8;
	wlr_pointer_swipe_end_event e = {.pointer = &ptr, .time_msec = t};
	wl_signal_emit_mutable(&ptr.events.swipe_end, &e);
}

constexpr uint32_t SUPER[] = {KEY_LEFTMETA};
constexpr uint32_t SUPER_SHIFT[] = {KEY_LEFTMETA, KEY_LEFTSHIFT};
constexpr uint32_t SUPER_CTRL[] = {KEY_LEFTMETA, KEY_LEFTCTRL};
constexpr uint32_t SUPER_ALT[] = {KEY_LEFTMETA, KEY_LEFTALT};
constexpr uint32_t SUPER_CTRL_SHIFT[] = {KEY_LEFTMETA, KEY_LEFTCTRL,
	KEY_LEFTSHIFT};

int run_step() {
	Output *o = out();
	View *a = titled("a"), *b = titled("b");
	switch (step) {
	case 0:
		ptr = wlr_pointer{};
		wlr_pointer_init(&ptr, &ptr_impl, "fake-pointer");
		wl_signal_emit_mutable(&server->backend->events.new_input, &ptr.base);
		wlr_keyboard_init(&kbd, &kbd_impl, "fake-keyboard");
		wl_signal_emit_mutable(&server->backend->events.new_input, &kbd.base);
		spawn("foot -T a sh -c 'echo a; exec sleep 600'");
		return 400;
	case 1:
		spawn("foot -T b sh -c 'echo b; exec sleep 600'");
		return 300;
	case 2:
		if (mapped() < 2 && retries++ < 100) return -100;
		check(a && b && server->focused_view == b, "two windows, newest focused");
		return 100;
	case 3: { // title bar drag moves the window
		wlr_box f = b->frame_box();
		saved_x = b->x;
		int saved_y = b->y;
		drag(f.x + 80, f.y + 16, f.x + 180, f.y + 66);
		check(b->x == saved_x + 100 && b->y == saved_y + 50,
			std::format("title bar drag moved the window by (100,50) -> ({},{})", b->x - saved_x,
				b->y - saved_y));
		return 200;
	}
	case 4: { // double-click title toggles maximize
		wlr_box f = b->frame_box();
		move_to(f.x + 80, f.y + 16);
		click(f.x + 80, f.y + 16, BTN_LEFT);
		click(f.x + 80, f.y + 16, BTN_LEFT);
		check(b->maximized, "double-click on the title maximized");
		return 400;
	}
	case 5: { // the maximize button now shows restore; click it
		wlr_box f = b->frame_box();
		shot("maximized");
		Point restore = button_centre(f, DecoPart::maximize);
		click(restore.x, restore.y, BTN_LEFT);
		check(!b->maximized, "restore button restored the window");
		return 400;
	}
	case 6: { // resize from the top-left corner: right/bottom edges must stay put
		saved = b->content_box();
		wlr_box f = b->frame_box();
		drag(f.x - 3, f.y - 3, f.x - 83, f.y - 53);
		return 400;
	}
	case 7: {
		wlr_box c = b->content_box();
		// foot snaps to whole character cells, so allow up to one cell short.
		check(c.width >= saved.width + 60 && c.height >= saved.height + 30 &&
			c.x + c.width == saved.x + saved.width && c.y + c.height == saved.y + saved.height,
			std::format("top-left corner drag resized {}x{} -> {}x{} with the bottom-right corner fixed",
				saved.width, saved.height, c.width, c.height));
		return 100;
	}
	case 8: { // minimize button
		wlr_box f = b->frame_box();
		Point mini = button_centre(f, DecoPart::minimize);
		move_to(mini.x, mini.y);
		check(b->deco.hover == DecoPart::minimize, "hovering the minimize button highlights it");
		shot("hover-minimize");
		return 300;
	}
	case 9: {
		wlr_box f = b->frame_box();
		Point mini = button_centre(f, DecoPart::minimize);
		click(mini.x, mini.y, BTN_LEFT);
		check(b->minimized && server->focused_view == a, "minimize button hid it, focus moved to a");
		chord(KEY_M, SUPER_SHIFT);
		check(!b->minimized && server->focused_view == b, "Super+Shift+M restored it");
		return 200;
	}
	case 10: { // close button
		wlr_box f = b->frame_box();
		Point close = button_centre(f, DecoPart::close);
		move_to(close.x, close.y);
		shot("hover-close");
		return 300;
	}
	case 11: {
		wlr_box f = b->frame_box();
		click(f.x + 60, f.y + 16, BTN_LEFT); // a title click: b is now the double-click candidate
		closed_view = b;
		Point close = button_centre(f, DecoPart::close);
		click(close.x, close.y, BTN_LEFT);
		return 600;
	}
	case 12:
		check(mapped() == 1 && titled("b") == nullptr, "close button closed the window");
		check(server->input.last_title_view != closed_view,
			"a closed window is no longer the double-click candidate");
		chord(KEY_ENTER, SUPER);
		return 300;
	case 13:
		if (mapped() < 2 && retries++ < 100) return -100;
		check(mapped() == 2, "Super+Return launched a terminal");
		chord(KEY_TAB, SUPER);
		return 500;
	case 14:
		check(o->overview.active(), "Super+Tab opened the overview");
		key(KEY_ESC, true);
		key(KEY_ESC, false);
		return 500;
	case 15:
		check(!o->overview.active() && o->camera.current().zoom == 1,
			"Escape went back");
		for (int i = 0; i < 3; i++) chord(KEY_RIGHT, SUPER_ALT);
		return 600;
	case 16:
		chord(KEY_D, SUPER_CTRL);
		check(server->canvas.desktops.size() == 2,
			"Super+Alt+Right x3 then Super+Ctrl+D pinned Desktop 2");
		chord(KEY_LEFT, SUPER_CTRL);
		return 600;
	case 17: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		check(d1 && o->desktop_showing() == d1, "Super+Ctrl+Left jumped back to Desktop 1");
		move_to(640, 360);
		swipe(3, 300, 0); // fingers right: hop onto the desktop on the right
		return 1200;
	}
	case 18: {
		Desktop *d2 = server->canvas.desktops.by_number(2);
		check(d2 && o->desktop_showing() == d2
			&& o->camera.current().cx == d2->frame().x + d2->frame().width / 2.0,
			std::format("3-finger swipe events from the device hopped onto Desktop 2 ({:.0f})",
				o->camera.current().cx));
		swipe(4, 0, -300);
		return 600;
	}
	case 19:
		check(o->overview.active(), "4-finger swipe events opened the overview");
		shot("overview");
		return 300;
	case 20: { // right-click on empty canvas below the desktops: new desktop
		Desktop *d1 = server->canvas.desktops.by_number(1);
		Point p = o->canvas_to_local(d1->frame().x + d1->frame().width / 2.0,
			d1->frame().y + d1->frame().height * 1.9);
		click(p.x, p.y, BTN_RIGHT);
		check(server->canvas.desktops.size() == 3,
			"right-click on empty canvas added Desktop 3");
		return 400;
	}
	case 21: { // drag Desktop 3 by its tab onto Desktop 2: combine
		shot("three-desktops");
		Desktop *d2 = server->canvas.desktops.by_number(2);
		Desktop *d3 = server->canvas.desktops.by_number(3);
		std::optional<TabLayout> layout = server->painter.overview_tab_layout(*o, *d3);
		Point tx = o->canvas_to_local(d2->frame().x, d2->frame().y);
		Point sx = o->canvas_to_local(d3->frame().x, d3->frame().y);
		drag(layout->tab.x + 30, layout->tab.y + 14, layout->tab.x + 30 + (tx.x - sx.x),
			layout->tab.y + 14 + (tx.y - sx.y));
		check(server->canvas.desktops.size() == 2
			&& server->canvas.desktops.by_number(3) == nullptr,
			"dropping a frame onto another combined them");
		return 400;
	}
	case 22: { // drag Desktop 2's frame over the windows of Desktop 1 (frames move, windows don't)
		Desktop *d2 = server->canvas.desktops.by_number(2);
		std::optional<TabLayout> layout = server->painter.overview_tab_layout(*o, *d2);
		int ax = a->x;
		wlr_box before = d2->frame();
		drag(layout->tab.x + 30, layout->tab.y + 14, layout->tab.x + 30, layout->tab.y + 14 + 40);
		check(d2->frame().y > before.y && a->x == ax, "dragging a frame moves only the frame");
		return 300;
	}
	case 23: { // click the Tile switch on Desktop 1's tab: tiling on
		Desktop *d1 = server->canvas.desktops.by_number(1);
		std::optional<TabLayout> layout = server->painter.overview_tab_layout(*o, *d1);
		click(layout->tile_button.x + layout->tile_button.width / 2.0,
			layout->tile_button.y + layout->tile_button.height / 2.0, BTN_LEFT);
		return 500; // clients commit the new sizes asynchronously
	}
	case 24: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		for (View *v : server->views) {
			if (v->mapped && v != a) {
				second = v;
			}
		}
		check(d1 && second && d1->tiling(), "the tab's Tile switch switched tiling on");
		View *left = a->frame_rect().x <= second->frame_rect().x ? a : second;
		View *right = left == a ? second : a;
		check(a && second && on_slice(left, slice_frame_of(*d1, LEFT))
				&& on_slice(right, slice_frame_of(*d1, RIGHT)),
			"the desktop's two windows took the board's halves");
		check(a->tiled && second->tiled, "tiled windows are told they are tiled");
		shot("tiled-halves");
		return 300;
	}
	case 25: { // leave the overview onto the empty Desktop 2
		Desktop *d2 = server->canvas.desktops.by_number(2);
		Point c = d2->centre();
		o->overview.target_point(c.x, c.y);
		o->overview.exit();
		return 700;
	}
	case 26: {
		Desktop *d2 = server->canvas.desktops.by_number(2);
		if ((o->overview.active() || o->desktop_showing() != d2) && retries++ < 100) {
			return -100;
		}
		check(!o->overview.active() && o->desktop_showing() == d2,
			"left the overview onto Desktop 2");
		// An empty desktop takes the switch too; its first window fills it.
		chord(KEY_T, SUPER);
		check(d2->tiling() && server->hint.text == "Tiling on for Desktop 2",
			"Super+T switched tiling on for the empty desktop");
		spawn("foot -T c sh -c 'echo c; exec sleep 600'");
		return 400;
	}
	case 27:
		if (mapped() < 3 && retries++ < 100) {
			return -100;
		}
		return 500; // the client commits the slice size
	case 28: { // Super+Left takes the whole board's window to the left half
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c");
		check(c && c->board == d2->id() && c->tiled
				&& on_slice(c, slice_frame_of(*d2, WHOLE)),
			"the first window on the empty tiled desktop filled it");
		shot("tiled-first-fills");
		chord(KEY_LEFT, SUPER);
		return 500;
	}
	case 29:
		check(on_slice(titled("c"), slice_frame_of(
			*server->canvas.desktops.by_number(2), LEFT)),
			"Super+Left took the window to the left half");
		chord(KEY_DOWN, SUPER);
		return 500;
	case 30: // Left then Down reaches the bottom-left quarter
		check(on_slice(titled("c"), slice_frame_of(
			*server->canvas.desktops.by_number(2), BL)),
			"Super+Left then Super+Down took the window to the bottom-left quarter");
		shot("slice-bl");
		chord(KEY_UP, SUPER); // the opposite key widens back to the left half
		return 500;
	case 31:
		check(on_slice(titled("c"), slice_frame_of(
			*server->canvas.desktops.by_number(2), LEFT)),
			"Super+Up widened the bottom-left quarter back to the left half");
		chord(KEY_RIGHT, SUPER); // and on to the whole board
		return 500;
	case 32:
		check(on_slice(titled("c"), slice_frame_of(
			*server->canvas.desktops.by_number(2), WHOLE)),
			"Super+Right widened the left half back to the whole board");
		chord(KEY_DOWN, SUPER); // the other order: Down first, then Left
		return 500;
	case 33:
		check(on_slice(titled("c"), slice_frame_of(
			*server->canvas.desktops.by_number(2), BOTTOM)),
			"Super+Down took the whole board's window to the bottom half");
		chord(KEY_LEFT, SUPER);
		return 500;
	case 34: // both orders land on the bottom-left quarter
		check(on_slice(titled("c"), slice_frame_of(
			*server->canvas.desktops.by_number(2), BL)),
			"Super+Down then Super+Left also took the window to the bottom-left quarter");
		// A second window takes the largest free slice: the top half wins the
		// tie with the right one by reading order.
		spawn("foot -T d sh -c 'echo d; exec sleep 600'");
		return 400;
	case 35: {
		if (mapped() < 4 && retries++ < 100) {
			return -100;
		}
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d");
		check(d && on_slice(d, slice_frame_of(*d2, TOP))
				&& on_slice(c, slice_frame_of(*d2, BL)),
			"the new window took the largest free slice, the top half");
		// Focus c through its title bar and grow it over d. The frame is in
		// canvas coordinates; the pointer takes output-local ones.
		wlr_box f = c->frame_box();
		Point title = o->canvas_to_local(f.x + 80, f.y + 16);
		click(title.x, title.y, BTN_LEFT);
		return 300;
	}
	case 36:
		check(server->focused_view == titled("c"), "the clicked window is focused");
		chord(KEY_UP, SUPER); // bottom-left quarter widens to the left half, over d
		return 500;
	case 37: {
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d");
		check(on_slice(c, slice_frame_of(*d2, LEFT))
				&& on_slice(d, slice_frame_of(*d2, RIGHT)),
			"the grown slice pushed the covered window into the free space");
		check(!c->frame_rect().intersects(d->frame_rect()), "tiled windows do not overlap");
		shot("slice-push");
		// A third window with no free space left: it halves a held slice.
		spawn("foot -T e sh -c 'echo e; exec sleep 600'");
		return 400;
	}
	case 38: {
		if (mapped() < 5 && retries++ < 100) {
			return -100;
		}
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d"), *e = titled("e");
		check(e && on_slice(e, slice_frame_of(*d2, BL))
				&& on_slice(c, slice_frame_of(*d2, TL))
				&& on_slice(d, slice_frame_of(*d2, RIGHT)),
			"with no free slice left, the new window halved the left half");
		shot("slice-halved");
		titled("e")->close(); // its slice frees up
		return 600;
	}
	case 39: {
		if (mapped() > 4 && retries++ < 100) {
			return -100;
		}
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d");
		check(titled("e") == nullptr && on_slice(c, slice_frame_of(*d2, TL))
				&& on_slice(d, slice_frame_of(*d2, RIGHT)),
			"the closed window's slice stayed free, the others kept theirs");
		spawn("foot -T f sh -c 'echo f; exec sleep 600'");
		return 400;
	}
	case 40: {
		if (mapped() < 5 && retries++ < 100) {
			return -100;
		}
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d"), *f = titled("f");
		check(f && on_slice(f, slice_frame_of(*d2, BL))
				&& on_slice(c, slice_frame_of(*d2, TL))
				&& on_slice(d, slice_frame_of(*d2, RIGHT)),
			"the next window took the closed window's slice");
		shot("slice-refilled");
		// Drag c onto d's slice: they swap. Canvas coordinates in, the
		// pointer takes output-local ones.
		wlr_box fc = c->frame_box(), fd = d->frame_box();
		Point from = o->canvas_to_local(fc.x + 80, fc.y + 16);
		Point to = o->canvas_to_local(
			fd.x + fd.width / 2.0 - fc.width / 2.0 + 80,
			fd.y + fd.height / 2.0 - fc.height / 2.0 + 16);
		drag(from.x, from.y, to.x, to.y);
		return 500;
	}
	case 41: {
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d"), *f = titled("f");
		check(on_slice(c, slice_frame_of(*d2, RIGHT))
				&& on_slice(d, slice_frame_of(*d2, TL))
				&& on_slice(f, slice_frame_of(*d2, BL)),
			"dragging a window onto the other's slice swapped them");
		check(c->tiled && d->tiled && f->tiled, "both windows stayed tiled");
		shot("slice-swap");
		// Double-click c's title: maximize keeps the slice reserved.
		wlr_box frame = c->frame_box();
		Point title = o->canvas_to_local(frame.x + 80, frame.y + 16);
		click(title.x, title.y, BTN_LEFT);
		click(title.x, title.y, BTN_LEFT);
		return 500;
	}
	case 42: {
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d"), *f = titled("f");
		check(c && c->maximized && !c->tiled && c->board == d2->id()
				&& c->slice == RIGHT,
			"maximize left the board but kept the window's slice");
		check(on_slice(d, slice_frame_of(*d2, TL))
				&& on_slice(f, slice_frame_of(*d2, BL)),
			"maximize moved no other window: no re-layout happens anymore");
		shot("slice-maximized");
		wlr_box frame = c->frame_box();
		Point maximize = button_centre(frame, DecoPart::maximize);
		Point restore = o->canvas_to_local(maximize.x, maximize.y);
		click(restore.x, restore.y, BTN_LEFT);
		return 500;
	}
	case 43: {
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d"), *f = titled("f");
		check(!c->maximized && c->tiled && c->board == d2->id()
				&& on_slice(c, slice_frame_of(*d2, RIGHT))
				&& on_slice(d, slice_frame_of(*d2, TL))
				&& on_slice(f, slice_frame_of(*d2, BL)),
			"leaving maximize put the window back on its reserved slice");
		// Minimize: the slice frees up but is remembered.
		wlr_box frame = c->frame_box();
		Point minimize = button_centre(frame, DecoPart::minimize);
		Point mini = o->canvas_to_local(minimize.x, minimize.y);
		click(mini.x, mini.y, BTN_LEFT);
		return 400;
	}
	case 44: {
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d"), *f = titled("f");
		check(c && c->minimized && !c->tiled && c->board == d2->id()
				&& c->slice == RIGHT,
			"minimize freed the slice but kept it remembered");
		check(on_slice(d, slice_frame_of(*d2, TL))
				&& on_slice(f, slice_frame_of(*d2, BL)),
			"minimize moved no other window either");
		chord(KEY_M, SUPER_SHIFT); // restore it
		return 500;
	}
	case 45: {
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *c = titled("c"), *d = titled("d"), *f = titled("f");
		check(!c->minimized && c->tiled && on_slice(c, slice_frame_of(*d2, RIGHT))
				&& on_slice(d, slice_frame_of(*d2, TL))
				&& on_slice(f, slice_frame_of(*d2, BL)),
			"the restored window took its own slice back, still free");
		// Nothing lies right of Desktop 2: the swipe events from the device
		// leave the view where it is.
		swipe(3, 900, 0);
		return 600;
	}
	case 46: {
		Desktop *d2 = server->canvas.desktops.by_number(2);
		check(o->desktop_showing() == d2 && !o->camera.panning()
				&& server->hint.text == "No desktop that way",
			"a 3-finger swipe toward no desktop stays on Desktop 2 and says so");
		// The free view beside it is a pan away.
		for (int i = 0; i < 3; i++) chord(KEY_RIGHT, SUPER_ALT);
		check(o->desktop_showing() == nullptr, "Super+Alt+Right three times panned onto a free view");
		chord(KEY_RIGHT, SUPER_CTRL_SHIFT); // move the focused window here
		return 600;
	}
	case 47: {
		View *c = titled("c");
		check(c && !c->tiled && !c->board.has_value()
				&& server->canvas.desktop_of(*c) == nullptr,
			"moving out freed the window's slice and left the board");
		chord(KEY_UP, SUPER); // a floating window still maximizes
		return 500;
	}
	case 48: {
		View *c = titled("c");
		Rect fr = c->frame_rect(), vp = o->viewport();
		check(c->maximized && std::fabs(fr.x - vp.x) < 1 && std::fabs(fr.y - vp.y) < 1
				&& std::fabs(fr.w - vp.w) < 1 && std::fabs(fr.h - vp.h) < 1,
			"Super+Up still toggles maximize on a floating desktop");
		shot("floating-maximized");
		chord(KEY_UP, SUPER); // and back
		return 500;
	}
	case 49: {
		View *c = titled("c");
		check(c && !c->maximized, "Super+Up restored the floating window");
		// Pin this free view as a desktop with tiling on: c joins it.
		chord(KEY_T, SUPER);
		Desktop *d3 = server->canvas.desktops.by_number(3);
		check(d3 && d3->tiling() && o->desktop_showing() == d3,
			"Super+T pinned the free view as a desktop with tiling on");
		check(server->hint.text == "Tiling on for Desktop 3",
			"the hint names the new tiled desktop");
		return 400;
	}
	case 50: {
		Desktop *d3 = server->canvas.desktops.by_number(3);
		View *c = titled("c");
		check(c && c->board == d3->id() && on_slice(c, slice_frame_of(*d3, WHOLE)),
			"the window standing there joined the new desktop's board");
		// Back to Desktop 1 (via 2): its tiling survived, boxes unmoved.
		chord(KEY_LEFT, SUPER_CTRL);
		return 800;
	}
	case 51: {
		if (o->desktop_showing() != server->canvas.desktops.by_number(2)
				&& retries++ < 100) {
			return -100;
		}
		chord(KEY_LEFT, SUPER_CTRL);
		return 800;
	}
	case 52: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		if (o->desktop_showing() != d1 && retries++ < 100) {
			return -100;
		}
		View *left = a->frame_rect().x <= second->frame_rect().x ? a : second;
		View *right = left == a ? second : a;
		check(d1 && o->desktop_showing() == d1 && d1->tiling(),
			"back on Desktop 1, tiling still on");
		check(on_slice(left, slice_frame_of(*d1, LEFT))
				&& on_slice(right, slice_frame_of(*d1, RIGHT)),
			"and its two windows kept their slices all along");
		// `t` in the overview switches tiling off: boxes stay, boards drop.
		chord(KEY_TAB, SUPER);
		return 700;
	}
	case 53: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		if (!o->overview.active() && retries++ < 100) {
			return -100;
		}
		check(o->overview.active() && o->overview.target().desktop == d1->id(),
			"the overview targets Desktop 1");
		key(KEY_T, true);
		key(KEY_T, false);
		return 500;
	}
	case 54: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		View *left = a->frame_rect().x <= second->frame_rect().x ? a : second;
		View *right = left == a ? second : a;
		Rect fl = left->frame_rect(), fr = right->frame_rect();
		check(d1 && !d1->tiling() && !a->tiled && !second->tiled
				&& !a->board.has_value() && !second->board.has_value(),
			"`t` in the overview switched tiling off, boards dropped");
		check(on_slice(left, slice_frame_of(*d1, LEFT))
				&& on_slice(right, slice_frame_of(*d1, RIGHT)),
			"switching off left the windows where the board had them");
		check(fl.x == left->frame_rect().x && fr.x == right->frame_rect().x,
			"not a pixel moved when the board went away");
		shot("tiled-off-kept");
		// The tab's Tile switch brings it back: re-placed as they stood.
		std::optional<TabLayout> layout = server->painter.overview_tab_layout(*o, *d1);
		click(layout->tile_button.x + layout->tile_button.width / 2.0,
			layout->tile_button.y + layout->tile_button.height / 2.0, BTN_LEFT);
		return 500;
	}
	case 55: {
		Desktop *d1 = server->canvas.desktops.by_number(1);
		View *left = a->frame_rect().x <= second->frame_rect().x ? a : second;
		View *right = left == a ? second : a;
		check(d1 && d1->tiling() && a->tiled && second->tiled
				&& on_slice(left, slice_frame_of(*d1, LEFT))
				&& on_slice(right, slice_frame_of(*d1, RIGHT)),
			"the tab's Tile switch laid the standing windows back on the same slices");
		// And off again, for the combine below.
		key(KEY_T, true);
		key(KEY_T, false);
		return 500;
	}
	case 56: { // combine the tiled Desktop 2 into the untiled Desktop 1
		Desktop *d1 = server->canvas.desktops.by_number(1);
		Desktop *d2 = server->canvas.desktops.by_number(2);
		View *d = titled("d"), *f = titled("f");
		Rect d0 = d->frame_rect(), f0 = f->frame_rect();
		check(d1 && d2 && !d1->tiling() && d2->tiling(),
			"Desktop 1 untiled, Desktop 2 still tiled, before the combine");
		int dx = d1->frame().x - d2->frame().x;
		int dy = d1->frame().y - d2->frame().y;
		server->canvas.combine(d1->id(), d2->id(), d2->frame());
		check(server->canvas.desktops.by_number(2) == nullptr,
			"the tiled desktop combined into the untiled one");
		check(d->frame_rect().x == d0.x + dx && d->frame_rect().y == d0.y + dy
				&& f->frame_rect().x == f0.x + dx && f->frame_rect().y == f0.y + dy,
			"its windows moved with the translation, keeping their layout");
		check(!d->tiled && !f->tiled
				&& server->canvas.desktop_of(*d) == d1,
			"no longer tiled, they belong to the target desktop");
		shot("combined-untiled");
		return 400;
	}
	case 57: { // the overview's right clicks: on the empty canvas, a new desktop
		check(o->overview.active(), "the overview is still open for the right clicks");
		Desktop *d1 = server->canvas.desktops.by_number(1);
		Point c{d1->frame().x + d1->frame().width / 2.0,
			d1->frame().y + d1->frame().height * 1.9};
		check(server->canvas.desktops.at(c.x, c.y) == nullptr
				&& !server->view_at(c.x, c.y, o->camera.drawn().zoom).has_value(),
			"the spot below Desktop 1 is empty canvas");
		std::size_t before = server->canvas.desktops.size();
		Point p = o->canvas_to_local(c.x, c.y);
		click(p.x, p.y, BTN_RIGHT);
		Desktop *made = server->canvas.desktops.at(c.x, c.y);
		check(made != nullptr && server->canvas.desktops.size() == before + 1,
			"a right click on the empty canvas made a desktop there");
		right_made = made != nullptr ? std::optional(made->id()) : std::nullopt;
		return 400;
	}
	case 58: { // inside a desktop, clear of windows: that desktop goes
		Desktop *made = server->canvas.desktops.find(right_made);
		std::size_t before = server->canvas.desktops.size();
		if (made != nullptr) {
			Point p = o->canvas_to_local(made->centre().x, made->centre().y);
			click(p.x, p.y, BTN_RIGHT);
		}
		check(made != nullptr && server->canvas.desktops.find(right_made) == nullptr
				&& server->canvas.desktops.size() == before - 1,
			"a right click inside a desktop, clear of windows, removed it");
		return 400;
	}
	case 59: { // on a window: nothing, the window's desktop stays
		Desktop *d1 = server->canvas.desktops.by_number(1);
		View *d = titled("d");
		std::size_t before = server->canvas.desktops.size();
		Point p = o->canvas_to_local(d->frame_rect().cx(), d->frame_rect().cy());
		click(p.x, p.y, BTN_RIGHT);
		check(server->canvas.desktops.by_number(1) == d1
				&& server->canvas.desktops.size() == before,
			"a right click on a window inside Desktop 1 left the desktop alone");
		return 400;
	}
	case 60: { // on a desktop's tab: that desktop goes, its windows stay
		Desktop *d1 = server->canvas.desktops.by_number(1);
		DesktopId id = d1->id();
		View *d = titled("d"), *f = titled("f");
		Rect d0 = d->frame_rect(), f0 = f->frame_rect();
		std::optional<TabLayout> tab = server->painter.overview_tab_layout(*o, *d1);
		if (tab.has_value()) {
			click(tab->tab.x + 30, tab->tab.y + tab->tab.height / 2.0, BTN_RIGHT);
		}
		check(tab.has_value() && server->canvas.desktops.find(id) == nullptr,
			"a right click on Desktop 1's tab removed it");
		check(d->frame_rect().x == d0.x && d->frame_rect().y == d0.y
				&& f->frame_rect().x == f0.x && f->frame_rect().y == f0.y
				&& server->canvas.desktop_of(*d) == nullptr,
			"its windows stayed where they were, on no desktop");
		shot("right-click-removed");
		return 400;
	}
	case 61: { // canvas.conf: written with every default, chords from it
		key(KEY_ESC, true);
		key(KEY_ESC, false);
		check(!o->overview.active(), "Escape left the overview");
		std::optional<std::string> text = read_file(canvas_conf);
		check(text.has_value() && *text == settings::template_text(),
			"the first run wrote canvas.conf with every default");
		key(KEY_LEFTMETA, true);
		key(KEY_SPACE, true);
		key(KEY_SPACE, false);
		key(KEY_LEFTMETA, false);
		check(server->launcher.is_open(), "Super+Space opened the launcher");
		key(KEY_LEFTMETA, true);
		key(KEY_SPACE, true);
		key(KEY_SPACE, false);
		key(KEY_LEFTMETA, false);
		check(!server->launcher.is_open(), "its own chord closed it");
		key(KEY_LEFTMETA, true);
		key(KEY_LEFTMETA, false);
		check(!server->launcher.is_open(),
			"a lone Super tap runs nothing by default");
		return 300;
	}
	case 62: { // a hand edit saves over the file; the session applies it
		std::optional<std::string> text = read_file(canvas_conf);
		std::string from = "launcher = Super+Space";
		std::size_t at = text.has_value() ? text->find(from) : std::string::npos;
		if (at != std::string::npos) {
			text->replace(at, from.size(), "launcher = Super+K");
			save_conf(*text);
		}
		return 300;
	}
	case 63: { // Super+K now opens it, without any restart
		bool applied = holds_chord("launcher",
			{settings::mod_logo, XKB_KEY_k});
		if (!applied && retries++ < 100) {
			return -100;
		}
		check(applied, "saving canvas.conf applied to the running session");
		key(KEY_LEFTMETA, true);
		key(KEY_K, true);
		key(KEY_K, false);
		key(KEY_LEFTMETA, false);
		check(server->launcher.is_open(), "Super+K, from the file, opened the launcher");
		key(KEY_LEFTMETA, true);
		key(KEY_K, true);
		key(KEY_K, false);
		key(KEY_LEFTMETA, false);
		check(!server->launcher.is_open(), "and closed it again");
		key(KEY_LEFTMETA, true);
		key(KEY_SPACE, true);
		key(KEY_SPACE, false);
		key(KEY_LEFTMETA, false);
		check(!server->launcher.is_open(),
			"Super+Space, replaced in the file, opens nothing");
		return 300;
	}
	case 64: { // a hand-written file with comments, for the edits below
		save_conf("# my wayland-canvas\n[shortcuts]\n# the launcher\n"
			"launcher = Super+K\n\n[commands]\n# Super+B runs a command\n");
		return 300;
	}
	case 65: {
		bool applied = holds_chord("launcher",
			{settings::mod_logo, XKB_KEY_k});
		if (!applied && retries++ < 100) {
			return -100;
		}
		// settings.set takes Super+K from the launcher for the terminal.
		raw_pid = fork_ipc_client(
			"{\"id\":1,\"method\":\"settings.set\",\"params\":"
			"{\"action\":\"terminal\",\"keys\":[\"Super+K\"]}}\n",
			"set-terminal.out", 3);
		return 400;
	}
	case 66: { // one line rewritten, every other byte of the user's file kept
		reap_client();
		std::string out = read_file(shot_dir + "/set-terminal.out")
			.value_or("");
		check(out.find("\"moved\":[{\"scope\":\"global\","
				"\"action\":\"launcher\",\"keys\":[\"Super+K\"]}") != std::string::npos,
			"settings.set reported the chord taken from the launcher");
		std::string text = read_file(canvas_conf).value_or("");
		check(text.find("# my wayland-canvas\n[shortcuts]\n# the launcher\n"
				"terminal = Super+K\n") == 0,
			"only the launcher's line was rewritten, comments kept");
		check(text.find("\n[commands]\n# Super+B runs a command\n")
				!= std::string::npos,
			"the [commands] section and its comment stayed");
		check(holds_chord("terminal", {settings::mod_logo, XKB_KEY_k}),
			"the edit applied at once");
		return 300;
	}
	case 67: { // a [commands] chord runs its command
		raw_pid = fork_ipc_client(
			std::format("{{\"id\":2,\"method\":\"settings.set\",\"params\":"
				"{{\"commands\":[{{\"keys\":\"Super+B\","
				"\"command\":\"touch {}/command-marker\"}}]}}}}\n", shot_dir),
			"set-commands.out", 3);
		return 400;
	}
	case 68: {
		reap_client();
		std::string out = read_file(shot_dir + "/set-commands.out")
			.value_or("");
		check(out.find("\"commands\":[{\"keys\":\"Super+B\"") != std::string::npos,
			"settings.set wrote the [commands] entry");
		key(KEY_LEFTMETA, true);
		key(KEY_B, true);
		key(KEY_B, false);
		key(KEY_LEFTMETA, false);
		return 300;
	}
	case 69: {
		if (!file_exists(shot_dir + "/command-marker") && retries++ < 100) {
			return -100;
		}
		check(file_exists(shot_dir + "/command-marker"),
			"a [commands] chord ran its command");
		return 300;
	}
	case 70: { // a bad line: ignored with its number, hinted on every screen
		if (std::optional<std::string> text = read_file(canvas_conf)) {
			std::string in = "[shortcuts]\n";
			std::size_t at = text->find(in);
			if (at != std::string::npos) {
				text->insert(at + in.size(), "lancher = Super+P\n");
			}
			save_conf(*text);
		}
		return 300;
	}
	case 71: {
		if (server->conf_errors.empty() && retries++ < 100) {
			return -100;
		}
		check(!server->conf_errors.empty()
				&& server->conf_errors.front().text == "unknown action 'lancher'",
			"the ignored line is listed with its reason");
		check(server->hint.text.starts_with("canvas.conf line ")
				&& server->hint.text.find("unknown action 'lancher'")
					!= std::string::npos,
			std::format("the first error shows as a hint ({})",
				server->hint.text));
		return 300;
	}
	case 72: { // the gesture choices from the file reach the gesture engine
		if (std::optional<std::string> text = read_file(canvas_conf)) {
			save_conf(*text + "\n[gestures]\nswipes = four-finger-hops\n");
		}
		return 300;
	}
	case 73: {
		if (server->gestures.options().swipes != SwipePreset::four_finger_hops
				&& retries++ < 100) {
			return -100;
		}
		check(server->gestures.options().swipes == SwipePreset::four_finger_hops,
			"the file's swipe preset reached the gesture engine");
		return 300;
	}
	case 74: { // a subscriber sees the settings change as an event
		raw_pid = fork_ipc_client(
			"{\"id\":1,\"method\":\"subscribe\"}\n"
			"{\"id\":2,\"method\":\"settings.set\",\"params\":"
			"{\"pinchScale\":false}}\n",
			"settings-event.out", 4);
		return 600;
	}
	case 75: {
		reap_client();
		std::string out = read_file(shot_dir + "/settings-event.out")
			.value_or("");
		check(out.find("\"event\":\"settings\"") != std::string::npos
				&& out.find("\"pinchScale\":false") != std::string::npos,
			"a subscriber received the settings event");
		return 300;
	}
	case 76: { // a recorded chord: every key belongs to the capture
		// The terminal spawned by an early step reads into this marker:
		// a fresh one makes "ran nothing" observable.
		unlink((shot_dir + "/terminal-marker").c_str());
		raw_pid = fork_ipc_client(
			"{\"id\":7,\"method\":\"shortcut.capture\"}\n",
			"capture-chord.out", 24);
		return 400;
	}
	case 77: { // Super+K (the terminal's chord now) recorded: run by nothing
		key(KEY_LEFTMETA, true);
		key(KEY_K, true);
		key(KEY_K, false);
		key(KEY_LEFTMETA, false);
		return 400;
	}
	case 78: {
		std::string out = read_file(shot_dir + "/capture-chord.out")
			.value_or("");
		if (out.empty() && retries++ < 100) {
			return -100;
		}
		check(out.find("\"keys\":\"Super+K\"") != std::string::npos,
			"the capture reported the chord pressed");
		check(!file_exists(shot_dir + "/terminal-marker"),
			"the recorded chord ran nothing");
		reap_client();
		// The same chord, unrecorded, is the terminal again.
		key(KEY_LEFTMETA, true);
		key(KEY_K, true);
		key(KEY_K, false);
		key(KEY_LEFTMETA, false);
		return 300;
	}
	case 79: {
		if (!file_exists(shot_dir + "/terminal-marker") && retries++ < 100) {
			return -100;
		}
		check(file_exists(shot_dir + "/terminal-marker"),
			"the same chord, unrecorded, ran the terminal");
		// A plain key reaches the terminal's client: foot reads into the
		// marker. Pressed until it lands, while the new terminal takes the
		// focus.
		key(KEY_X, true);
		key(KEY_X, false);
		// The Super tap, given to the launcher through the socket.
		raw_pid = fork_ipc_client(
			"{\"id\":8,\"method\":\"settings.set\",\"params\":"
			"{\"action\":\"launcher\",\"keys\":[\"Super+Space\",\"Super\"]}}\n",
			"set-tap.out", 3);
		return 400;
	}
	case 80: {
		std::string typed = read_file(shot_dir + "/terminal-marker")
			.value_or("");
		if (typed.find('x') == std::string::npos && retries++ < 100) {
			// The terminal may still be taking the focus, and its line
			// discipline holds the key until Enter: press x, then Enter.
			key(KEY_X, true);
			key(KEY_X, false);
			key(KEY_ENTER, true);
			key(KEY_ENTER, false);
			return -100;
		}
		check(typed.find('x') != std::string::npos,
			"a plain key reached the terminal's client");
		bool tap_given = holds_chord("launcher", {settings::mod_logo, 0});
		if (!tap_given && retries++ < 100) {
			return -100;
		}
		// pan-back holds the double tap by default, so the single tap runs
		// once the double-tap interval has passed without a second.
		key(KEY_LEFTMETA, true);
		key(KEY_LEFTMETA, false);
		check(!server->launcher.is_open(),
			"the Super tap waits out the double-tap interval before it runs");
		step = 200;
		return -(static_cast<int>(config::double_tap_ms) + 200);
	}
	case 200: {
		check(server->launcher.is_open(),
			"the Super tap, given to the launcher, opened it after the interval");
		key(KEY_LEFTMETA, true);
		key(KEY_LEFTMETA, false);
		return static_cast<int>(config::double_tap_ms) + 200;
	}
	case 201: { // two taps within the interval: the double tap, not two singles
		check(!server->launcher.is_open(), "a second tap later closed it again");
		shot("tap-launcher");
		key(KEY_LEFTMETA, true);
		key(KEY_LEFTMETA, false);
		key(KEY_LEFTMETA, true);
		key(KEY_LEFTMETA, false);
		return static_cast<int>(config::double_tap_ms) + 200;
	}
	case 202: {
		check(!server->launcher.is_open(),
			"a double tap ran the double tap's action, neither single tap");
		// The settings record a double tap by tapping twice.
		raw_pid = fork_ipc_client(
			"{\"id\":12,\"method\":\"shortcut.capture\"}\n",
			"capture-double.out", 24);
		return 400;
	}
	case 203:
		key(KEY_LEFTMETA, true);
		key(KEY_LEFTMETA, false);
		key(KEY_LEFTMETA, true);
		key(KEY_LEFTMETA, false);
		return 400;
	case 204: {
		std::string out = read_file(shot_dir + "/capture-double.out").value_or("");
		if (out.empty() && retries++ < 100) {
			return -100;
		}
		retries = 0;
		check(out.find("\"keys\":\"Super Super\"") != std::string::npos,
			std::format("the capture recorded two taps as the double tap ({})", out));
		reap_client();
		raw_pid = fork_ipc_client(
			"{\"id\":13,\"method\":\"shortcut.capture\"}\n",
			"capture-single.out", 24);
		return 400;
	}
	case 205:
		key(KEY_LEFTMETA, true);
		key(KEY_LEFTMETA, false);
		return static_cast<int>(config::double_tap_ms) + 300;
	case 206: {
		std::string out = read_file(shot_dir + "/capture-single.out").value_or("");
		if (out.empty() && retries++ < 100) {
			return -100;
		}
		retries = 0;
		check(out.find("\"keys\":\"Super\"") != std::string::npos,
			std::format("one tap, the interval passed, was recorded as the tap ({})", out));
		reap_client();
		step = 81;
		return -300;
	}
	case 81: { // shortcut.cancel: a capture pending on one connection
		reap_client();
		// The terminal the chord spawns after the cancel recreates the
		// marker: a fresh one makes "it ran" observable.
		unlink((shot_dir + "/terminal-marker").c_str());
		raw_pid = fork_ipc_client(
			"{\"id\":9,\"method\":\"shortcut.capture\"}\n",
			"cancel-pending.out", 12);
		return 1000;
	}
	case 82: { // still unanswered; the cancel comes from a second connection
		std::string pending = read_file(shot_dir + "/cancel-pending.out")
			.value_or("");
		check(pending.empty(),
			"a shortcut.capture stays unanswered until it is finished or cancelled");
		cancel_pid = fork_ipc_client(
			"{\"id\":10,\"method\":\"shortcut.cancel\"}\n",
			"cancel-answer.out", 3);
		return 1000;
	}
	case 83: {
		std::string pending = read_file(shot_dir + "/cancel-pending.out")
			.value_or("");
		std::string answer = read_file(shot_dir + "/cancel-answer.out")
			.value_or("");
		if ((pending.empty() || answer.empty()) && retries++ < 100) {
			return -100;
		}
		check(pending.find("{\"id\":9,\"result\":{\"keys\":null}}") != std::string::npos,
			"the pending capture was answered {\"keys\": null}");
		check(answer.find("{\"id\":10,\"result\":true}") != std::string::npos,
			"shortcut.cancel answered true");
		reap_client();
		reap_client(cancel_pid);
		// The capture is over: the chord the file binds runs its action.
		key(KEY_LEFTMETA, true);
		key(KEY_K, true);
		key(KEY_K, false);
		key(KEY_LEFTMETA, false);
		return 300;
	}
	case 84: {
		if (!file_exists(shot_dir + "/terminal-marker") && retries++ < 100) {
			return -100;
		}
		check(file_exists(shot_dir + "/terminal-marker"),
			"after the cancel, a bound chord ran its action (Super+K opened the terminal)");
		return 300;
	}
	case 85: {
		std::string typed = read_file(shot_dir + "/terminal-marker")
			.value_or("");
		if (typed.find('z') == std::string::npos && retries++ < 100) {
			// As for the first plain key: the terminal may still be taking
			// the focus, and its line discipline holds the key until Enter.
			key(KEY_Z, true);
			key(KEY_Z, false);
			key(KEY_ENTER, true);
			key(KEY_ENTER, false);
			return -100;
		}
		check(typed.find('z') != std::string::npos,
			"after the cancel, a plain key reached the focused client");
		// With nothing to cancel the method is still just true.
		cancel_pid = fork_ipc_client(
			"{\"id\":11,\"method\":\"shortcut.cancel\"}\n",
			"cancel-idle.out", 3);
		return 1000;
	}
	case 86: {
		std::string idle = read_file(shot_dir + "/cancel-idle.out")
			.value_or("");
		if (idle.empty() && retries++ < 100) {
			return -100;
		}
		check(idle.find("{\"id\":11,\"result\":true}") != std::string::npos,
			"shortcut.cancel with no capture under way answered true");
		reap_client(cancel_pid);
		return 300;
	}
	// A button held on a window keeps the pointer on it past its edge, as
	// Wayland's implicit grab does (docs/intents/input/keyboard-and-pointer.md).
	case 87: {
		View *v = server->views.empty() ? nullptr : server->views.front();
		check(v != nullptr, "a window to hold a button on");
		if (v == nullptr) {
			return 100;
		}
		held_view = v;
		wlr_box content = v->content_box();
		Point inside = server->outputs.front()->canvas_to_local(
			content.x + content.width / 2.0, content.y + content.height / 2.0);
		move_to(inside.x, inside.y);
		button(BTN_LEFT, true);
		return 200;
	}
	case 88: {
		wlr_box frame = held_view->frame_box();
		Output *o = server->outputs.front().get();
		Point right = o->canvas_to_local(frame.x + frame.width + 40, frame.y + frame.height / 2.0);
		Point left = o->canvas_to_local(frame.x - 40, frame.y + frame.height / 2.0);
		Point outside = right.x < o->width - 10 ? right : left;
		for (int i = 1; i <= 10; i++) {
			move_to(outside.x, outside.y - 10 + i); // the press travels off the window
		}
		check(server->seat->pointer_state.focused_surface == held_view->surface(),
			"a button held on a window keeps the pointer on it past its edge");
		button(BTN_LEFT, false);
		return 200;
	}
	case 89: {
		check(server->seat->pointer_state.focused_surface != held_view->surface(),
			"its release gives the pointer to what is under it");
		return 300;
	}
	// Super+C and Super+V send the app with the keyboard the clipboard
	// chords (docs/intents/input/keyboard-and-pointer.md); an X11 client
	// through Xwayland proves them, the same as a wayland client takes them.
	case 90:
		check(std::getenv("DISPLAY") != nullptr,
			"the session names an X display for X11 clients");
		check(start_x11_client(), "the scenario's X11 client started");
		return 1500;
	case 91: { // the client's window is up; a click gives it the keyboard
		View *v = x11_view();
		if (v == nullptr && retries++ < 200) {
			return -100;
		}
		check(v != nullptr, "the X11 window mapped as a view");
		if (v != nullptr) {
			wlr_box frame = v->frame_box();
			Point inside = out()->canvas_to_local(frame.x + frame.width / 2.0,
				frame.y + config::titlebar_height + 20.0);
			click(inside.x, inside.y, BTN_LEFT);
		}
		return 400;
	}
	case 92: { // the shortcuts: each sends the client the app's own chord
		View *v = x11_view();
		check(v != nullptr && server->focused_view == v,
			"the X11 window holds the keyboard");
		chord(KEY_C, SUPER);
		chord(KEY_V, SUPER);
		return 500;
	}
	case 93: {
		std::string_view said = x11_said();
		if ((said.find("KEY 118 4") == std::string_view::npos
				|| said.find("KEY 118 1") == std::string_view::npos)
				&& retries++ < 100) {
			return -100;
		}
		check(said.find("KEY 118 4") != std::string_view::npos,
			"Super+C sent the X11 client Ctrl+Insert (keycode 118, state 4)");
		check(said.find("KEY 118 1") != std::string_view::npos,
			"Super+V sent the X11 client Shift+Insert (keycode 118, state 1)");
		reap_x11_client();
		return 300;
	}
	default:
		finished = true;
		std::println("{}: {} failure(s)", failures ? "FAILED" : "ALL PASSED", failures);
		wl_display_terminate(server->display);
		return -1;
	}
}

int handle_timer() {
	if (grim_pid > 0) {
		if (waitpid(grim_pid, nullptr, WNOHANG) == 0) {
			wl_event_source_timer_update(timer, 30);
			return 0;
		}
		grim_pid = -1;
	}
	t += 1000; // real time passes between steps (double-click detection)
	int delay = run_step();
	if (delay < -1) {
		wl_event_source_timer_update(timer, -delay);
		return 0;
	}
	if (delay == -1) {
		return 0;
	}
	step++;
	retries = 0;
	wl_event_source_timer_update(timer, delay > 0 ? delay : 1);
	return 0;
}

} // namespace

int main() {
	wlr_log_init(WLR_ERROR, nullptr);
	const char *env = std::getenv("CANVAS_TMP");
	std::string root = env && *env ? env : "tmp"; // meson passes the repo's tmp/ via env
	shot_dir = std::format("{}/scenarios/input", root);
	mkdir_p(shot_dir);
	// The terminal action runs this run's script: foot reading stdin into
	// the marker, so a chord that spawns it — and keys that reach it — are
	// observable.
	std::string terminal_sh = std::format("{}/terminal.sh", shot_dir);
	if (FILE *f = std::fopen(terminal_sh.c_str(), "w"); f != nullptr) {
		std::fprintf(f, "#!/bin/sh\nexec foot sh -c 'cat > %s/terminal-marker'\n",
			shot_dir.c_str());
		std::fclose(f);
		chmod(terminal_sh.c_str(), 0755);
	}
	setenv("TERMINAL", terminal_sh.c_str(), 1);
	// Before the server starts: canvas.conf lives in this run's scratch,
	// never in the user's real ~/.config — and every run sees it written
	// fresh, as the first run does.
	canvas_conf = std::format("{}/scenarios/input/xdg-config"
		"/wayland-canvas/canvas.conf", root);
	unlink(canvas_conf.c_str());
	setenv("XDG_CONFIG_HOME",
		std::format("{}/scenarios/input/xdg-config", root).c_str(), 1);
	setenv("WLR_BACKENDS", "headless", 1);
	setenv("WLR_RENDERER", "gles2", 1);
	server.emplace();
	Options options;
	options.start_bar = false; // the suites never spawn the bar
	if (!server->init(options) || !server->start()) {
		server.reset();
		return 2;
	}
	timer = wl_event_loop_add_timer(server->loop, [](void *data) { return handle_timer(); },
		nullptr);
	wl_event_source_timer_update(timer, 200);
	wl_display_run(server->display);
	// A compositor that stops by itself (its own shutdown, a lost backend)
	// ends the run early: without this the scenario would still exit 0.
	if (!finished) {
		check(false, std::format("the run ended at step {}, before its last step", step));
	}
	wl_event_source_remove(timer);
	wlr_pointer_finish(&ptr);
	wlr_keyboard_finish(&kbd);
	server.reset();
	return failures != 0;
}
