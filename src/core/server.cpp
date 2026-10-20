#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <limits.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "core/config.h"
#include "platform/layer.h"
#include "platform/output.h"
#include "platform/session_bus.h"
#include "platform/session_services.h"
#include "core/server.h"
#include "windows/view.h"

namespace {

bool is_dir(const std::string &path) {
	struct stat st{};
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// True when `name` names an executable on $PATH.
bool program_on_path(const char *name) {
	if (strchr(name, '/') != nullptr) {
		return access(name, X_OK) == 0;
	}
	const char *path = getenv("PATH");
	if (path == nullptr) {
		return false;
	}
	std::string_view rest = path;
	while (!rest.empty()) {
		std::string_view entry;
		std::size_t colon = rest.find(':');
		if (colon == std::string_view::npos) {
			entry = rest;
			rest = {};
		} else {
			entry = rest.substr(0, colon);
			rest.remove_prefix(colon + 1);
		}
		if (entry.empty()) {
			entry = ".";
		}
		std::string candidate = std::string(entry) + "/" + name;
		if (access(candidate.c_str(), X_OK) == 0) {
			return true;
		}
	}
	return false;
}

// The directory of the running executable, when it can be read: the
// shell's own helpers sit beside it.
std::optional<std::string> self_dir() {
	char self[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
	if (n <= 0) {
		return std::nullopt;
	}
	std::string dir(self, static_cast<std::size_t>(n));
	if (std::size_t slash = dir.rfind('/'); slash != std::string::npos) {
		dir.resize(slash);
	}
	return dir;
}

// The directory holding the bar's Quickshell config: $WAYLAND_CANVAS_SHELL_DIR
// when set, else beside the compositor binary — the build tree's shell/, then
// the installed share/wayland-canvas/shell.
std::optional<std::string> bar_shell_dir() {
	if (const char *env = getenv("WAYLAND_CANVAS_SHELL_DIR")) {
		if (is_dir(env)) {
			return env;
		}
	}
	std::optional<std::string> dir = self_dir();
	if (!dir) {
		return std::nullopt;
	}
	for (const char *rel : {config::bar_shell_build_dir, config::bar_shell_install_dir}) {
		std::string candidate = *dir + "/" + rel;
		if (is_dir(candidate)) {
			return candidate;
		}
	}
	return std::nullopt;
}


// ---- canvas.conf (core/settings.h) ----

// $XDG_CONFIG_HOME when absolute, else ~/.config.
std::string config_home() {
	if (const char *xdg = getenv("XDG_CONFIG_HOME");
			xdg != nullptr && xdg[0] == '/') {
		return xdg;
	}
	if (const char *home = getenv("HOME"); home != nullptr && home[0] == '/') {
		return std::string(home) + "/.config";
	}
	return {};
}

// Reads a whole file; empty when missing or unreadable, so every default
// applies (docs/intents/input/config-file.md).
std::string read_text_file(const std::string &path) {
	std::FILE *file = std::fopen(path.c_str(), "rb");
	if (file == nullptr) {
		return {};
	}
	std::string text;
	char chunk[4096];
	std::size_t n = 0;
	while ((n = std::fread(chunk, 1, sizeof chunk, file)) > 0) {
		text.append(chunk, n);
	}
	std::fclose(file);
	return text;
}

// Writes a whole file; false when it cannot.
bool write_text_file(const std::string &path, const std::string &text) {
	std::FILE *file = std::fopen(path.c_str(), "wb");
	if (file == nullptr) {
		return false;
	}
	bool ok = text.empty() || std::fwrite(text.data(), 1, text.size(), file) == text.size();
	ok = std::fflush(file) == 0 && ok;
	if (std::fclose(file) != 0) {
		ok = false;
	}
	return ok;
}

// mkdir -p.
void mkdir_p(const std::string &path) {
	for (std::size_t slash = path.find('/'); ; slash = path.find('/', slash + 1)) {
		std::string made = path.substr(0, slash == std::string::npos
			? path.size() : slash);
		if (!made.empty() && mkdir(made.c_str(), 0755) != 0 && errno != EEXIST) {
			return; // the write that follows fails and is logged there
		}
		if (slash == std::string::npos) {
			return;
		}
	}
}

// The gesture code's own options from the settings' own preset enum: the
// settings library links no wlroots, so it carries its own (the values
// match one for one).
GestureOptions gesture_options(const settings::Resolved &conf) {
	GestureOptions options;
	switch (conf.swipes) {
	case settings::SwipePreset::three_finger_hops:
		options.swipes = SwipePreset::three_finger_hops;
		break;
	case settings::SwipePreset::four_finger_hops:
		options.swipes = SwipePreset::four_finger_hops;
		break;
	case settings::SwipePreset::off:
		options.swipes = SwipePreset::off;
		break;
	}
	options.pinch_scale = conf.pinch_scale;
	options.overview_down = conf.overview_down;
	options.hops_away = conf.hops_away;
	return options;
}

// Single-quotes `text` for /bin/sh.
std::string shell_quote(std::string_view text) {
	std::string out = "'";
	for (char c : text) {
		if (c == '\'') {
			out += "'\\''";
		} else {
			out += c;
		}
	}
	out += '\'';
	return out;
}

// Keeps the idle notifier inhibited while at least one inhibitor is alive;
// `delta` adjusts for the one whose destroy signal is running.
void update_idle_inhibit(Server &server, int delta) {
	int count = wl_list_length(&server.idle_inhibit->inhibitors) + delta;
	wlr_idle_notifier_v1_set_inhibited(server.idle_notifier, count > 0);
	server.lock.set_idle_inhibited(count > 0);
}

// SIGPIPE as the compositor found it. The compositor ignores the signal for
// itself (see Server::init); what it starts gets this back, since an ignored
// signal stays ignored across exec.
void (*inherited_sigpipe)(int) = SIG_DFL;
bool sigpipe_recorded = false;

// The desktop name every client sees; "wlroots" lets the portal pick its
// wlroots backend where our portals config is not installed.
constexpr const char *desktop_name = "wayland-canvas:wlroots";
// The portals a hand-over and a hand-back restart, so they serve the display
// the systemd environment now names.
constexpr const char *portal_units =
	"xdg-desktop-portal.service xdg-desktop-portal-wlr.service xdg-desktop-portal-gtk.service";

// The handed-over values in the user's systemd environment, read now.
session_bus::Handed systemd_handed() {
	FILE *pipe = popen("systemctl --user show-environment", "r");
	if (pipe == nullptr) {
		return {};
	}
	std::string out;
	char chunk[4096];
	std::size_t n = 0;
	while ((n = std::fread(chunk, 1, sizeof chunk, pipe)) > 0) {
		out.append(chunk, n);
	}
	pclose(pipe);
	return session_bus::handed_in(out);
}

// Puts this compositor's WAYLAND_DISPLAY and desktop name into the
// activation environment of the services the session bus starts — the
// screen-sharing portal and its wlroots backend first among them — when
// the bus is this run's to take (docs/intents/platform/protocols.md). A
// login returns the values it replaced, for the hand-back on exit.
session_bus::Owner bus_owner(bool login) {
	const char *address = getenv("DBUS_SESSION_BUS_ADDRESS");
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	return session_bus::owner(address ? address : "", runtime ? runtime : "", login);
}

std::optional<session_bus::Handed> hand_over_session_bus(bool login) {
	switch (bus_owner(login)) {
	case session_bus::Owner::none:
	case session_bus::Owner::host:
		return std::nullopt;
	case session_bus::Owner::own:
		spawn("dbus-update-activation-environment WAYLAND_DISPLAY XDG_CURRENT_DESKTOP");
		return std::nullopt;
	case session_bus::Owner::login: {
		session_bus::Handed before = systemd_handed();
		// Portals an earlier session left running still serve that
		// session's display: restart the ones that run.
		spawn(std::format("dbus-update-activation-environment --systemd WAYLAND_DISPLAY"
			" XDG_CURRENT_DESKTOP && systemctl --user try-restart {}", portal_units).c_str());
		return before;
	}
	}
	return std::nullopt;
}

// Gives the portal back to the session a login took it from: the values it
// replaced restored (unset ones unset) and the portals restarted, unless
// another session has taken it since.
void hand_back_session_bus(const session_bus::HandOver &hand_over) {
	std::optional<session_bus::Handed> restore =
		session_bus::hand_back(hand_over, systemd_handed());
	if (!restore.has_value()) {
		return;
	}
	std::string set, unset;
	auto add = [&set, &unset](const char *name, const std::optional<std::string> &value) {
		if (value.has_value()) {
			set += " " + shell_quote(std::format("{}={}", name, *value));
			return;
		}
		unset += std::format(" {}", name);
	};
	add("WAYLAND_DISPLAY", restore->wayland_display);
	add("XDG_CURRENT_DESKTOP", restore->current_desktop);
	std::string cmd;
	if (!set.empty()) {
		cmd += "dbus-update-activation-environment --systemd" + set + " && ";
	}
	if (!unset.empty()) {
		cmd += "systemctl --user unset-environment" + unset + " && ";
	}
	spawn((cmd + "systemctl --user try-restart " + portal_units).c_str());
}

} // namespace

double now_ms() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

pid_t spawn(const char *cmd) {
	// The intermediate child reports the command's pid through a pipe, so a
	// launch can be traced to the windows its processes open.
	int report[2];
	if (pipe2(report, O_CLOEXEC) < 0) {
		wlr_log_errno(WLR_ERROR, "pipe failed");
		return -1;
	}
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork failed");
		close(report[0]);
		close(report[1]);
		return -1;
	}
	if (pid == 0) {
		// Double fork so the command is reparented and never becomes a zombie.
		close(report[0]);
		setsid();
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, nullptr);
		std::signal(SIGPIPE, inherited_sigpipe);
		pid_t command = fork();
		if (command == 0) {
			// The command starts with only its three standard fds:
			// nothing the compositor holds open reaches it.
			close_range(STDERR_FILENO + 1, ~0U, 0);
			execl("/bin/sh", "/bin/sh", "-c", cmd, static_cast<char *>(nullptr));
			_exit(127);
		}
		ssize_t sent = write(report[1], &command, sizeof command);
		_exit(sent == static_cast<ssize_t>(sizeof command) ? 0 : 1);
	}
	close(report[1]);
	pid_t command = -1;
	ssize_t got = 0;
	do {
		got = read(report[0], &command, sizeof command);
	} while (got < 0 && errno == EINTR);
	close(report[0]);
	waitpid(pid, nullptr, 0);
	return got == static_cast<ssize_t>(sizeof command) ? command : -1;
}

