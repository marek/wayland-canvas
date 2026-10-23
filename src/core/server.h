#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include "input/bindings.h"
#include "canvas/canvas.h"
#include "input/gesture.h"
#include "input/input.h"
#include "core/changes.h"
#include "core/conf_store.h"
#include "platform/ipc_events.h"
#include "platform/ipc_server.h"
#include "platform/image_capture.h"
#include "platform/lock.h"
#include "platform/logind.h"
#include "apps/launcher.h"
#include "apps/capture_ui.h"
#include "core/theme.h"
#include "platform/night_light.h"
#include "render/hint.h"
#include "render/render.h"
#include "render/text_cache.h"
#include "core/util.h"
#include "core/options.h"
#include "platform/session_environment.h"
#include "windows/launches.h"
#include "windows/popup.h"
#include "windows/view.h"

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

	// output.cpp
	void damage_all();
	Output *output_at(double lx, double ly);
	Output *output_under_cursor();
	// The output whose viewport holds the canvas point, nullptr when none
	// does; `normal_view` leaves out the outputs showing the overview.
	enum class Showing { viewport, normal_view };
	Output *output_showing(double cx, double cy, Showing which = Showing::viewport);
	// Which outputs show which window may have changed; the next
	// update_visibility() works them out again. View::visibility_changed and
	// Output::visibility_changed pair this with the repaint.
	void visibility_changed() { visibility_dirty_ = true; }
	// Works every window's outputs out again when visibility_changed() said
	// they may differ.
	void update_visibility();

	// server.cpp: switches the palette, repaints, tells the subscribers.
	void set_theme(ThemeMode mode);

	// A window was unmapped or destroyed: every holder of a pointer to it
	// drops it, here and nowhere else.
	void view_gone(View &view);

	// view.cpp
	std::optional<ViewHit> view_at(double cx, double cy, double zoom);
	View *top_visible_view(Output *output);
	View *last_minimized_view();
	// Brings `view` into sight: the camera pans to its desktop or its box, or
	// an open overview exits onto it; nothing moves when it is already in view.
	void reveal_view(View &view);
	// Activates and raises `view`; nullptr clears the focus.
	void focus_view(View *view);
	// Keyboard focus follows a move onto `box`: the focused window keeps it
	// when it stands there, else the topmost window there, the last used,
	// takes it (docs/intents/windows/window-management.md).
	void focus_in(wlr_box box);
	void focus_keyboard(wlr_surface *surface);
	// view_x11.cpp: X's own picture of its windows follows the canvas
	// (docs/intents/windows/x11.md). Once per frame X is told the canvas's
	// stacking and the windows' places; at every pointer move the offset
	// moves if X's pointer would leave X's screen.
	void sync_x11();
	void x11_follow_pointer();
	// A move, a resize or a restack went to X: input for an X11 window waits
	// until X has taken it in.
	void x11_changed();
	// Whether X has taken in every change sent to it; asks X to say so when
	// changes went out since the last ask.
	bool x11_settled();
	// X did not answer within config::x11_input_wait_ms: input goes on
	// without the answer, and the ask is dropped.
	void x11_give_up();
	// A window's _NET_WM_STATE, as the client set it, asked of X at the map
	// request without waiting: the ask's sequence, nullopt when X is not
	// there. X answers before it sends the window's MapNotify, so the answer
	// is taken there (X11View::handle_x11_map_notify) and costs no wait.
	std::optional<unsigned int> x11_ask_state(xcb_window_t window);
	// Whether the answer to `ask` holds _NET_WM_STATE_MODAL; takes the answer.
	bool x11_state_modal(unsigned int ask);
	// Gives up an answer that will not be taken.
	void x11_drop_ask(unsigned int ask);
	// Whether `surface` belongs to an X11 window: input to one waits for X.
	static bool on_x11_window(wlr_surface *surface);

	// layer.cpp
	void refocus_layers();
	// The keyboard back from an on-demand shell surface to the focused window.
	void return_keyboard();
	// Puts a plugin's surface on the plane above the others.
	void raise_canvas_surface(LayerSurface &layer);

	wl_display *display = nullptr;
	wl_event_loop *loop = nullptr;
	wlr_backend *backend = nullptr;
	wlr_session *session = nullptr;
	wlr_renderer *renderer = nullptr;
	wlr_allocator *allocator = nullptr;
	wlr_compositor *compositor = nullptr;
	wlr_output_layout *output_layout = nullptr;
	wlr_xdg_shell *xdg_shell = nullptr;
	wlr_xwayland *xwayland = nullptr;
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

	// Declared before everything that holds a texture of it, so it outlives them.
	TextCache text_cache;

	std::vector<std::unique_ptr<Output>> outputs;
	std::vector<std::unique_ptr<View>> toplevels; // every view, mapped or not
	std::vector<View *> views; // mapped views in stacking order, front is topmost
	std::vector<std::unique_ptr<Popup>> popups;

	View *focused_view = nullptr;
	LayerSurface *focused_layer = nullptr; // keyboard-interactive layer surface
	uint64_t minimize_counter = 0;
	// What the control socket's subscribers have not been told yet: the
	// owner of a fact calls changed(), and IpcEvents reads the flags once per
	// frame (docs/intents/shell/app-bar.md for the window list).
	void changed(Change what);
	Changes changes;
	bool pointer_focus_stale = false; // the canvas moved under the pointer: rebase next frame
	// X positions are canvas positions less this offset, one for every X11
	// window, so they keep their places relative to each other.
	int x11_offset_x = 0, x11_offset_y = 0;

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
	ConfStore conf;

	// Where a plugin asked for its surfaces to be on the plane, by name
	// (docs/intents/platform/ipc.md). Kept here, not on the surface, so a
	// placement asked for before the surface exists — or again after a
	// shell reload — still lands when it maps.
	std::map<std::string, wlr_box> canvas_places;
	// The mapped surfaces that belong on the plane, bottom first: every
	// output draws and hit-tests them in this order, whichever output each
	// was made on.
	std::vector<LayerSurface *> canvas_stack;

