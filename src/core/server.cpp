#include <csignal>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

#include "core/config.h"
#include "core/process.h"
#include "core/text.h"
#include "platform/ipc_methods.h"
#include "platform/layer.h"
#include "platform/output.h"
#include "core/server.h"
#include "core/theme_prefs.h"
#include "input/ui_layer.h"
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

// Keeps the idle notifier inhibited while at least one inhibitor is alive;
// `delta` adjusts for the one whose destroy signal is running.
void update_idle_inhibit(Server &server, int delta) {
	int count = wl_list_length(&server.idle_inhibit->inhibitors) + delta;
	wlr_idle_notifier_v1_set_inhibited(server.idle_notifier, count > 0);
	server.lock.set_idle_inhibited(count > 0);
}

} // namespace

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
		server_.outputs.damage_all(); // drag icons and other unowned surfaces
	}

	void handle_destroy() {
		destroy_owned(server_.trackers_, this);
	}

	Server &server_;
	wlr_surface *surface_;
	Listener commit_, destroy_;
};

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
	: text_cache(*this), outputs(*this), input(*this), bindings(*this), gestures(*this),
	hint(*this),
	launcher(*this),
	painter(*this), decorations(*this), canvas(*this), ipc(), ipc_events(*this, ipc),
	image_capture(*this),
	capture_ui(*this), lock(*this), night_light(*this), logind(*this), focus(*this),
	canvas_surfaces(*this), conf(*this), xwm(*this), xdg_shell(*this), layer_shell(*this) {
	register_ipc_methods(ipc, *this);
	input.set_layers({&capture_ui, &launcher});
	input.set_gate(lock);
}

Server::~Server() {
	// The reverse of init() and start(). The signal sources go first: nothing
	// reads the loop from here on, so a signal now must take the process down,
	// not wait in a source nobody dispatches.
	sigint_source_.reset();
	sigterm_source_.reset();
	// The session ends: its services go with it, and the portal goes back.
	environment_.reset();
	logind.finish();
	conf.finish();
	// The clients go before the subsystems that listen to them.
	if (display != nullptr) {
		wl_display_destroy_clients(display);
	}
	ipc.finish();
	xwm.finish();
	night_light.finish();
	lock.finish();
	for (UiLayer *layer : input.layers()) {
		layer->finish();
	}
	image_capture.finish();
	text_cache.clear();
	hint.finish();
	// wlroots asserts that destroyed objects have no listeners left.
	input.finish();
	decorations.finish();
	layer_shell.finish();
	xdg_shell.finish();
	// Release textures and foreign toplevel handles before the renderer and display go.
	views.clear();
	toplevels.clear();
	outputs.finish();
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
	// IPC writes to clients that exit mid-line must fail with EPIPE, not take
	// the compositor down: IpcServer::flush keeps such writes queued until the
	// connection's EOF is dispatched.
	ignore_sigpipe();
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
	// The seat always offers a pointer and a keyboard, so a keyboard that
	// connects later (wtype, an on-screen keyboard) types into the focus from
	// its first key instead of racing the client's get_keyboard.
	wlr_seat_set_capabilities(seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
	cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(cursor, output_layout);
	const char *size_env = getenv("XCURSOR_SIZE");
	int cursor_size = size_env != nullptr && atoi(size_env) > 0 ? atoi(size_env)
		: config::cursor_size;
	xcursor_mgr = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"), cursor_size);

	outputs.init();
	xdg_shell.init();
	layer_shell.init();
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
	// and started by the first X11 client (windows/xwm.cpp).
	xwm.init();
	// A nested run inherits the host compositor's control sockets; unset them
	// so no client of ours reaches the host.
	unsetenv("HYPRLAND_INSTANCE_SIGNATURE");
	unsetenv("SWAYSOCK");
	unsetenv("I3SOCK");
	unsetenv("NIRI_SOCKET");
	// Before any client is spawned, so they inherit the socket path.
	ipc.init(display, loop);
	conf.init();
	// A login watches its logind session: lock-session and sleep lock it.
	logind.init(session != nullptr);
	environment_.emplace(socket, session != nullptr);
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
	std::string cmd = "QML_IMPORT_PATH=" + text::shell_quote(*dir + "/ui")
		+ "${QML_IMPORT_PATH:+:$QML_IMPORT_PATH} " + config::bar_program
		+ " -p " + text::shell_quote(*dir);
	// The shell's helpers (its msg client among them) sit beside the
	// compositor: its own directory comes first on the shell's PATH.
	if (std::optional<std::string> self = self_dir()) {
		cmd = "PATH=" + text::shell_quote(*self) + ":$PATH " + cmd;
	}
	spawn(cmd.c_str());
}

void Server::set_theme(ThemeMode mode) {
	// The applications' preference follows every call, a changed palette or
	// not: the shell's start push makes it match the mode the compositor has.
	theme_prefs::apply(mode, session != nullptr);
	if (theme.mode() == mode) {
		return;
	}
	theme.set_mode(mode);
	// Every cached texture was drawn with the old palette.
	text_cache.clear();
	outputs.damage_all();
	changed(Change::theme);
}

void Server::changed(Change what) {
	changes.set(what);
	// IpcEvents reads the flags in a frame handler, and an idle compositor has
	// no frame coming: ask for one.
	if (!outputs.empty()) {
		wlr_output_schedule_frame(outputs.front()->wlr);
	}
}

void Server::flush_pending() {
	// A window that left fullscreen, by any path, comes back from its own
	// desktop before the frame shows where it is.
	canvas.fullscreen_desktops.reconcile();
	outputs.update_visibility();
	ipc_events.flush();
	if (pointer_focus_stale) {
		pointer_focus_stale = false;
		input.rebase_cursor();
	}
	xwm.sync();
}

void Server::view_gone(View &view) {
	focus.forget(view);
	input.forget(view);
	capture_ui.forget(view);
	image_capture.forget(view);
}

void Server::output_gone(Output &output) {
	input.output_gone(output);
	gestures.output_gone(output);
	for (View *view : views) {
		view->output_gone(output);
	}
	output.release_layers();
	outputs.visibility_changed();
}

void Server::outputs_changed() {
	changed(Change::outputs);
	input.outputs_changed();
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