// Per-surface commit tracking: repaint only the outputs showing the surface.
struct Server::SurfaceTracker {
	SurfaceTracker(Server &server, wlr_surface *surface)
		: server_(server), surface_(surface) {
		commit_.connect<&SurfaceTracker::handle_commit>(surface->events.commit, this);
		destroy_.connect<&SurfaceTracker::handle_destroy>(surface->events.destroy, this);
	}

	void handle_commit() {
		wlr_surface *surface = surface_;
		if (surface->role != nullptr && surface->role->name != nullptr &&
				strcmp(surface->role->name, "wl_pointer-cursor") == 0) {
			return; // wlr_cursor draws these
		}
		wlr_surface *root = wlr_surface_get_root_surface(surface);
		wlr_layer_surface_v1 *layer = wlr_layer_surface_v1_try_from_wlr_surface(root);
		wlr_xdg_popup *popup = wlr_xdg_popup_try_from_wlr_surface(root);
		while (layer == nullptr && popup != nullptr && popup->parent != nullptr) {
			root = wlr_surface_get_root_surface(popup->parent);
			layer = wlr_layer_surface_v1_try_from_wlr_surface(root);
			popup = wlr_xdg_popup_try_from_wlr_surface(root);
		}
		if (layer != nullptr) {
			LayerSurface *l = static_cast<LayerSurface *>(layer->data);
			if (l != nullptr) {
				l->damage();
			}
			return;
		}
		View *view = View::from_surface(root);
		if (view != nullptr) {
			view->damage();
			return;
		}
		server_.damage_all(); // drag icons and other unowned surfaces
	}

