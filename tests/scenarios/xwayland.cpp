// End-to-end X11 scenario: X11 clients of the scenarios' own
// (tests/clients/x11-client.c) through Xwayland, which starts with them and
// not before. The window is a view like any other — a title bar, a place
// on the canvas, a click and a key reaching the client, maximize from the
// title bar, and the × closing it through WM_DELETE_WINDOW. X's own picture
// follows the canvas: a window opened over another takes its clicks, the
// one under it takes clicks where it shows and once raised, a minimized one
// takes none, and windows past X's screen or larger than it at 25 % take
// clicks at their own points; a click after a jump, and a press in the same
// instant as one, land where they were aimed. The window resizes (by
// Super+right-drag, by its frame's left edge, by its own
// _NET_WM_MOVERESIZE request), the session locks and unlocks over it, and
// Super+Shift+4 shoots it, the compositor answering `msg state` after each.
// A modal dialog whose map request finds Xwayland stopped leaves the
// compositor answering `msg state` within 100 ms, and is laid out as a modal
// one once X answers.
// Writes only under $CANVAS_TMP, with canvas.conf in its own scratch config
// dir.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <linux/input-event-codes.h>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <variant>
#include <vector>
#include "core/config.h"
#include "core/process.h"
#include "core/server.h"
#include "harness/check.h"
#include "harness/child.h"
#include "harness/control.h"
#include "harness/devices.h"
#include "harness/files.h"
#include "harness/image.h"
#include "harness/layout.h"
#include "harness/run.h"
#include "platform/output.h"
#include "windows/view.h"
#include "windows/view_x11.h"

using namespace harness;

namespace {

// Most steps need one client; the stacking steps a second beside it.
Child client, other;
Child locker;
Camera home{}; // the camera the run starts at
Point main_spot{}; // where the raised main window's click is aimed, its own point
std::size_t desktops_before = 0; // the desktops before a fullscreen step
wlr_box resized_from{}; // the X11 window's content box before the resize drag
Point drag_from{}; // where the resize drag's button went down
int pastes_asked = 0; // the clipboard asks of the X11-to-wayland paste, one file each
std::string pictures_dir; // the scratch Pictures: every shot lands here

constexpr int client_w = 400, client_h = 300;
constexpr uint32_t x11_colour = 0xc04040; // x11-client's default window colour

// Starts `child` at the scenario's size; later arguments override it.
bool start_x11(Child &child, std::vector<std::string> args = {}) {
	args.insert(args.begin(), std::format("--size={}x{}", client_w, client_h));
	return child.start_client("CANVAS_X11_CLIENT", args);
}

// A `msg state` round trip on the compositor's own control socket, run
// through its own spawn so this process never blocks; the answer is read a
// step later.
void ask_state() {
	run_msg("state", "", "state");
}

// The last ask's answer: the compositor is alive and says what it holds.
void check_alive(std::string_view after) {
	check(command_code("state") == 0 && command_out("state").contains("\"windows\""),
		std::format("the compositor answers `msg state` after {}", after));
}

// Xwayland's pid: the process of that name serving this compositor's X
// display. wlroots starts it through a double fork and keeps the pid of the
// parent that exits.
pid_t xwayland_pid() {
	std::string_view display = server->xwm.wlr()->display_name;
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator("/proc", ec)) {
		std::string name = entry.path().filename().string();
		pid_t pid = 0;
		const char *stop = name.data() + name.size();
		auto [parsed_to, parsed] = std::from_chars(name.data(), stop, pid);
		if (parsed != std::errc{} || parsed_to != stop) {
			continue;
		}
		std::string args = read_text(entry.path().string() + "/cmdline");
		std::vector<std::string_view> argv;
		for (std::size_t at = 0; at < args.size();) {
			std::size_t end = std::min(args.find('\0', at), args.size());
			argv.push_back(std::string_view(args).substr(at, end - at));
			at = end + 1;
		}
		if (!argv.empty() && std::filesystem::path(argv[0]).filename() == "Xwayland"
				&& std::ranges::contains(argv, display)) {
			return pid;
		}
	}
	return -1;
}

long elapsed_ms(const timespec &since) {
	timespec now{};
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - since.tv_sec) * 1000 + (now.tv_nsec - since.tv_nsec) / 1000000;
}

// Milliseconds one `state` request takes to be answered on the control
// socket; -1 when no answer comes within `limit_ms`.
long state_round_trip_ms(long limit_ms) {
	const char *path = std::getenv("WAYLAND_CANVAS_SOCKET");
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	if (path == nullptr || std::strlen(path) >= sizeof addr.sun_path) {
		return -1;
	}
	std::strcpy(addr.sun_path, path);
	timespec start{};
	clock_gettime(CLOCK_MONOTONIC, &start);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	constexpr std::string_view request = "{\"id\":1,\"method\":\"state\"}\n";
	if (fd < 0 || connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0
			|| write(fd, request.data(), request.size())
				!= static_cast<ssize_t>(request.size())) {
		if (fd >= 0) {
			close(fd);
		}
		return -1;
	}
	std::string answer;
	char buf[4096];
	while (!answer.contains('\n')) {
		pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
		long left = limit_ms - elapsed_ms(start);
		ssize_t got = 0;
		if (left <= 0 || poll(&pfd, 1, static_cast<int>(left)) <= 0
				|| (got = read(fd, buf, sizeof buf)) <= 0) {
			close(fd);
			return -1;
		}
		answer.append(buf, static_cast<std::size_t>(got));
	}
	close(fd);
	return answer.contains("\"windows\"") ? elapsed_ms(start) : -1;
}

// ---- Xwayland stopped at a window's map request ----

// A stall: Xwayland stopped, by this process, at the map request of the next
// X11 window the compositor is told of, and a child that times the
// compositor's answer to `state` while X stays stopped. A child, because the
// compositor runs in this process and a stalled loop cannot ask itself.
struct Stall {
	wl_listener new_surface{}, map_request{}, destroy{};
	pid_t xwayland = -1, probe = -1;
	std::string result;
} stall;

void unlisten(wl_listener &listener) {
	if (listener.link.next != nullptr) {
		wl_list_remove(&listener.link);
	}
}

// The probe: leaves the milliseconds the compositor took to answer in
// stall.result (-1: no answer within a second), then keeps Xwayland stopped
// until the scenario has looked at what the compositor did meanwhile. When
// the compositor gave no answer it waits on X and cannot go on, so X is
// continued at once; it is also continued when the scenario is gone.
[[noreturn]] void run_stall_probe(pid_t scenario) {
	long ms = state_round_trip_ms(1000);
	write_file(stall.result + ".part", std::to_string(ms));
	std::rename((stall.result + ".part").c_str(), stall.result.c_str());
	for (int waited = 0; ms >= 0 && waited < 10000 && getppid() == scenario; waited += 10) {
		usleep(10000);
	}
	kill(stall.xwayland, SIGCONT);
	_exit(0);
}

// Runs ahead of the compositor's own listener of the same signal, so that X
// is stopped before the compositor asks it anything: one that came after
// would find the ask already answered.
void stall_at_map_request(wl_listener *, void *) {
	unlisten(stall.map_request);
	unlisten(stall.destroy);
	kill(stall.xwayland, SIGSTOP);
	pid_t scenario = getpid();
	stall.probe = fork();
	if (stall.probe == 0) {
		run_stall_probe(scenario);
	}
}

