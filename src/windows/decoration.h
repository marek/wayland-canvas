#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "core/config.h"
#include "core/geometry.h"
#include "core/util.h"
#include "render/cached_texture.h"

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

// The title bar's buttons from left to right, against its right edge.
inline constexpr std::array<DecoPart, 3> deco_buttons{
	DecoPart::minimize, DecoPart::maximize, DecoPart::close};

// The `index`th of deco_buttons on a title bar `width` logical px wide,
// bar-local: drawing, hit-testing and the tests that click it share it.
inline wlr_box button_box(std::size_t index, int width) {
	int from_right = static_cast<int>(deco_buttons.size() - index);
	return {width - from_right * config::button_width, 0, config::button_width,
		config::titlebar_height};
}

// The middle of `part`'s button on a view whose frame is `frame`, canvas
// coordinates.
inline Point button_centre(const wlr_box &frame, DecoPart part) {
	std::size_t index = static_cast<std::size_t>(std::ranges::find(deco_buttons, part)
		- deco_buttons.begin());
	wlr_box box = button_box(index, frame.width);
	return {frame.x + box.x + box.width / 2.0, frame.y + box.y + box.height / 2.0};
}

// What a title bar's picture shows besides its scale.
struct BarState {
	int width = 0;
	bool active = false, maximized = false;
	DecoPart hover = DecoPart::none, pressed = DecoPart::none;
	std::string title; // empty for none
	bool operator==(const BarState &) const = default;
};

// A view's server-side title bar and border: the mode negotiated with the client, hit
// testing and the cached title bar textures.
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
	// The title bar's picture at `scale`, drawn again when what it shows
	// changed; nullptr while the window has no title bar.
	wlr_texture *title_bar(float scale);
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
	// Draws the picture of `wanted_` at `scale`.
	TexturePtr draw_bar(float scale) const;

	View &view_;
	wlr_xdg_toplevel_decoration_v1 *xdg_decoration_ = nullptr;
	Listener deco_request_mode_, deco_destroy_;
	CachedTexture<BarState> bar_;
	BarState wanted_; // what the bar shows now, reused for each lookup

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
