#pragma once

#include "core/util.h"

class Output;
class View;

// What a mapped window tells the foreign-toplevel protocols: the handle taskbars
// list it by (wlr-foreign-toplevel-management) and the one capture clients name it by
// (ext-foreign-toplevel-list). The handles exist while the window is mapped; their
// requests are the window's own actions.
class ForeignToplevel {
public:
	explicit ForeignToplevel(View &view);
	~ForeignToplevel();
	ForeignToplevel(const ForeignToplevel &) = delete;
	ForeignToplevel &operator=(const ForeignToplevel &) = delete;

	// Creates the handles and publishes the window as it is.
	void open();
	void close();
	// Makes the handles say what the window is now: title, app id, mode, minimized,
	// activated. One call per change; what did not change sends nothing.
	void publish();
	// The window entered or left `output`'s viewport.
	void output_enter(Output &output);
	void output_leave(Output &output);
	// The ext handle's identifier, or null with no handle.
	const char *identifier() const;

private:
	void handle_activate();
	void handle_maximize(const wlr_foreign_toplevel_handle_v1_maximized_event *event);
	void handle_minimize(const wlr_foreign_toplevel_handle_v1_minimized_event *event);
	void handle_fullscreen(const wlr_foreign_toplevel_handle_v1_fullscreen_event *event);
	void handle_close();

	View &view_;
	Handle<wlr_foreign_toplevel_handle_v1, wlr_foreign_toplevel_handle_v1_destroy> handle_{};
	Listener activate_, maximize_, minimize_, fullscreen_, close_;
	Handle<wlr_ext_foreign_toplevel_handle_v1,
		wlr_ext_foreign_toplevel_handle_v1_destroy> ext_handle_{};
};
