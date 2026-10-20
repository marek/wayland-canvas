#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "apps/apps.h"
#include "apps/icons.h"
#include "core/util.h"
#include "apps/launcher_layout.h"
#include "input/ui_layer.h"
#include "render/cached_texture.h"

class Output;
class Server;

// The Start-menu-style app launcher overlay.
class Launcher final : public UiLayer {
public:
	explicit Launcher(Server &server);
	~Launcher();
	Launcher(const Launcher &) = delete;
	Launcher &operator=(const Launcher &) = delete;

	// Opens on `output` (closes when already open anywhere); nullptr does nothing.
	void toggle(Output *output);
	void close();
	bool is_open() const;

	// While open the launcher owns the keyboard: every press is consumed,
	// and its own chords — the ones that open it — close it.
	bool owns_keyboard() const override { return is_open(); }
	void key(const KeyPress &press) override;
	// A lone Super tap runs its action: the launcher's own tap closes it.
	bool allows_tap() const override { return true; }

	// Open on `output`: every press there is the launcher's, inside the
	// panel or a click outside that closes it. Motion and scrolling are
	// consumed only over the panel.
	bool takes_pointer_at(const Output &output, double ox, double oy) const override;
	bool pointer_motion(Output &output, double ox, double oy) override;
	bool pointer_button(Output &output, double ox, double oy, uint32_t button,
		bool pressed) override;
	bool pointer_axis(Output &output, double ox, double oy,
		wl_pointer_axis orientation, double delta) override;
	bool blocks_gestures() const override { return false; }
	const char *cursor_image() const override { return "default"; }

	struct Panel {
		wlr_texture *texture = nullptr;
		wlr_box box{}; // output-local logical
	};
	// The panel image for `output` (rebuilt when stale); nullopt unless open there.
	std::optional<Panel> panel(Output &output);

	// Draws the backdrop and the panel on `ctx`'s output when open there.
	void draw(const RenderCtx &ctx) override;
	// Closes the launcher if it is open on `output`.
	void output_gone(Output &output) override;
	void finish() override; // closes, and releases the icons before the renderer goes

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

	CachedTexture<wlr_box> panel_; // the panel image, per scale and layout

	void open(Output &output);
	// One key's symbol: navigation, editing the query, or typing into it.
	// `utf32` is the character it produces (0 for none).
	void key_sym(xkb_keysym_t sym, uint32_t utf32, uint32_t mods);
	bool query_pop_char();
	bool query_pop_word();
	void query_changed();
	// Runs the selected entry and closes. Launched from the overview, its
	// windows open in the overview's target, whatever the overview does
	// before they appear.
	void launch_selected();
	void scroll_to_selection(int rows);
	void move_selection(int rows, int delta);
	int row_at(const PanelLayout &layout, double ox, double oy) const;
	TexturePtr draw_panel(const Output &output, const PanelLayout &layout);
};