	void handle_destroy() {
		destroy_owned(server_.trackers_, this);
	}

	Server &server_;
	wlr_surface *surface_;
	Listener commit_, destroy_;
};

// One idle inhibitor client resource.
struct Server::Inhibitor {
	Inhibitor(Server &server, wlr_idle_inhibitor_v1 *wlr)
		: server_(server) {
		destroy_.connect<&Inhibitor::handle_destroy>(wlr->events.destroy, this);
	}

	void handle_destroy() {
		// Still listed while its destroy signal runs.
		update_idle_inhibit(server_, -1);
		destroy_owned(server_.inhibitors_, this);
	}

	Server &server_;
	Listener destroy_;
};

Server::Server()
	: input(*this), bindings(*this), gestures(*this), hint(*this), launcher(*this),
	painter(*this), decorations(*this), canvas(*this), ipc(*this),
	image_capture(*this),
	capture_ui(*this), lock(*this), night_light(*this), logind(*this) {
}

Server::~Server() {
	sigint_source_.reset();
	sigterm_source_.reset();
	conf_inotify_source_.reset();
	if (conf_inotify_fd_ >= 0) {
		close(conf_inotify_fd_);
		conf_inotify_fd_ = -1;
	}
	if (manages_services_) {
		// The session ends: its services go with it. A crash runs nothing
		// here, and the next login stops what it left behind.
		spawn(session_services::stop_command().c_str());
	}
	if (hand_over_.has_value()) {
		hand_back_session_bus(*hand_over_);
	}
	if (display != nullptr) {
		wl_display_destroy_clients(display);
	}
	ipc.finish();
	night_light.finish();
	logind.finish();
	lock.finish();
	capture_ui.finish();
	image_capture.finish();
	launcher.finish();
	painter.finish();
	hint.finish();
	// wlroots asserts that destroyed objects have no listeners left.
	input.finish();
	decorations.finish();
	finish_layers();
	finish_xwayland();
	finish_xdg_shell();
	finish_views();
	finish_outputs();
	new_surface_.disconnect();
	request_activate_.disconnect();
	new_inhibitor_.disconnect();
	if (xcursor_mgr != nullptr) {
		wlr_xcursor_manager_destroy(xcursor_mgr);
	}
	if (cursor != nullptr) {
		wlr_cursor_destroy(cursor);
	}
	if (backend != nullptr) {
		wlr_backend_destroy(backend);
	}
	if (allocator != nullptr) {
		wlr_allocator_destroy(allocator);
	}
	if (renderer != nullptr) {
		wlr_renderer_destroy(renderer);
	}
	if (display != nullptr) {
		wl_display_destroy(display);
	}
}

