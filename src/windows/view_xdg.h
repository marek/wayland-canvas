#pragma once

#include <optional>
#include "windows/view.h"

// A window that speaks xdg-shell: the wayland half of View, its twin being
// X11View. It holds the toplevel, which only the files of src/windows/ reach;
// view_xdg.cpp answers the shell queries and listens to the toplevel.
class XdgView final : public View {
public:
	XdgView(Server &server, wlr_xdg_toplevel *wlr);

	// The view whose toplevel holds `surface`, directly or through the popups
	// that hang from it; nullptr for any other surface.
	static XdgView *from_surface(wlr_surface *surface);

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

	wlr_xdg_toplevel *const wlr;

protected:
	void drop_resize() override;

private:
	// The client sizes itself and its commit arrives later, so the dragged
	// left or top edge is not where the window's position says: each commit
	// puts the position back so the opposite edge holds.
	struct ResizeAnchor {
		uint32_t edges; // the edges being dragged
		wlr_box start; // the content box the drag began with
		// The configure that ended the resize, 0 while it goes on: its commit
		// arrives after the release, and anchoring lasts until the client acks it.
		uint32_t end_serial = 0;
	};

	void anchor_resize();
	void handle_xdg_commit();
	void handle_xdg_request_resize(wlr_xdg_toplevel_resize_event *event);
	void handle_xdg_request_maximize();
	void handle_xdg_request_minimize();
	void handle_xdg_request_fullscreen();

	std::optional<ResizeAnchor> resize_anchor_;
	Listener map_, unmap_, commit_, destroy_;
	Listener request_move_, request_resize_, request_maximize_, request_minimize_,
		request_fullscreen_;
	Listener set_title_, set_app_id_;
};