private:
	// output.cpp
	void init_outputs();
	void finish_outputs();
	void handle_new_output(wlr_output *wlr);
	void handle_layout_change();
	Listener new_output_, layout_change_;
	bool visibility_dirty_ = false; // see visibility_changed()

	// view.cpp
	void finish_views();
	// view_xdg.cpp: the xdg-shell global and the windows and popups it brings
	void init_xdg_shell();
	void finish_xdg_shell();
	void handle_new_xdg_toplevel(wlr_xdg_toplevel *toplevel);
	void handle_new_xdg_popup(wlr_xdg_popup *popup);
	Listener new_xdg_toplevel_, new_xdg_popup_;
	// view_x11.cpp: Xwayland and the X11 windows it brings
	void init_xwayland();
	void finish_xwayland();
	void handle_xwayland_ready();
	void handle_new_xwayland_surface(wlr_xwayland_surface *surface);
	// Sees every X event before wlroots handles it; wlroots still handles it.
	void handle_x11_event(xcb_generic_event_t &event);
	Listener new_xwayland_surface_, xwayland_ready_;
	// X's stacking as last sent, bottom first, by view session id.
	std::vector<uint64_t> x11_stack_;
	// After X's windows move, X's pointer is put where the cursor is over
	// them, on the window manager's own connection, so X applies it after
	// the moves.
	void x11_put_pointer();
	// Asks X, on the window manager's connection, to answer once it has
	// taken in everything sent before: that request's sequence, and whether
	// changes went out since it was sent.
	void x11_ask_settled();
	std::optional<unsigned int> x11_settle_ask_;
	bool x11_unasked_ = false;
	// The atoms of _NET_WM_STATE and _NET_WM_STATE_MODAL on the running X server.
	uint32_t x11_atom_state_ = 0, x11_atom_modal_ = 0;

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
	EventSourcePtr sigint_source_, sigterm_source_;
	std::vector<std::unique_ptr<SurfaceTracker>> trackers_;
	std::vector<std::unique_ptr<Inhibitor>> inhibitors_;
	Listener new_surface_, request_activate_, new_inhibitor_;
	// Present from start() to the end of the run.
	std::optional<SessionEnvironment> environment_;
};