void stall_new_surface(wl_listener *, void *data) {
	unlisten(stall.new_surface);
	auto *surface = static_cast<wlr_xwayland_surface *>(data);
	stall.map_request.notify = stall_at_map_request;
	wl_list_insert(&surface->events.map_request.listener_list, &stall.map_request.link);
	stall.destroy.notify = [](wl_listener *, void *) {
		unlisten(stall.map_request);
		unlisten(stall.destroy);
	};
	wl_signal_add(&surface->events.destroy, &stall.destroy);
}

// Arms the stall for the next X11 window; false when Xwayland's process is
// not found.
bool arm_stall() {
	stall.xwayland = xwayland_pid();
	stall.result = std::format("{}/stall-state.ms", scratch());
	if (stall.xwayland <= 0) {
		return false;
	}
	stall.new_surface.notify = stall_new_surface;
	wl_signal_add(&server->xwm.wlr()->events.new_surface, &stall.new_surface);
	return true;
}

// Nothing left of the stall, whatever it got to: the probe gone, and Xwayland
// running again, as only a SIGCONT ends a stop.
void reap_stall() {
	unlisten(stall.new_surface);
	unlisten(stall.map_request);
	unlisten(stall.destroy);
	if (stall.probe > 0) {
		kill(stall.probe, SIGKILL);
		waitpid(stall.probe, nullptr, 0);
		stall.probe = -1;
	}
	if (stall.xwayland > 0) {
		kill(stall.xwayland, SIGCONT);
		stall.xwayland = -1;
	}
}

// The newest file the shot hotkey saved, "" when there is none.
std::string newest_shot() {
	std::string newest;
	std::filesystem::file_time_type newest_time{};
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(
			pictures_dir + "/Screenshots", ec)) {
		if (!entry.path().filename().string().starts_with("Screenshot")) {
			continue;
		}
		std::filesystem::file_time_type time = entry.last_write_time(ec);
		if (newest.empty() || time > newest_time) {
			newest = entry.path().string();
			newest_time = time;
		}
	}
	return newest;
}

// A PNG's size and the share of its pixels that are the colour `rgb`.
struct PngLook {
	int width = 0, height = 0;
	double share = -1;
};

PngLook look_at_png(const std::string &path, uint32_t rgb) {
	std::optional<Image> image = Image::read(path);
	if (!image.has_value()) {
		return {};
	}
	return {image->width(), image->height(), image->share_of(rgb)};
}

// Whether the interactive resize grab is on.
bool resizing() {
	return std::holds_alternative<PointerGrab::Resize>(server->input.grab.state());
}

// Whether `now` still ends where `from` did, at the right and at the bottom,
// within a pixel: the edges a drag by the left or top edge leaves alone.
bool far_edges_held(const wlr_box &from, const wlr_box &now) {
	return std::abs(now.x + now.width - (from.x + from.width)) <= 1
		&& std::abs(now.y + now.height - (from.y + from.height)) <= 1;
}

View *x11_view() {
	for (View *view : server->views) {
		if (view->mapped && view->as_x11() != nullptr) {
			return view;
		}
	}
	return nullptr;
}

// A point of the window's body, clear of its title bar.
Point body_point(const View &view) {
	wlr_box frame = frame_on_screen(view);
	return {frame.x + frame.width / 2.0, frame.y + config::titlebar_height + 20.0};
}

void click_into(const View &view) {
	Point at = body_point(view);
	click(at.x, at.y);
}

// Every point a client said a button arrived at, in order.
std::vector<Point> clicks_said(Child &c) {
	std::vector<Point> out;
	std::string text{c.said()};
	for (std::size_t at = text.find("BUTTON "); at != std::string::npos;
			at = text.find("BUTTON ", at + 1)) {
		int button = 0, x = 0, y = 0;
		if (std::sscanf(text.c_str() + at, "BUTTON %d %d,%d", &button, &x, &y) == 3) {
			out.push_back(Point{static_cast<double>(x), static_cast<double>(y)});
		}
	}
	return out;
}

std::string said_clicks(Child &c) {
	std::string out;
	for (Point p : clicks_said(c)) {
		out += std::format("{}{:.0f},{:.0f}", out.empty() ? "" : " ", p.x, p.y);
	}
	return out.empty() ? "none" : out;
}

// A click arrives on whole pixels: within two of where it was aimed.
bool close_to(Point got, Point want) {
	return std::abs(got.x - want.x) <= 2 && std::abs(got.y - want.y) <= 2;
}

// Whether the screen's camera is going to rest on `desktop`.
bool camera_on(const Desktop *desktop) {
	if (desktop == nullptr) {
		return false;
	}
	Camera to = output().camera.destination();
	Point centre = desktop->centre();
	return std::abs(to.cx - centre.x) < 1 && std::abs(to.cy - centre.y) < 1;
}

// Whether `over`'s frame is centred on `under`'s, within a pixel.
bool centred_on(const View &over, const View &under) {
	Rect a = under.frame_rect(), b = over.frame_rect();
	return std::abs(a.cx() - b.cx()) <= 1 && std::abs(a.cy() - b.cy()) <= 1;
}

// One line of what a client said, from `word` to its end.
std::string_view line_from(std::string_view said, std::string_view word) {
	std::size_t at = said.find(word);
	if (at == std::string_view::npos) {
		return {};
	}
	return said.substr(at, said.find('\n', at) - at);
}

// Whether the drag of a resize left the window larger by `dx` (and `dy`, when
// given) screen pixels at the zoom it began at, within a pixel.
bool grown_by(const View *view, int dx, std::optional<int> dy = std::nullopt) {
	if (view == nullptr) {
		return false;
	}
	double zoom = output().camera.drawn().zoom;
	int want_w = resized_from.width + static_cast<int>(std::round(dx / zoom));
	bool wide = std::abs(view->content_box().width - want_w) <= 1;
	if (!dy.has_value()) {
		return wide;
	}
	int want_h = resized_from.height + static_cast<int>(std::round(*dy / zoom));
	return wide && std::abs(view->content_box().height - want_h) <= 1;
}

// The window menu's surface and the window it belongs to.
std::pair<View *, View *> menu_and_window() {
	View *menu = nullptr, *window = nullptr;
	for (View *view : server->views) {
		if (!view->mapped || view->as_x11() == nullptr) {
			continue;
		}
		(view->client_placed() ? menu : window) = view;
	}
	return {menu, window};
}

// ---- the window is a view like any other ----

Result client_starts() { // no X11 client yet: Xwayland has nothing to show
	attach_pointer();
	attach_keyboard();
	home = output().camera.drawn();
	check(std::getenv("DISPLAY") != nullptr, "the session names an X display for X11 clients");
	check(x11_view() == nullptr, "no X11 window before a client runs");
	check(start_x11(client), "the scenario's X11 client started");
	return after(1500);
}