bool Server::init(const Options &options) {
	this->options = options;

	display = wl_display_create();
	loop = wl_display_get_event_loop(display);
	// IPC writes to clients that exit mid-line must fail with EPIPE, not
	// take the compositor down: Ipc::flush already keeps such writes queued
	// until the connection's EOF is dispatched. The disposition found here
	// is what spawned commands get back; a second Server in the same process
	// would find the first one's ignore instead, so only the first records.
	void (*previous)(int) = std::signal(SIGPIPE, SIG_IGN);
	if (!sigpipe_recorded) {
		// A handler would not survive exec: only an ignore is passed on.
		inherited_sigpipe = previous == SIG_IGN ? SIG_IGN : SIG_DFL;
		sigpipe_recorded = true;
	}
	sigint_source_.reset(wl_event_loop_add_signal(loop, SIGINT, handle_terminate_signal, this));
	sigterm_source_.reset(wl_event_loop_add_signal(loop, SIGTERM, handle_terminate_signal, this));

	backend = wlr_backend_autocreate(loop, &session);
	if (backend == nullptr) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return false;
	}
	renderer = wlr_renderer_autocreate(backend);
	if (renderer == nullptr) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return false;
	}
	wlr_renderer_init_wl_display(renderer, display);
	allocator = wlr_allocator_autocreate(backend, renderer);
	if (allocator == nullptr) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return false;
	}

	wl_display *d = display;
	compositor = wlr_compositor_create(d, 6, renderer);
	wlr_subcompositor_create(d);
	wlr_data_device_manager_create(d);
	wlr_primary_selection_v1_device_manager_create(d);
	wlr_data_control_manager_v1_create(d);
	wlr_ext_data_control_manager_v1_create(d, 1);
	wlr_viewporter_create(d);
	wlr_single_pixel_buffer_manager_v1_create(d);
	wlr_fractional_scale_manager_v1_create(d, 1);
	presentation = wlr_presentation_create(d, backend, 2);
	wlr_export_dmabuf_manager_v1_create(d);
	wlr_screencopy_manager_v1_create(d);
	wlr_relative_pointer_manager_v1_create(d);
	pointer_gestures = wlr_pointer_gestures_v1_create(d);
	foreign_mgr = wlr_foreign_toplevel_manager_v1_create(d);
	idle_notifier = wlr_idle_notifier_v1_create(d);
	idle_inhibit = wlr_idle_inhibit_v1_create(d);
	new_inhibitor_.connect<&Server::handle_new_inhibitor>(
		idle_inhibit->events.new_inhibitor, this);

	wlr_xdg_activation_v1 *activation = wlr_xdg_activation_v1_create(d);
	request_activate_.connect<&Server::handle_request_activate>(
		activation->events.request_activate, this);
	// A wayland dialog says through it that it is modal: it floats over the
	// window it blocks (docs/intents/windows/window-management.md).
	wlr_xdg_wm_dialog_v1_create(d, 1);

	new_surface_.connect<&Server::handle_new_surface>(
		compositor->events.new_surface, this);

	output_layout = wlr_output_layout_create(d);
	wlr_xdg_output_manager_v1_create(d, output_layout);

	seat = wlr_seat_create(d, "seat0");
	// The seat always offers a pointer and a keyboard, so clients bind both
	// at start: a keyboard that connects later (wtype, an on-screen keyboard)
	// types into the focus from its first key, instead of racing the
	// client's get_keyboard after a late capability change.
	wlr_seat_set_capabilities(seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
	cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(cursor, output_layout);
	const char *size_env = getenv("XCURSOR_SIZE");
	int cursor_size = size_env != nullptr && atoi(size_env) > 0 ? atoi(size_env) : 24;
	xcursor_mgr = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"), cursor_size);

	init_outputs();
	init_xdg_shell();
	init_layers();
	decorations.init();
	input.init();
	image_capture.init();
	lock.init();
	night_light.init();
	return true;
}

