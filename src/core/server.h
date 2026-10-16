#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include "input/bindings.h"
#include "canvas/canvas.h"
#include "canvas/items.h"
#include "input/gesture.h"
#include "input/input.h"
#include "platform/ipc.h"
#include "platform/image_capture.h"
#include "apps/launcher.h"
#include "core/theme.h"
#include "render/render.h"
#include "core/util.h"
#include "windows/view.h"

class LayerSurface;
class Output;

struct Options {
	const char *startup_cmd = nullptr;
	bool start_bar = true; // autostart the top bar when qs is available
	double scale = 0; // 0: pick per output
};

double now_ms();
// Runs `cmd` through /bin/sh -c, double-forked so it never becomes a zombie.
void spawn(const char *cmd);

// The compositor: wlroots globals, every object on the canvas, and the subsystems.
class Server {
public:
	Server();
	~Server();
	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;

	// False (logged) when the backend, renderer or allocator cannot be created; the
	// destructor still releases whatever was made.
	bool init(const Options &options);
	// Opens the Wayland socket, starts the backend and runs the startup command.
	bool start();

	// output.cpp
	void damage_all();
	Output *output_at(double lx, double ly);
	Output *output_under_cursor();
	void update_visibility();

	// server.cpp: switches the palette, repaints, tells the subscribers.
	void set_theme(ThemeMode mode);

	// canvas.cpp
	void reveal_view(View &view);

	// view.cpp
	std::optional<ViewHit> view_at(double cx, double cy, double zoom);
	View *top_visible_view(Output *output);
	View *last_minimized_view();
	// Activates and raises `view`; nullptr clears the focus.
	void focus_view(View *view);
	void focus_keyboard(wlr_surface *surface);

	// layer.cpp
	void refocus_layers();

	wl_display *display = nullptr;
	wl_event_loop *loop = nullptr;
	wlr_backend *backend = nullptr;
	wlr_session *session = nullptr;
	wlr_renderer *renderer = nullptr;
	wlr_allocator *allocator = nullptr;
	wlr_compositor *compositor = nullptr;
	wlr_output_layout *output_layout = nullptr;
	wlr_xdg_shell *xdg_shell = nullptr;
	wlr_layer_shell_v1 *layer_shell = nullptr;
	wlr_foreign_toplevel_manager_v1 *foreign_mgr = nullptr;
	wlr_pointer_gestures_v1 *pointer_gestures = nullptr;
	wlr_presentation *presentation = nullptr;
	wlr_idle_notifier_v1 *idle_notifier = nullptr;
	wlr_idle_inhibit_manager_v1 *idle_inhibit = nullptr;
	wlr_seat *seat = nullptr;
	wlr_cursor *cursor = nullptr;
	wlr_xcursor_manager *xcursor_mgr = nullptr;
	Options options;

	std::vector<std::unique_ptr<Output>> outputs;
	std::vector<std::unique_ptr<View>> toplevels; // every view, mapped or not
	std::vector<View *> views; // mapped views in stacking order, front is topmost
	std::vector<std::unique_ptr<Popup>> popups;

	View *focused_view = nullptr;
	LayerSurface *focused_layer = nullptr; // keyboard-interactive layer surface
	uint64_t minimize_counter = 0;
	bool visibility_dirty = false;
	bool pointer_focus_stale = false; // the canvas moved under the pointer: rebase next frame

	Input input;
	Bindings bindings;
	Gestures gestures;
	Hint hint;
	Launcher launcher;
	Painter painter;
	Theme theme;
	Decorations decorations;
	Canvas canvas;
	Items items;
	Ipc ipc;
	ImageCapture image_capture;

private:
	// output.cpp
	void init_outputs();
	void finish_outputs();
	void handle_new_output(wlr_output *wlr);
	void handle_layout_change();
	Listener new_output_, layout_change_;

	// view.cpp
	void init_views();
	void finish_views();
	void handle_new_xdg_toplevel(wlr_xdg_toplevel *toplevel);
	void handle_new_xdg_popup(wlr_xdg_popup *popup);
	Listener new_xdg_toplevel_, new_xdg_popup_;

	// layer.cpp
	void init_layers();
	void finish_layers();
	void handle_new_layer_surface(wlr_layer_surface_v1 *layer_surface);
	Listener new_layer_surface_;

	// server.cpp
	struct SurfaceTracker;
	struct Inhibitor;
	void handle_new_surface(wlr_surface *surface);
	void handle_request_activate(wlr_xdg_activation_v1_request_activate_event *event);
	static int handle_terminate_signal(int signal, void *data);
	void start_bar();
	void handle_new_inhibitor(wlr_idle_inhibitor_v1 *inhibitor);
	std::vector<std::unique_ptr<SurfaceTracker>> trackers_;
	std::vector<std::unique_ptr<Inhibitor>> inhibitors_;
	Listener new_surface_, request_activate_, new_inhibitor_;
};
