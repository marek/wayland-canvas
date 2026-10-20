#pragma once

#include "windows/view.h"

// An X11 window through Xwayland: the other half of View, its twin being
// XdgView (docs/intents/windows/x11.md). It holds the xwayland surface, which
// only the files of src/windows/ reach, and the members the X window manager
// (view_x11.cpp's Server functions) calls.
class X11View final : public View {
public:
	X11View(Server &server, wlr_xwayland_surface *wlr);

	// The view whose X11 window holds `surface`; nullptr for any other surface.
	static X11View *from_surface(wlr_surface *surface);

	wlr_surface *surface() const override;
	wlr_box shell_geometry() const override;
	bool initialized() const override;
	void send_close() override;
	void configure_size(int width, int height) override;
	void tell_maximized(bool want) override;
	void tell_fullscreen(bool want) override;
	void tell_tiled(bool want) override;
	void tell_activated(bool want) override;
	const char *shell_title() const override;
	const char *shell_app_id() const override;
	pid_t client_pid() const override;
	bool wants_maximized() const override;
	bool wants_fullscreen() const override;
	View *shell_parent() const override;
	bool modal() const override;
	bool client_placed() const override;
	bool decorated_without_negotiating() const override;
	SizeLimit min_size() const override;
	SizeLimit max_size() const override;
	void for_each_surface(
		std::function_ref<void(wlr_surface *surface, int sx, int sy)> fn) const override;
	wlr_surface *surface_at(double lx, double ly, double *sx, double *sy) const override;
	void begin_interactive_resize(uint32_t edges) override;
	void resize_to(wlr_box content) override;
	void end_interactive_resize() override;
	X11View *as_x11() override { return this; }
	const X11View *as_x11() const override { return this; }

	// Where an override-redirect window asked to be, on the canvas.
	void x11_place_override_redirect();
	// Where X has the window: its canvas position less the shared offset.
	int16_t x11_x() const;
	int16_t x11_y() const;
	// Tells X the window's canvas place, when X has it elsewhere; true when
	// it did.
	bool x11_follow_canvas();
	// Tells X the window's place and size; input for an X11 window then
	// waits until X has taken it in.
	void x11_configure(int16_t x, int16_t y, uint16_t width, uint16_t height);

	wlr_xwayland_surface *const wlr;
	// A menu, a tooltip or a drag icon, which the window manager must not
	// decorate, list or place. Set when the window gets its surface.
	bool override_redirect = false;

protected:
	void drop_resize() override;

private:
	void handle_x11_associate();
	void handle_x11_dissociate();
	void handle_x11_map();
	void handle_x11_configure(wlr_xwayland_surface_configure_event *event);
	void handle_x11_set_geometry();
	void handle_x11_request_resize(wlr_xwayland_resize_event *event);
	void handle_x11_request_maximize();
	void handle_x11_request_minimize();
	void handle_x11_request_fullscreen();

	// The surface comes and goes with the pairing; map, unmap and commit are
	// connected while it is there.
	Listener x11_associate_, x11_dissociate_, x11_configure_, x11_set_geometry_;
	Listener map_, unmap_, commit_, destroy_;
	Listener request_move_, request_resize_, request_maximize_, request_minimize_,
		request_fullscreen_;
	Listener set_title_, set_app_id_;
};