bool Server::start() {
	const char *socket = wl_display_add_socket_auto(display);
	if (socket == nullptr) {
		wlr_log(WLR_ERROR, "failed to open a Wayland socket");
		return false;
	}
	if (!wlr_backend_start(backend)) {
		wlr_log(WLR_ERROR, "failed to start backend");
		return false;
	}
	setenv("WAYLAND_DISPLAY", socket, true);
	unsetenv("DISPLAY");
	// Xwayland after the unset, so $DISPLAY names this session's X server
	// and not the one the compositor was started from. It is created now
	// and started by the first X11 client (view_x11.cpp).
	init_xwayland();
	// A nested run inherits the host compositor's control sockets; unset them
	// so no client of ours reaches the host.
	unsetenv("HYPRLAND_INSTANCE_SIGNATURE");
	unsetenv("SWAYSOCK");
	unsetenv("I3SOCK");
	unsetenv("NIRI_SOCKET");
	// Before any client is spawned, so they inherit the socket path.
	ipc.init();
	// canvas.conf: written with every default when missing (its directory
	// created with it), then watched — a save or a delete re-reads it and
	// applies to the running session (docs/intents/input/config-file.md).
	conf_path = config_home() + "/wayland-canvas/canvas.conf";
	if (config_home().size() > 0) {
		if (access(conf_path.c_str(), F_OK) != 0) {
			mkdir_p(config_home() + "/wayland-canvas");
			if (!write_text_file(conf_path, settings::template_text())) {
				wlr_log(WLR_ERROR, "could not write %s: the defaults apply",
					conf_path.c_str());
			}
		}
		conf_inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		if (conf_inotify_fd_ < 0 || inotify_add_watch(conf_inotify_fd_,
				(config_home() + "/wayland-canvas").c_str(),
				IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE) < 0) {
			wlr_log(WLR_ERROR, "could not watch %s for changes",
				conf_path.c_str());
			if (conf_inotify_fd_ >= 0) {
				close(conf_inotify_fd_);
				conf_inotify_fd_ = -1;
			}
		} else {
			conf_inotify_source_.reset(wl_event_loop_add_fd(
				wl_display_get_event_loop(display), conf_inotify_fd_,
				WL_EVENT_READABLE, handle_conf_change, this));
		}
	} else {
		wlr_log(WLR_ERROR, "neither $XDG_CONFIG_HOME nor $HOME is set: "
			"canvas.conf is unreadable, the defaults apply");
	}
	load_conf(true);
	// Every client is in a wayland-canvas session.
	setenv("XDG_CURRENT_DESKTOP", desktop_name, true);
	if (std::optional<session_bus::Handed> before = hand_over_session_bus(session != nullptr)) {
		hand_over_ = session_bus::HandOver{
			.ours = {.wayland_display = socket, .current_desktop = desktop_name},
			.before = *before,
		};
	}
	// A login watches its logind session: lock-session and sleep lock it.
	logind.init(session != nullptr);
	// ...and brings the session's own services up, once its environment is
	// where the user manager can see it
	// (docs/intents/workspace/session-services.md).
	if (session_services::manages(bus_owner(session != nullptr))) {
		manages_services_ = true;
		spawn(session_services::start_command().c_str());
	}
	wlr_log(WLR_INFO, "Running wayland-canvas on WAYLAND_DISPLAY=%s", socket);
	if (options.start_bar) {
		start_bar();
	}
	if (options.startup_cmd != nullptr) {
		spawn(options.startup_cmd);
	}
	return true;
}

