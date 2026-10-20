#pragma once

#include <cstdint>
#include "core/wlr.h"
#include "input/key_syms.h"

class Output;
struct RenderCtx;

// A key press as a layer sees it.
struct KeyPress {
	const KeySyms &syms;
	uint32_t utf32; // the character the key produces, 0 for none
	uint32_t mods; // WLR_MODIFIER_* held: shift, ctrl, alt, logo
	bool logo; // the key is a Super key itself
};

// One of the compositor's own UIs, drawn over everything and taking input
// before the overview and the bindings do: the capture UI and the launcher.
// Input keeps them topmost first and every input path asks that list, so
// they cannot disagree on who gets what; the painter draws the same list
// from the bottom up. The session lock is not one: it is the session, and
// comes before every layer.
class UiLayer {
public:
	UiLayer() = default;
	virtual ~UiLayer() = default;
	UiLayer(const UiLayer &) = delete;
	UiLayer &operator=(const UiLayer &) = delete;

	// While true every key press is the layer's and none reaches the
	// overview, the bindings or a client.
	virtual bool owns_keyboard() const = 0;
	virtual void key(const KeyPress &press) = 0;
	// Whether a lone Super tap still runs its action while the layer owns
	// the keyboard.
	virtual bool allows_tap() const = 0;

	// Pointer at output-local logical (ox, oy). A layer that takes the
	// pointer there gets every pointer, touch and pen event at that place,
	// and no window or surface under it does; `pointer_motion` and
	// `pointer_button` tell whether the event is consumed, `pointer_axis`
	// whether the scroll is.
	virtual bool takes_pointer_at(const Output &output, double ox, double oy) const = 0;
	virtual bool pointer_motion(Output &output, double ox, double oy) = 0;
	virtual bool pointer_button(Output &output, double ox, double oy, uint32_t button,
		bool pressed) = 0;
	virtual bool pointer_axis(Output &output, double ox, double oy,
		wl_pointer_axis orientation, double delta) = 0;
	// Whether touchpad and touchscreen swipes and pinches are the layer's
	// and start nothing.
	virtual bool blocks_gestures() const = 0;
	// The cursor image the layer wants while it consumes pointer motion.
	virtual const char *cursor_image() const = 0;

	// Draws the layer on `ctx`'s render target; nothing when it shows
	// nothing there.
	virtual void draw(const RenderCtx &ctx) = 0;
	// `output` is about to be destroyed: whatever the layer shows or does
	// there ends.
	virtual void output_gone(Output &output) = 0;
	// Releases what the renderer and the display own, before they go.
	virtual void finish() = 0;
};
