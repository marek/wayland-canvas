#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "core/util.h"

class Server;
class View;

enum class DecoPart {
	none,
	title,
	minimize,
	maximize,
	close,
	resize,
};

struct DecoHit {
	DecoPart part = DecoPart::none;
	uint32_t edges = 0; // wlr_edges, for DecoPart::resize
};

// A view's server-side title bar and border: the mode negotiated with the client, hit
// testing and the cached title bar texture.
class Decoration {
public:
	explicit Decoration(View &view);
	~Decoration();
	Decoration(const Decoration &) = delete;
	Decoration &operator=(const Decoration &) = delete;

	int titlebar() const; // title bar height, logical px
	int border() const;
	// What the canvas point lies on at `zoom`.
	DecoHit hit(double cx, double cy, double zoom) const;
	// Redraws the title bar texture at `scale` when what it shows changed.
	void update(float scale);
	// Drops the cached texture so the next update() redraws in the new palette.
	void theme_changed();
	wlr_texture *texture() const;
	// View initial commit: send the SERVER_SIDE mode once the toplevel is initialized.
	void handle_initial_commit();
	// Recomputes `ssd` from the negotiated decoration modes; damages when it flips. Called
	// on map and whenever a mode changes.
	void refresh_mode();

	bool ssd = false; // client accepted server-side decorations
	DecoPart hover = DecoPart::none, pressed = DecoPart::none; // pointer state

private:
	// Takes the toplevel's new xdg-decoration object and starts negotiating with it.
	void attach(wlr_xdg_toplevel_decoration_v1 *decoration);
	void handle_request_mode();
	void handle_deco_destroy();

	View &view_;
	wlr_xdg_toplevel_decoration_v1 *xdg_decoration_ = nullptr;
	Listener deco_request_mode_, deco_destroy_;
	TexturePtr texture_; // the title bar as drawn for the cached state below
	int width_ = 0;
	float scale_ = 0;
	bool active_ = false, maximized_ = false;
	DecoPart hover_ = DecoPart::none, pressed_ = DecoPart::none;
	std::optional<std::string> title_;

	friend class Decorations;
};

// xdg-decoration and KDE server-decoration negotiation for every view.
class Decorations {
public:
	explicit Decorations(Server &server);
	~Decorations();
	Decorations(const Decorations &) = delete;
	Decorations &operator=(const Decorations &) = delete;

	void init(); // creates both managers and listens for new decorations
	void finish(); // disconnects before the display is destroyed

	// True when a KDE server-decoration for `surface` negotiated server-side mode.
	bool kde_server_mode(wlr_surface *surface) const;

private:
	struct KdeDeco;
	void handle_new_xdg_decoration(wlr_xdg_toplevel_decoration_v1 *decoration);
	void handle_new_kde_decoration(wlr_server_decoration *decoration);

	Server &server_;
	wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr_ = nullptr;
	wlr_server_decoration_manager *kde_decoration_mgr_ = nullptr;
	std::vector<std::unique_ptr<KdeDeco>> kde_decos_; // one per KDE decoration object
	Listener new_xdg_decoration_, new_kde_decoration_;
};