// Autostarts the shell (the plugin host and its bar) when its client and
// its config are both present. QML_IMPORT_PATH is prefixed with the
// shell's ui/ dir so every plugin, wherever the host found it, can
// `import WaylandCanvas.Ui`.
void Server::start_bar() {
	if (!program_on_path(config::bar_program)) {
		wlr_log(WLR_INFO, "%s is not on PATH: no top bar", config::bar_program);
		return;
	}
	std::optional<std::string> dir = bar_shell_dir();
	if (!dir) {
		wlr_log(WLR_INFO,
			"no bar config beside the binary or at $WAYLAND_CANVAS_SHELL_DIR: no top bar");
		return;
	}
	std::string cmd = "QML_IMPORT_PATH=" + shell_quote(*dir + "/ui")
		+ "${QML_IMPORT_PATH:+:$QML_IMPORT_PATH} " + config::bar_program
		+ " -p " + shell_quote(*dir);
	// The shell's helpers (its msg client among them) sit beside the
	// compositor: its own directory comes first on the shell's PATH.
	if (std::optional<std::string> self = self_dir()) {
		cmd = "PATH=" + shell_quote(*self) + ":$PATH " + cmd;
	}
	spawn(cmd.c_str());
}

// Reads canvas.conf and applies it to the running session: the bindings'
// tables, the gesture options, and — when the file has lines it ignored —
// the hint that names the first on every screen. The first load never
// emits the settings event; later ones only when something changed, so
// the watch's own echo of a write_conf is silent.
void Server::load_conf(bool first) {
	std::string text = read_text_file(conf_path);
	settings::Parsed parsed = settings::parse(text);
	settings::Resolved resolved = settings::resolve(parsed);
	bool changed = !first && !(resolved == conf);
	conf = std::move(resolved);
	conf_text = text;
	conf_errors = std::move(parsed.errors);
	bindings.apply(conf);
	gestures.set_options(gesture_options(conf));
	input.settings_changed();
	lock.settings_changed();
	night_light.settings_changed();
	if (!conf_errors.empty()) {
		for (const settings::Error &error : conf_errors) {
			wlr_log(WLR_ERROR, "canvas.conf line %d: %s", error.line,
				error.text.c_str());
		}
		const settings::Error &first_error = conf_errors.front();
		std::string hint_text = std::format("canvas.conf line {}: {}",
			first_error.line, first_error.text);
		if (conf_errors.size() > 1) {
			hint_text += std::format(" (+{} more)", conf_errors.size() - 1);
		}
		hint.set(hint_text);
	}
	if (changed) {
		ipc.emit_settings();
	}
}

