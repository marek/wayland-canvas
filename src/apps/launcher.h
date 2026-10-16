#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "apps/apps.h"
#include "apps/icons.h"
#include "core/util.h"

class Output;
class Server;

// The Start-menu-style app launcher overlay.
class Launcher {
public:
	explicit Launcher(Server &server);
	~Launcher();
	Launcher(const Launcher &) = delete;
	Launcher &operator=(const Launcher &) = delete;

	// Opens on `output` (closes when already open anywhere); nullptr does nothing.
	void toggle(Output *output);
	void close();
	bool is_open() const;

	// While open the launcher owns the keyboard: every press is consumed. `utf32` is the
	// character the key produces (0 for none), `mods` the WLR_MODIFIER_* mask.
	void key(xkb_keysym_t sym, uint32_t utf32, uint32_t mods);

	// Pointer at output-local logical (ox, oy). True when the event is consumed: over the
	// panel, or (button) a click outside that closes it.
	bool pointer_motion(Output &output, double ox, double oy);
	bool pointer_button(Output &output, double ox, double oy, uint32_t button, bool pressed);
	bool pointer_axis(Output &output, double ox, double oy, double delta);

	struct Panel {
		wlr_texture *texture = nullptr;
		wlr_box box{}; // output-local logical
	};
	// The panel image for `output` (rebuilt when stale); nullopt unless open there.
	std::optional<Panel> panel(Output &output);

	// Before `output` is destroyed: closes the launcher if it is open there.
	void output_destroyed(Output &output);
	// Redraws the open panel in the new palette.
	void theme_changed();
	void finish(); // releases the panel texture, apps and icons before the renderer goes

private:
	Server &server_;
	Apps apps_;
	Icons icons_;

	bool open_ = false;
	Output *output_ = nullptr;

	std::string query_;
	std::vector<std::size_t> results_; // indices into apps_.items()

	std::size_t selected_ = 0; // index into results_
	std::size_t scroll_ = 0; // first visible row
	int hover_ = -1; // result index under the pointer, -1 none
	int pressed_row_ = -1; // result index of the pending click, -1 none

	TexturePtr texture_; // cached panel image
	float texture_scale_ = 0; // scale it was drawn for
	wlr_box texture_box_{}; // layout it was drawn for
	bool stale_ = true;

	void open(Output &output);
	bool query_pop_char();
	bool query_pop_word();
	void query_changed();
	// Runs the selected entry and closes. Launched from the overview, its
	// windows open in the overview's target, whatever the overview does
	// before they appear.
	void launch_selected();
	void scroll_to_selection(int rows);
	void move_selection(int rows, int delta);
	int row_at(const wlr_box &box, int rows, double ox, double oy) const;
	TexturePtr draw_panel(const Output &output, const wlr_box &box, int rows);
};
