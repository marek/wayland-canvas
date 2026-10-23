#pragma once

#include <memory>
#include <optional>
#include "input/bindings.h"
#include "canvas/canvas.h"
#include "input/gesture.h"
#include "input/input.h"
#include "core/changes.h"
#include "core/conf_store.h"
#include "platform/canvas_surfaces.h"
#include "platform/ipc_events.h"
#include "platform/ipc_server.h"
#include "platform/image_capture.h"
#include "platform/layer_shell.h"
#include "platform/lock.h"
#include "platform/logind.h"
#include "apps/launcher.h"
#include "apps/capture_ui.h"
#include "core/theme.h"
#include "platform/night_light.h"
#include "platform/outputs.h"
#include "render/hint.h"
#include "render/render.h"
#include "render/text_cache.h"
#include "core/util.h"
#include "core/options.h"
#include "platform/session_environment.h"
#include "windows/focus.h"
#include "windows/launches.h"
#include "windows/popup.h"
#include "windows/view.h"
#include "windows/xdg_shell.h"
#include "windows/xwm.h"

class LayerSurface;
class Output;

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

	// Switches the palette, repaints, tells the subscribers.
	void set_theme(ThemeMode mode);
	// Settles what the events since the last frame left pending, before a
	// frame draws: a window that left fullscreen goes back to its desktop,
	// every window's outputs are worked out again, the control socket's
	// subscribers are told what changed, the pointer's focus is rebased on
	// a canvas that moved under it, and X is told the canvas's places. An
	// output's frame runs it first; every step consumes what is pending or
	// compares with what it last sent, so the frame of a second output in
	// the same refresh finds nothing left to do.
	void flush_pending();

	// A window was unmapped or destroyed: every holder of a pointer to it
	// drops it, here and nowhere else.
	void view_gone(View &view);
	// An output is about to be destroyed: every holder of a pointer to it
	// drops it, here and nowhere else, as view_gone does for a window.
	void output_gone(Output &output);
	// An output came or went: the state the control socket tells lists it
	// differently, and every touchscreen and pen is mapped to one again.
	void outputs_changed();

	wl_display *display = nullptr;
	wl_event_loop *loop = nullptr;
	wlr_backend *backend = nullptr;
	wlr_session *session = nullptr;
	wlr_renderer *renderer = nullptr;
	wlr_allocator *allocator = nullptr;
	wlr_compositor *compositor = nullptr;
	wlr_output_layout *output_layout = nullptr;
	wlr_foreign_toplevel_manager_v1 *foreign_mgr = nullptr;
	wlr_pointer_gestures_v1 *pointer_gestures = nullptr;
	wlr_presentation *presentation = nullptr;
	wlr_idle_notifier_v1 *idle_notifier = nullptr;
	wlr_idle_inhibit_manager_v1 *idle_inhibit = nullptr;
	wlr_seat *seat = nullptr;
	wlr_cursor *cursor = nullptr;
	wlr_xcursor_manager *xcursor_mgr = nullptr;
	Options options;

	// Declared before everything that holds a texture of it, so it outlives them.
	TextCache text_cache;

	Outputs outputs;
	std::vector<std::unique_ptr<View>> toplevels; // every view, mapped or not
	std::vector<View *> views; // mapped views in stacking order, front is topmost
	std::vector<std::unique_ptr<Popup>> popups;

	// What the control socket's subscribers have not been told yet: the
	// owner of a fact calls changed(), and IpcEvents reads the flags once per
	// frame (docs/intents/shell/app-bar.md for the window list).
	void changed(Change what);
	Changes changes;
	bool pointer_focus_stale = false; // the canvas moved under the pointer: rebase next frame

	Input input;
	Bindings bindings;
	Gestures gestures;
	Hint hint;
	Launcher launcher;
	Launches launches;
	Painter painter;
	Theme theme;
	Decorations decorations;
	Canvas canvas;
	IpcServer ipc;
	IpcEvents ipc_events;
	ImageCapture image_capture;
	CaptureUi capture_ui;
	SessionLock lock;
	NightLight night_light;
	Logind logind;
	Focus focus;
	CanvasSurfaces canvas_surfaces;
	ConfStore conf;
	Xwm xwm;
	XdgShell xdg_shell;
	LayerShell layer_shell;

private:
	struct SurfaceTracker;
	struct Inhibitor;
	void handle_new_surface(wlr_surface *surface);
	void handle_request_activate(wlr_xdg_activation_v1_request_activate_event *event);
	static int handle_terminate_signal(int signal, void *data);
	void start_bar();
	void handle_new_inhibitor(wlr_idle_inhibitor_v1 *inhibitor);
	EventSourcePtr sigint_source_, sigterm_source_;
	std::vector<std::unique_ptr<SurfaceTracker>> trackers_;
	std::vector<std::unique_ptr<Inhibitor>> inhibitors_;
	Listener new_surface_, request_activate_, new_inhibitor_;
	// Present from start() to the end of the run.
	std::optional<SessionEnvironment> environment_;
};