// Writes `text` as canvas.conf — beside the old file, then renamed over
// it, so neither an editor nor a crash sees half a file — and applies it
// at once. False, logged, when the file cannot be written: the running
// settings stay.
bool Server::write_conf(const std::string &text) {
	std::string tmp = conf_path + ".tmp-" + std::to_string(getpid());
	if (!write_text_file(tmp, text)
			|| rename(tmp.c_str(), conf_path.c_str()) != 0) {
		wlr_log(WLR_ERROR, "could not write %s: the running settings stay",
			conf_path.c_str());
		unlink(tmp.c_str());
		return false;
	}
	load_conf(false); // the settings event leaves with it; the watch echoes
	return true;
}

int Server::handle_conf_change(int fd, uint32_t mask, void *data) {
	auto *server = static_cast<Server *>(data);
	if ((mask & WL_EVENT_READABLE) == 0) {
		return 0;
	}
	// Read every event pending: a save of canvas.conf — written in place
	// or renamed over, as editors do — or its deletion re-reads it.
	alignas(struct inotify_event) char buf[4096];
	bool reload = false;
	ssize_t n = 0;
	while ((n = read(fd, buf, sizeof buf)) > 0) {
		char *at = buf;
		while (at + sizeof(struct inotify_event) <= buf + n) {
			auto *event = reinterpret_cast<struct inotify_event *>(at);
			at += sizeof(struct inotify_event) + event->len;
			if (event->len > 0 && strcmp(event->name, "canvas.conf") == 0) {
				reload = true;
			}
		}
	}
	if (reload) {
		server->load_conf(false);
	}
	return 0;
}

void Server::set_theme(ThemeMode mode) {
	if (theme.mode() == mode) {
		return; // no change: no event, no repaint
	}
	theme.set_mode(mode);
	// Every cached texture was drawn with the old palette.
	painter.theme_changed();
	hint.theme_changed();
	launcher.theme_changed();
	capture_ui.theme_changed();
	for (std::unique_ptr<View> &view : toplevels) {
		view->deco.theme_changed();
	}
	damage_all();
	ipc.emit_theme();
}

void Server::handle_new_surface(wlr_surface *surface) {
	trackers_.push_back(std::make_unique<SurfaceTracker>(*this, surface));
}

void Server::handle_request_activate(wlr_xdg_activation_v1_request_activate_event *event) {
	View *view = View::from_surface(event->surface);
	// Only honour tokens minted from real input, so apps can't steal focus.
	if (view != nullptr && view->mapped && event->token->seat != nullptr) {
		view->activate();
	}
}

void Server::handle_new_inhibitor(wlr_idle_inhibitor_v1 *wlr_inhibitor) {
	inhibitors_.push_back(std::make_unique<Inhibitor>(*this, wlr_inhibitor));
	update_idle_inhibit(*this, 0);
}

int Server::handle_terminate_signal(int, void *data) {
	auto *server = static_cast<Server *>(data);
	wl_display_terminate(server->display);
	return 0;
}