Result window_maps() {
	if (auto wait = wait_for(x11_view() != nullptr, {.budget_ms = 20'000})) {
		return *wait;
	}
	View *view = x11_view();
	check(view != nullptr, "the X11 window mapped as a view");
	if (view == nullptr) {
		return go_to("end", 100);
	}
	check(client.said().contains("MAPPED"), "the client says it mapped its window");
	check(view->shell_title() != nullptr
			&& std::string_view{view->shell_title()} == "canvas-x11",
		"the view carries the X11 window's title");
	check(view->deco.titlebar() == config::titlebar_height,
		"it is drawn with a title bar, like any other window");
	check(server->canvas.desktop_of(*view) != nullptr
			|| view->frame_rect().intersects(output().viewport()),
		"it opened where the user is looking");
	return after(600);
}

// A click and a key reach the client.
Result click_and_key() {
	click_into(*x11_view());
	tap(KEY_A);
	return after(800);
}

Result click_and_key_arrive() {
	if (auto wait = wait_for(client.said().contains("BUTTON") && client.said().contains("KEY"),
			{.budget_ms = 5000})) {
		return *wait;
	}
	check(client.said().contains("BUTTON"), "a click reached the X11 client");
	check(client.said().contains("KEY"), "a key reached the X11 client");
	return after(300);
}

// Maximize from the title bar.
Result maximize_click() {
	Point maximize = deco_point(*x11_view(), DecoPart::maximize);
	click(maximize.x, maximize.y);
	return after(1200);
}

Result window_maximized() {
	if (auto wait = wait_for(x11_view() == nullptr || x11_view()->mode == WindowMode::maximized,
			{.budget_ms = 5000})) {
		return *wait;
	}
	View *view = x11_view();
	check(view != nullptr && view->mode == WindowMode::maximized,
		"the title bar's maximize button maximized the X11 window");
	check(view != nullptr && client.said().contains(std::format("CONFIGURED {}x{}",
			view->as_x11()->wlr->width, view->as_x11()->wlr->height)),
		"the client was told its new size");
	return after(600);
}

// The × closes it through WM_DELETE_WINDOW.
Result close_click() {
	Point close = deco_point(*x11_view(), DecoPart::close);
	click(close.x, close.y);
	return after(1200);
}

Result client_ends() {
	if (auto wait = wait_for(!client.running())) {
		return *wait;
	}
	check(client.said().contains("CLOSING"), "the close button asked the client to close itself");
	check(!client.running(), "the X11 client ended");
	return after(600);
}

Result popup_client_starts() {
	check(x11_view() == nullptr, "its view went with it");
	client.kill();
	// A menu of the client's own: an override-redirect window.
	check(start_x11(client, {"--popup"}), "an X11 client with a menu started");
	return after(1500);
}

Result popup_maps() {
	if (auto wait = wait_for(client.said().contains("POPUP"), {.budget_ms = 20'000})) {
		return *wait;
	}
	check(client.said().contains("POPUP"), "the client mapped its menu");
	return after(800);
}

// The menu is where the client put it, and undecorated.
Result popup_is_placed() {
	if (auto wait = wait_for(menu_and_window().first != nullptr, {.budget_ms = 5000})) {
		return *wait;
	}
	auto [menu, window] = menu_and_window();
	check(menu != nullptr, "the menu is on the canvas as its own surface");
	check(window != nullptr, "beside the window it belongs to");
	if (menu != nullptr) {
		check(menu->deco.titlebar() == 0, "an override-redirect window wears no title bar");
		check(menu->x == server->xwm.canvas_x(120) && menu->y == server->xwm.canvas_y(140),
			std::format("it sits where its client put it, through X's offset ({},{})",
				menu->x, menu->y));
		check(server->focused_view != menu, "and it took no focus from the window");
	}
	client.kill();
	return after(800);
}

// ---- the clipboard: X11 to wayland ----

Result clipboard_owner_starts() {
	check(start_x11(client, {"--offer=from-x11"}), "an X11 client owning the clipboard started");
	return after(1500);
}

Result clipboard_is_asked() {
	if (auto wait = wait_for(client.said().contains("OFFERED"), {.budget_ms = 20'000})) {
		return *wait;
	}
	check(client.said().contains("OFFERED"), "the client owns the X11 clipboard");
	// X11 hands its selection over for the window that holds the focus: the
	// client's own window is clicked first.
	if (View *view = x11_view()) {
		click_into(*view);
	}
	// Each ask writes its own file: a file left by an ask that found nothing
	// must never be read as this one's answer.
	pastes_asked = 0;
	run_command("wl-paste -n", "paste-0");
	return after(2000);
}

Result x11_clipboard_pastes() {
	std::string pasted = command_out(std::format("paste-{}", pastes_asked));
	if (auto wait = wait_for(pasted.starts_with("from-x11"),
			{.poll_ms = 500, .budget_ms = 60'000})) {
		// The offer crosses the bridge once the compositor holds the X11
		// selection, which follows the focus: the window is clicked again and
		// the clipboard asked again.
		if (View *view = x11_view()) {
			click_into(*view);
		}
		run_command("wl-paste -n", std::format("paste-{}", ++pastes_asked));
		return *wait;
	}
	check(pasted.starts_with("from-x11"),
		std::format("what an X11 client copied pastes in a wayland client ('{}')", pasted));
	client.kill();
	return after(500);
}

// ---- and wayland to X11 ----

Result wayland_copies() {
	spawn("wl-copy from-wayland");
	return after(1000);
}

Result paste_client_starts() {
	check(start_x11(client, {"--paste"}), "an X11 client reading the clipboard started");
	return after(2000);
}

Result wayland_clipboard_pastes() {
	if (auto wait = wait_for(client.said().contains("PASTED from-wayland"),
			{.poll_ms = 200, .budget_ms = 40'000})) {
		// The selection crosses to the window that holds the focus: the
		// client's own window is clicked as it comes up.
		if (View *view = x11_view()) {
			click_into(*view);
		}
		return *wait;
	}
	check(client.said().contains("PASTED from-wayland"),
		std::format("what a wayland client copied pastes in an X11 client ({})",
			line_from(client.said(), "PASTED")));
	client.kill();
	return after(300);
}

// ---- a machine whose Xwayland is missing ----

// No X11, and the compositor runs all the same: a compositor of its own,
// headless and on its own display, so this run's X server is untouched.
Result compositor_without_xwayland_starts() {
	check(server->xwm.wlr() != nullptr, "this run has an X server, its binary being there");
	const char *bin = std::getenv("WAYLAND_CANVAS_BIN");
	run_command(std::format("env WLR_BACKENDS=headless WLR_RENDERER=gles2 "
			"WLR_XWAYLAND='{0}/not-an-xwayland' WAYLAND_DISPLAY= "
			"XDG_CONFIG_HOME='{0}/xdg-config' '{1}' -B "
			"-s \"sleep 2; '{1}' msg quit\"", scratch(), bin),
		"xwayland-missing");
	return after(2000);
}

Result compositor_without_xwayland_ends() {
	if (auto wait = wait_for(command_done("xwayland-missing"),
			{.poll_ms = 300, .budget_ms = 30'000})) {
		return *wait;
	}
	std::string log = command_out("xwayland-missing") + command_err("xwayland-missing");
	check(command_done("xwayland-missing"),
		"a compositor whose Xwayland binary is missing starts and stops");
	check(log.contains("Running wayland-canvas on WAYLAND_DISPLAY="),
		"it served its own display while it ran");
	return after(300);
}

// ---- X's picture follows the canvas: a window opened over another ----

Result main_window_starts() {
	client.kill();
	check(start_x11(client, {"--size=600x400", "--title=x11-main"}), "a main X11 window started");
	return after(1500);
}

Result dialog_window_starts() {
	if (auto wait = wait_for(titled("x11-main") != nullptr, {.budget_ms = 20'000})) {
		return *wait;
	}
	check(start_x11(other, {"--size=300x200", "--title=x11-dialog"}),
		"a dialog-sized X11 window started after it");
	return after(1500);
}

Result both_are_mapped() {
	if (auto wait = wait_for(titled("x11-dialog") != nullptr, {.budget_ms = 20'000})) {
		return *wait;
	}
	View *main = titled("x11-main"), *dialog = titled("x11-dialog");
	check(main != nullptr && dialog != nullptr && server->views.front() == dialog,
		"both mapped, the second on top");
	if (main == nullptr || dialog == nullptr) {
		return go_to("end", 100);
	}
	return after(600); // a frame tells X the stacking and the places
}

Result click_on_the_dialog() {
	Point p = on_screen(*titled("x11-dialog"), 150, 100);
	click(p.x, p.y);
	tap(KEY_A);
	return after(800);
}

Result dialog_got_the_click() {
	std::vector<Point> top = clicks_said(other);
	check(top.size() == 1 && close_to(top.back(), {150, 100}),
		std::format("a click on the window opened over another reaches it ({})",
			said_clicks(other)));
	check(clicks_said(client).empty(), "and not the window under it");
	check(other.said().contains("KEY"), "its keys reach it too");
	Point p = on_screen(*titled("x11-main"), 30, 30);
	click(p.x, p.y);
	return after(800);
}

Result main_got_the_click() {
	std::vector<Point> under = clicks_said(client);
	check(under.size() == 1 && close_to(under.back(), {30, 30}),
		std::format("the window under it takes clicks where it shows ({})", said_clicks(client)));
	// Raised by that click, it covers the other where both are.
	View *main = titled("x11-main"), *dialog = titled("x11-dialog");
	main_spot = Point{dialog->x + 150.0 - main->x, dialog->y + 100.0 - main->y};
	Point p = on_screen(*dialog, 150, 100);
	click(p.x, p.y);
	return after(800);
}

Result raised_main_covers_the_dialog() {
	std::vector<Point> under = clicks_said(client);
	check(under.size() == 2 && close_to(under.back(), main_spot) && clicks_said(other).size() == 1,
		std::format("raised, it takes the click where it covers the other ({})",
			said_clicks(client)));
	titled("x11-main")->set_minimized(true);
	return after(600);
}

Result click_on_the_dialog_again() {
	Point p = on_screen(*titled("x11-dialog"), 150, 100);
	click(p.x, p.y);
	return after(800);
}

Result minimized_takes_no_click() {
	std::vector<Point> top = clicks_said(other);
	check(top.size() == 2 && close_to(top.back(), {150, 100}) && clicks_said(client).size() == 2,
		std::format("a minimized window takes no click meant for the one it covered ({})",
			said_clicks(other)));
	client.kill();
	other.kill();
	// The camera looks past X's screen, which is the layout's box.
	output().camera.jump(Camera{home.cx + 3000, home.cy, 1});
	check(start_x11(client, {"--size=300x200", "--title=x11-far"}),
		"an X11 window started past X's screen");
	return after(1500);
}

// ---- past X's screen ----

Result far_window_maps() {
	if (auto wait = wait_for(titled("x11-far") != nullptr, {.budget_ms = 20'000})) {
		return *wait;
	}
	View *distant = titled("x11-far");
	check(distant != nullptr && distant->x > output().width,
		std::format("it opened where the camera looks ({})", distant != nullptr ? distant->x : 0));
	if (distant == nullptr) {
		return go_to("end", 100);
	}
	return after(600);
}

// The pointer comes in as a pointer does, then clicks.
Result pointer_comes_to_the_far_window() {
	Point p = on_screen(*titled("x11-far"), 150, 100);
	move_to(p.x - 3, p.y - 3);
	return after(300);
}

Result click_on_the_far_window() {
	Point p = on_screen(*titled("x11-far"), 150, 100);
	click(p.x, p.y);
	return after(800);
}

Result far_window_got_the_click() {
	std::vector<Point> distant = clicks_said(client);
	check(distant.size() == 1 && close_to(distant.back(), {150, 100}),
		std::format("a window past X's screen takes the click at its own point ({})",
			said_clicks(client)));
	client.kill();
	// At 25 %, a window larger than X's screen.
	output().camera.jump(Camera{home.cx, home.cy, 0.25});
	check(start_x11(client, {"--size=4000x2400", "--title=x11-big"}),
		"an X11 window larger than X's screen started");
	return after(1500);
}

Result big_window_maps() {
	if (auto wait = wait_for(titled("x11-big") != nullptr, {.budget_ms = 20'000})) {
		return *wait;
	}
	View *big = titled("x11-big");
	check(big != nullptr, "it mapped at 25 %");
	if (big == nullptr) {
		return go_to("end", 100);
	}
	return after(600);
}

Result pointer_comes_to_the_far_corner() {
	Point p = on_screen(*titled("x11-big"), 3900, 2300);
	move_to(p.x - 2, p.y - 2);
	return after(300);
}

Result click_the_far_corner() {
	Point p = on_screen(*titled("x11-big"), 3900, 2300);
	click(p.x, p.y);
	return after(300);
}

Result pointer_comes_to_the_near_corner() {
	Point p = on_screen(*titled("x11-big"), 100, 100);
	move_to(p.x + 2, p.y + 2);
	return after(300);
}

Result click_the_near_corner() {
	Point p = on_screen(*titled("x11-big"), 100, 100);
	click(p.x, p.y);
	return after(800);
}

Result big_window_got_the_corner_clicks() {
	std::vector<Point> big = clicks_said(client);
	check(big.size() == 2 && close_to(big[0], {3900, 2300}) && close_to(big[1], {100, 100}),
		std::format("at 25 %, the larger window takes clicks at its far corners ({})",
			said_clicks(client)));
	// A jump across the window, then a press with no motion between: X saw
	// the jump before its windows moved, and is told it again.
	Point p = on_screen(*titled("x11-big"), 3900, 100);
	move_to(p.x, p.y);
	return after(400);
}

Result press_after_the_jump() {
	button(BTN_LEFT, true);
	button(BTN_LEFT, false);
	return after(800);
}

Result press_after_the_jump_landed() {
	std::vector<Point> big = clicks_said(client);
	check(big.size() == 3 && close_to(big.back(), {3900, 100}),
		std::format("a click after a jump, with no motion between, lands where it was aimed ({})",
			said_clicks(client)));
	// A jump and a press in the same instant: the press waits until X has
	// taken in the moves the jump sent.
	Point p = on_screen(*titled("x11-big"), 100, 2300);
	move_to(p.x, p.y);
	button(BTN_LEFT, true);
	button(BTN_LEFT, false);
	return after(800);
}

Result jump_and_press_landed() {
	std::vector<Point> big = clicks_said(client);
	check(big.size() == 4 && close_to(big.back(), {100, 2300}),
		std::format("a jump and a press in the same instant land where they were aimed ({})",
			said_clicks(client)));
	client.kill();
	output().camera.jump(home);
	return after(300);
}

// ---- a modal dialog floats over its window on a tiled desktop ----

Result tiled_client_starts() {
	chord(KEY_T, mods::super);
	check(start_x11(client, {"--size=600x400", "--title=x11-tiled", "--dialog"}),
		"an X11 window with a modal dialog started on a tiled desktop");
	return after(1500);
}

Result tiled_window_maps() {
	if (auto wait = wait_for(titled("x11-tiled") != nullptr, {.budget_ms = 20'000})) {
		return *wait;
	}
	return after(800); // tiled by now
}

// Its first key opens the dialog.
Result tiled_dialog_key() {
	tap(KEY_D);
	return after(600);
}

Result tiled_dialog_maps() {
	if (auto wait = wait_for(titled("x11-tiled-dialog") != nullptr, {.budget_ms = 20'000})) {
		return *wait;
	}
	return after(600);
}

Result tiled_dialog_floats() {
	View *main = titled("x11-tiled"), *dialog = titled("x11-tiled-dialog");
	check(main != nullptr && main->tiled && main->board_seat.has_value(),
		"the window joined the tiled desktop's board");
	check(dialog != nullptr && !dialog->tiled && !dialog->board_seat.has_value(),
		"its modal dialog floats, off the board");
	check(main != nullptr && dialog != nullptr && centred_on(*dialog, *main),
		"centred over the window it blocks");
	// Switching tiling off and on lays the desktop out again: the modal
	// dialog still floats over its window.
	chord(KEY_T, mods::super);
	return after(600);
}

Result tiling_comes_back() {
	chord(KEY_T, mods::super);
	return after(600);
}

Result tiling_leaves_the_dialog_floating() {
	View *main = titled("x11-tiled"), *dialog = titled("x11-tiled-dialog");
	check(main != nullptr && main->tiled && main->board_seat.has_value()
			&& dialog != nullptr && !dialog->tiled && !dialog->board_seat.has_value(),
		"switched off and on, tiling leaves the modal dialog floating");
	// Going to a desktop gives the keyboard to the window last used there.
	chord(KEY_2, mods::super);
	return after(1000);
}

Result desktop_two_window_starts() {
	check(start_x11(other, {"--size=300x200", "--title=x11-two"}),
		"an X11 window started on desktop 2");
	return after(1500);
}

Result desktop_two_window_takes_the_keyboard() {
	if (auto wait = wait_for(titled("x11-two") != nullptr, {.budget_ms = 20'000})) {
		return *wait;
	}
	check(titled("x11-two") != nullptr && server->focused_view == titled("x11-two"),
		"it took the keyboard as it opened");
	chord(KEY_1, mods::super);
	return after(1000);
}

Result desktop_one_keeps_its_window() {
	check(server->focused_view != nullptr && server->focused_view == titled("x11-tiled-dialog"),
		"Super+1 gave the keyboard to the window last used on desktop 1");
	chord(KEY_2, mods::super);
	return after(1000);
}

Result desktop_two_keeps_its_window() {
	check(server->focused_view != nullptr && server->focused_view == titled("x11-two"),
		"Super+2 gave it back to the window on desktop 2");
	client.kill();
	other.kill();
	return after(300);
}

// ---- fullscreen over another window: a desktop of its own ----

Result covered_window_starts() {
	check(start_x11(other, {"--size=400x300", "--title=x11-beside"}),
		"a window to be covered started on desktop 2");
	return after(1500);
}

Result fullscreen_window_starts() {
	if (auto wait = wait_for(titled("x11-beside") != nullptr, {.budget_ms = 20'000})) {
		return *wait;
	}
	check(start_x11(client, {"--size=500x300", "--title=x11-fs"}),
		"a window to go fullscreen started beside it");
	return after(1500);
}

Result fullscreen_over_another_window() {
	if (auto wait = wait_for(titled("x11-fs") != nullptr, {.budget_ms = 20'000})) {
		return *wait;
	}
	Desktop *d2 = server->canvas.desktops.by_number(2);
	View *fs = titled("x11-fs"), *beside = titled("x11-beside");
	check(fs != nullptr && beside != nullptr && d2 != nullptr
			&& server->canvas.desktop_of(*fs) == d2
			&& server->canvas.desktop_of(*beside) == d2 && server->focused_view == fs,
		"both stand on desktop 2, the newer focused");
	desktops_before = server->canvas.desktops.size();
	chord(KEY_F, mods::super);
	return after(800);
}

Result fullscreen_has_a_desktop_of_its_own() {
	View *fs = titled("x11-fs"), *beside = titled("x11-beside");
	Desktop *d2 = server->canvas.desktops.by_number(2);
	Desktop *own = fs != nullptr ? server->canvas.desktop_of(*fs) : nullptr;
	check(fs != nullptr && fs->mode == WindowMode::fullscreen && own != nullptr && own != d2
			&& server->canvas.desktops.size() == desktops_before + 1,
		"Super+F over another window took it to a new desktop of its own");
	bool clear = own != nullptr;
	for (const Desktop &d : server->canvas.desktops.all()) {
		clear = clear && (&d == own || !own->rect().intersects(d.rect()));
	}
	check(clear, "beside the others, overlapping none");
	check(beside != nullptr && server->canvas.desktop_of(*beside) == d2,
		"the covered window stayed on desktop 2");
	check(camera_on(own) && server->focused_view == fs,
		"the screen and the keyboard went with it");
	chord(KEY_F, mods::super);
	return after(800);
}

Result fullscreen_comes_back() {
	View *fs = titled("x11-fs");
	Desktop *d2 = server->canvas.desktops.by_number(2);
	check(fs != nullptr && fs->mode != WindowMode::fullscreen
			&& server->canvas.desktop_of(*fs) == d2
			&& server->canvas.desktops.size() == desktops_before,
		"Super+F again brought it back to desktop 2, and its desktop went");
	check(camera_on(d2), "the screen came back to desktop 2 with it");
	other.kill();
	return after(600);
}

// Alone on its desktop: fullscreen in place.
Result fullscreen_alone() {
	if (auto wait = wait_for(titled("x11-beside") == nullptr)) {
		return *wait;
	}
	chord(KEY_F, mods::super);
	return after(800);
}

Result fullscreen_stays_in_place() {
	View *fs = titled("x11-fs");
	Desktop *d2 = server->canvas.desktops.by_number(2);
	check(fs != nullptr && fs->mode == WindowMode::fullscreen
			&& server->canvas.desktop_of(*fs) == d2
			&& server->canvas.desktops.size() == desktops_before,
		"alone on its desktop, it went fullscreen in place");
	chord(KEY_F, mods::super);
	return after(600);
}

Result fullscreen_client_ends() {
	client.kill();
	return after(300);
}

// ---- an interactive resize, an unlock and a window shot of an X11 window:
// each asks the window's shell for something and each is followed by a
// `msg state` round trip, which only a live compositor answers ----

Result act_window_starts() {
	other.kill();
	check(start_x11(client, {"--title=x11-act"}),
		"an X11 window to resize, lock under and shoot started");
	return after(1500);
}

// (a) Super is held, the pointer comes in over the window.
Result super_is_held_over_the_window() {
	if (auto wait = wait_for(titled("x11-act") != nullptr, {.budget_ms = 20'000})) {
		return *wait;
	}
	View *view = titled("x11-act");
	check(view != nullptr, "the window mapped");
	if (view == nullptr) {
		return go_to("end", 100);
	}
	resized_from = view->content_box();
	wlr_box frame = frame_on_screen(*view);
	// The lower right quarter: the right and bottom edges move.
	drag_from = Point{frame.x + frame.width * 0.75, frame.y + frame.height * 0.75};
	key(KEY_LEFTMETA, true);
	move_to(drag_from.x, drag_from.y);
	return after(300);
}

// The button goes down, and the grab is awaited before the pointer travels.
Result right_button_goes_down() {
	button(BTN_RIGHT, true);
	return after(300);
}

Result right_drag_begins() {
	if (auto wait = wait_for(resizing(), {.budget_ms = 5000})) {
		return *wait;
	}
	check(resizing(), "Super+right-drag over the X11 window began a resize");
	move_to(drag_from.x + 20, drag_from.y + 15);
	return after(100);
}

Result right_drag_moves_on() {
	move_to(drag_from.x + 40, drag_from.y + 30);
	return after(100);
}

Result right_drag_moves_further() {
	move_to(drag_from.x + 60, drag_from.y + 45);
	return after(100);
}

Result right_drag_arrives() {
	move_to(drag_from.x + 80, drag_from.y + 60);
	return after(300);
}

// The release ends the grab.
Result right_button_is_released() {
	button(BTN_RIGHT, false);
	ask_state();
	return after(600);
}

Result right_drag_resized() {
	View *view = titled("x11-act");
	if (auto wait = wait_for(command_done("state") && grown_by(view, 80, 60),
			{.budget_ms = 5000})) {
		return *wait;
	}
	check_alive("the resize");
	key(KEY_LEFTMETA, false);
	check(!resizing(), "the release ended the resize");
	check(grown_by(view, 80, 60),
		std::format("the window is larger by the drag ({}x{} to {}x{})",
			resized_from.width, resized_from.height,
			view != nullptr ? view->content_box().width : 0,
			view != nullptr ? view->content_box().height : 0));
	return after(300);
}

// (b) The window holds the focus.
Result act_window_is_clicked() {
	View *view = titled("x11-act");
	if (view == nullptr) {
		return go_to("end", 100);
	}
	click_into(*view);
	return after(600);
}

// ...and the session locks.
Result session_locks() {
	View *view = titled("x11-act");
	check(view != nullptr && server->focused_view == view, "a click on the window focused it");
	check(locker.start_client("CANVAS_LOCK_CLIENT"), "the scenarios' locker started");
	return after(600);
}

Result session_is_locked() {
	if (auto wait = wait_for(server->lock.locked() && locker.said().contains("LOCKED"))) {
		return *wait;
	}
	check(server->lock.locked(), "the locker took the session");
	check(locker.said().contains("LOCKED"), "the compositor confirmed the lock");
	ask_state();
	return after(600);
}

Result session_unlocks() {
	if (auto wait = wait_for(command_done("state"), {.budget_ms = 5000})) {
		return *wait;
	}
	check_alive("the lock");
	locker.signal(SIGUSR1); // unlock and destroy
	return after(600);
}

Result session_is_unlocked() {
	if (auto wait = wait_for(!server->lock.locked())) {
		return *wait;
	}
	check(!server->lock.locked(), "the locker unlocked the session");
	ask_state();
	return after(600);
}

Result keyboard_is_back_on_the_window() {
	if (auto wait = wait_for(command_done("state"), {.budget_ms = 5000})) {
		return *wait;
	}
	check_alive("the unlock");
	View *view = titled("x11-act");
	check(view != nullptr && server->seat->keyboard_state.focused_surface == view->surface(),
		"after the unlock the keyboard is on the X11 window's surface");
	locker.kill();
	return after(300);
}

// (c) Super+Shift+4, the pointer on the window.
Result shot_hotkey() {
	View *view = titled("x11-act");
	if (view == nullptr) {
		return go_to("end", 100);
	}
	Point at = body_point(*view);
	move_to(at.x, at.y);
	chord(KEY_4, mods::super_shift);
	return after(600);
}

// ...and a click on the window shoots it.
Result click_shoots_the_window() {
	check(server->capture_ui.picking(), "Super+Shift+4 opened the picker");
	click_into(*titled("x11-act"));
	ask_state();
	return after(1500);
}

Result window_shot_is_saved() {
	std::string shot = newest_shot();
	if (auto wait = wait_for(!shot.empty() && command_done("state"), {.budget_ms = 5000})) {
		return *wait;
	}
	check_alive("the window shot");
	check(!shot.empty(), "the click saved a window shot");
	View *view = titled("x11-act");
	PngLook look = look_at_png(shot, x11_colour);
	wlr_box frame = view != nullptr ? view->frame_box() : wlr_box{};
	check(view != nullptr && look.width == frame.width && look.height == frame.height,
		std::format("the shot has the window frame's size ({}x{}, frame {}x{})",
			look.width, look.height, frame.width, frame.height));
	check(look.share > 0.7, std::format("and shows the client's colour ({:.2f} of it)", look.share));
	return after(300);
}

// ---- by the frame's left edge: the window grows leftwards, the right edge
// holds ----

Result pointer_comes_to_the_left_edge() {
	View *view = titled("x11-act");
	if (view == nullptr) {
		return go_to("end", 100);
	}
	resized_from = view->content_box();
	wlr_box frame = frame_on_screen(*view);
	drag_from = Point{frame.x - config::resize_margin / 2.0, frame.y + frame.height / 2.0};
	move_to(drag_from.x, drag_from.y);
	return after(300);
}

Result left_button_goes_down() {
	button(BTN_LEFT, true);
	return after(300);
}

Result left_edge_drag_begins() {
	if (auto wait = wait_for(resizing(), {.budget_ms = 5000})) {
		return *wait;
	}
	check(resizing(), "a press on the X11 window's left edge began a resize");
	move_to(drag_from.x - 30, drag_from.y);
	return after(100);
}

Result left_edge_drag_moves_on() {
	move_to(drag_from.x - 60, drag_from.y);
	return after(100);
}

Result left_edge_drag_arrives() {
	move_to(drag_from.x - 90, drag_from.y);
	return after(300);
}

Result left_button_is_released() {
	button(BTN_LEFT, false);
	ask_state();
	return after(600);
}

Result left_edge_drag_resized() {
	View *view = titled("x11-act");
	if (auto wait = wait_for(command_done("state") && grown_by(view, 90), {.budget_ms = 5000})) {
		return *wait;
	}
	check_alive("the left-edge resize");
	check(!resizing(), "the release ended the resize");
	check(grown_by(view, 90), std::format("the window grew by the drag ({} to {})",
		resized_from.width, view != nullptr ? view->content_box().width : 0));
	check(view != nullptr && far_edges_held(resized_from, view->content_box()),
		"the right and bottom edges held after the release");
	return after(400);
}

// The client has redrawn at its new size by now.
Result left_edge_edges_held_after_commit() {
	View *view = titled("x11-act");
	check(view != nullptr && far_edges_held(resized_from, view->content_box()),
		"and after the client's next commit");
	return after(100);
}

// ---- a window that asks for a resize of its own ----

Result moveresize_client_starts() {
	client.kill();
	check(start_x11(client, {"--title=x11-ask", "--moveresize"}),
		"an X11 client that asks to be resized started");
	return after(1500);
}

Result moveresize_window_maps() {
	if (auto wait = wait_for(titled("x11-ask") != nullptr, {.budget_ms = 20'000})) {
		return *wait;
	}
	View *view = titled("x11-ask");
	check(view != nullptr, "its window mapped");
	if (view == nullptr) {
		return go_to("end", 100);
	}
	resized_from = view->content_box();
	drag_from = body_point(*view);
	move_to(drag_from.x, drag_from.y);
	return after(300);
}

// The press reaches the client, which asks for the resize.
Result press_on_the_asking_window() {
	button(BTN_LEFT, true);
	return after(300);
}

Result clients_request_begins_a_resize() {
	if (auto wait = wait_for(resizing(), {.budget_ms = 5000})) {
		return *wait;
	}
	check(client.said().contains("MOVERESIZE"),
		"the client asked the window manager to resize it");
	check(resizing(), "the client's own request began the same resize");
	move_to(drag_from.x - 40, drag_from.y - 30);
	return after(100);
}

Result asked_resize_arrives() {
	move_to(drag_from.x - 80, drag_from.y - 60);
	return after(300);
}

Result asking_button_is_released() {
	button(BTN_LEFT, false);
	ask_state();
	return after(600);
}

Result asked_resize_done() {
	View *view = titled("x11-ask");
	if (auto wait = wait_for(command_done("state") && grown_by(view, 80, 60),
			{.budget_ms = 5000})) {
		return *wait;
	}
	check_alive("the client's own resize request");
	check(!resizing(), "the release ended the resize");
	check(grown_by(view, 80, 60), std::format("the window grew by the drag ({}x{} to {}x{})",
		resized_from.width, resized_from.height,
		view != nullptr ? view->content_box().width : 0,
		view != nullptr ? view->content_box().height : 0));
	check(view != nullptr && far_edges_held(resized_from, view->content_box()),
		"the right and bottom edges held, the top left corner having moved");
	return after(400);
}

Result asked_resize_edges_held_after_commit() {
	View *view = titled("x11-ask");
	check(view != nullptr && far_edges_held(resized_from, view->content_box()),
		"and after the client's next commit");
	client.kill();
	return after(300);
}

// ---- a modal dialog whose hints reach the window manager after it maps: the
// kind they decide is laid out when they arrive, as at the map ----

Result camera_goes_home() {
	output().camera.jump(home);
	return after(300);
}

Result late_dialog_client_starts() {
	Desktop *shown = output().desktop_showing();
	check(shown != nullptr, "the screen shows a desktop");
	if (shown != nullptr && !shown->tiling()) {
		chord(KEY_T, mods::super);
	}
	check(start_x11(client, {"--size=600x400", "--title=x11-late", "--dialog-late"}),
		"an X11 window that opens its modal dialog bare started on a tiled desktop");
	return after(1500);
}

Result late_window_maps() {
	if (auto wait = wait_for(titled("x11-late") != nullptr, {.budget_ms = 20'000})) {
		return *wait;
	}
	return after(800); // tiled by now
}

// Its first key opens the dialog with no hints.
Result late_dialog_key() {
	tap(KEY_D);
	return after(600);
}

Result late_dialog_maps() {
	if (auto wait = wait_for(titled("x11-late-dialog") != nullptr, {.budget_ms = 20'000})) {
		return *wait;
	}
	return after(600);
}

// Mapped bare it is a window like any: the board holds it.
Result bare_dialog_is_on_the_board() {
	View *main = titled("x11-late"), *dialog = titled("x11-late-dialog");
	check(main != nullptr && main->tiled && main->board_seat.has_value(),
		"the window joined the tiled desktop's board");
	check(dialog != nullptr && dialog->kind() == ViewKind::normal && dialog->tiled
			&& dialog->board_seat.has_value(),
		"its dialog, mapped before its hints, joined the board as a plain window");
	// The second key gives the dialog its hints, after the map.
	tap(KEY_D);
	return after(600);
}

// The hints are in wlroots, and so was the layout when they arrived.
Result late_hints_arrive() {
	View *dialog = titled("x11-late-dialog");
	const X11View *shell = dialog != nullptr ? dialog->as_x11() : nullptr;
	bool arrived = shell != nullptr && shell->wlr->parent != nullptr && shell->wlr->modal;
	if (auto wait = wait_for(arrived, {.budget_ms = 10'000})) {
		return *wait;
	}
	check(arrived, "the dialog's hints, sent after its map, reached the window manager");
	View *main = titled("x11-late");
	check(dialog != nullptr && dialog->kind() == ViewKind::modal,
		"the dialog is a modal one now");
	check(dialog != nullptr && !dialog->tiled && !dialog->board_seat.has_value(),
		"its modal dialog floats, off the board");
	check(main != nullptr && dialog != nullptr && centred_on(*dialog, *main),
		"centred over the window it blocks");
	chord(KEY_T, mods::super);
	return after(600);
}

Result late_tiling_comes_back() {
	chord(KEY_T, mods::super);
	return after(600);
}

Result late_tiling_leaves_the_dialog_floating() {
	View *main = titled("x11-late"), *dialog = titled("x11-late-dialog");
	check(main != nullptr && main->tiled && main->board_seat.has_value()
			&& dialog != nullptr && !dialog->tiled && !dialog->board_seat.has_value(),
		"switched off and on, tiling leaves the late modal dialog floating");
	client.kill();
	return after(300);
}

// ---- a modal dialog whose map request finds Xwayland stopped: the
// compositor goes on answering, and lays the dialog out as a modal one when
// X answers at last ----

Result stall_client_starts() {
	Desktop *shown = output().desktop_showing();
	if (shown != nullptr && !shown->tiling()) {
		chord(KEY_T, mods::super);
	}
	check(start_x11(client, {"--size=600x400", "--title=x11-stall", "--dialog"}),
		"an X11 window that opens a modal dialog started on a tiled desktop");
	return after(1500);
}

Result stall_window_maps() {
	if (auto wait = wait_for(titled("x11-stall") != nullptr, {.budget_ms = 20'000})) {
		return *wait;
	}
	return after(800); // tiled by now
}

// Its first key opens the dialog, hints and map in one flush.
Result stall_is_armed() {
	check(arm_stall(), "Xwayland's process was found, and the stall armed for the next window");
	tap(KEY_D);
	return after(100);
}

Result stalled_compositor_answers() {
	if (auto wait = wait_for(!read_text(stall.result).empty(),
			{.poll_ms = 20, .budget_ms = 2000})) {
		return *wait;
	}
	std::string said = read_text(stall.result);
	long ms = said.empty() ? -1 : std::strtol(said.c_str(), nullptr, 10);
	check(stall.probe > 0,
		"the dialog's map request reached the compositor, which stopped Xwayland");
	check(ms >= 0 && ms <= 100, std::format("with Xwayland stopped on the dialog's map "
			"request, the compositor answers `msg state` within 100 ms (took {} ms)", ms));
	check(titled("x11-stall-dialog") == nullptr,
		"the dialog is still waiting on X after the compositor answered");
	reap_stall();
	return after(300);
}

Result stalled_dialog_maps() {
	if (auto wait = wait_for(titled("x11-stall-dialog") != nullptr, {.budget_ms = 20'000})) {
		return *wait;
	}
	return after(600);
}

// X has answered: the dialog is laid out as a modal one.
Result stalled_dialog_is_modal() {
	View *main = titled("x11-stall"), *dialog = titled("x11-stall-dialog");
	check(dialog != nullptr && dialog->kind() == ViewKind::modal,
		"once X answered, the dialog is a modal one");
	check(dialog != nullptr && !dialog->tiled && !dialog->board_seat.has_value(),
		"its modal dialog floats, off the board");
	check(main != nullptr && dialog != nullptr && centred_on(*dialog, *main),
		"centred over the window it blocks");
	ask_state();
	return after(600);
}

Result compositor_answers_after_the_stall() {
	if (auto wait = wait_for(command_done("state"), {.budget_ms = 5000})) {
		return *wait;
	}
	check_alive("the dialog mapped late");
	client.kill();
	return after(300);
}

Result run_ends() {
	return finish();
}

} // namespace

int main() {
	if (!begin("xwayland", {"CANVAS_X11_CLIENT", "CANVAS_LOCK_CLIENT", "WAYLAND_CANVAS_BIN"})) {
		return 2;
	}
	// Nothing touches the real Pictures: the user dir points into the scratch
	// tree. glib caches the user dirs, so this happens before the server exists.
	pictures_dir = scratch_dir("pictures");
	write_file(scratch() + "/xdg-config/user-dirs.dirs",
		std::format("XDG_PICTURES_DIR=\"{}\"\n", pictures_dir));
	Options options;
	options.start_bar = false;
	return run(options, {
		{"client starts", client_starts},
		{"window maps", window_maps},
		{"click and key", click_and_key},
		{"click and key arrive", click_and_key_arrive},
		{"maximize click", maximize_click},
		{"window maximized", window_maximized},
		{"close click", close_click},
		{"client ends", client_ends},
		{"popup client starts", popup_client_starts},
		{"popup maps", popup_maps},
		{"popup is placed", popup_is_placed},
		{"clipboard owner starts", clipboard_owner_starts},
		{"clipboard is asked", clipboard_is_asked},
		{"x11 clipboard pastes", x11_clipboard_pastes},
		{"wayland copies", wayland_copies},
		{"paste client starts", paste_client_starts},
		{"wayland clipboard pastes", wayland_clipboard_pastes},
		{"compositor without xwayland starts", compositor_without_xwayland_starts},
		{"compositor without xwayland ends", compositor_without_xwayland_ends},
		{"main window starts", main_window_starts},
		{"dialog window starts", dialog_window_starts},
		{"both are mapped", both_are_mapped},
		{"click on the dialog", click_on_the_dialog},
		{"dialog got the click", dialog_got_the_click},
		{"main got the click", main_got_the_click},
		{"raised main covers the dialog", raised_main_covers_the_dialog},
		{"click on the dialog again", click_on_the_dialog_again},
		{"minimized takes no click", minimized_takes_no_click},
		{"far window maps", far_window_maps},
		{"pointer comes to the far window", pointer_comes_to_the_far_window},
		{"click on the far window", click_on_the_far_window},
		{"far window got the click", far_window_got_the_click},
		{"big window maps", big_window_maps},
		{"pointer comes to the far corner", pointer_comes_to_the_far_corner},
		{"click the far corner", click_the_far_corner},
		{"pointer comes to the near corner", pointer_comes_to_the_near_corner},
		{"click the near corner", click_the_near_corner},
		{"big window got the corner clicks", big_window_got_the_corner_clicks},
		{"press after the jump", press_after_the_jump},
		{"press after the jump landed", press_after_the_jump_landed},
		{"jump and press landed", jump_and_press_landed},
		{"tiled client starts", tiled_client_starts},
		{"tiled window maps", tiled_window_maps},
		{"tiled dialog key", tiled_dialog_key},
		{"tiled dialog maps", tiled_dialog_maps},
		{"tiled dialog floats", tiled_dialog_floats},
		{"tiling comes back", tiling_comes_back},
		{"tiling leaves the dialog floating", tiling_leaves_the_dialog_floating},
		{"desktop two window starts", desktop_two_window_starts},
		{"desktop two window takes the keyboard", desktop_two_window_takes_the_keyboard},
		{"desktop one keeps its window", desktop_one_keeps_its_window},
		{"desktop two keeps its window", desktop_two_keeps_its_window},
		{"covered window starts", covered_window_starts},
		{"fullscreen window starts", fullscreen_window_starts},
		{"fullscreen over another window", fullscreen_over_another_window},
		{"fullscreen has a desktop of its own", fullscreen_has_a_desktop_of_its_own},
		{"fullscreen comes back", fullscreen_comes_back},
		{"fullscreen alone", fullscreen_alone},
		{"fullscreen stays in place", fullscreen_stays_in_place},
		{"fullscreen client ends", fullscreen_client_ends},
		{"act window starts", act_window_starts},
		{"super is held over the window", super_is_held_over_the_window},
		{"right button goes down", right_button_goes_down},
		{"right drag begins", right_drag_begins},
		{"right drag moves on", right_drag_moves_on},
		{"right drag moves further", right_drag_moves_further},
		{"right drag arrives", right_drag_arrives},
		{"right button is released", right_button_is_released},
		{"right drag resized", right_drag_resized},
		{"act window is clicked", act_window_is_clicked},
		{"session locks", session_locks},
		{"session is locked", session_is_locked},
		{"session unlocks", session_unlocks},
		{"session is unlocked", session_is_unlocked},
		{"keyboard is back on the window", keyboard_is_back_on_the_window},
		{"shot hotkey", shot_hotkey},
		{"click shoots the window", click_shoots_the_window},
		{"window shot is saved", window_shot_is_saved},
		{"pointer comes to the left edge", pointer_comes_to_the_left_edge},
		{"left button goes down", left_button_goes_down},
		{"left edge drag begins", left_edge_drag_begins},
		{"left edge drag moves on", left_edge_drag_moves_on},
		{"left edge drag arrives", left_edge_drag_arrives},
		{"left button is released", left_button_is_released},
		{"left edge drag resized", left_edge_drag_resized},
		{"left edge edges held after commit", left_edge_edges_held_after_commit},
		{"moveresize client starts", moveresize_client_starts},
		{"moveresize window maps", moveresize_window_maps},
		{"press on the asking window", press_on_the_asking_window},
		{"client's request begins a resize", clients_request_begins_a_resize},
		{"asked resize arrives", asked_resize_arrives},
		{"asking button is released", asking_button_is_released},
		{"asked resize done", asked_resize_done},
		{"asked resize edges held after commit", asked_resize_edges_held_after_commit},
		{"camera goes home", camera_goes_home},
		{"late dialog client starts", late_dialog_client_starts},
		{"late window maps", late_window_maps},
		{"late dialog key", late_dialog_key},
		{"late dialog maps", late_dialog_maps},
		{"bare dialog is on the board", bare_dialog_is_on_the_board},
		{"late hints arrive", late_hints_arrive},
		{"late tiling comes back", late_tiling_comes_back},
		{"late tiling leaves the dialog floating", late_tiling_leaves_the_dialog_floating},
		{"stall client starts", stall_client_starts},
		{"stall window maps", stall_window_maps},
		{"stall is armed", stall_is_armed},
		{"stalled compositor answers", stalled_compositor_answers},
		{"stalled dialog maps", stalled_dialog_maps},
		{"stalled dialog is modal", stalled_dialog_is_modal},
		{"compositor answers after the stall", compositor_answers_after_the_stall},
		{"end", run_ends},
	}, reap_stall);
}
