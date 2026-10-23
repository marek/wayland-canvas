#pragma once

#include <vector>
#include "render/pass.h"

class Output;
class Server;
class View;

// Draws an output's frame: the canvas, windows, desktop frames and the overview UI.
class Painter {
public:
	explicit Painter(Server &server);
	~Painter();
	Painter(const Painter &) = delete;
	Painter &operator=(const Painter &) = delete;

	void render(Output &output);
	// Draws `local_area` (output-local logical) of what the output shows now
	// into an offscreen ARGB8888 `target` of exactly the area at the output's
	// scale, untransformed — the same pixels an on-screen frame of that area
	// holds. False (logged) when the pass cannot begin or the area is empty.
	// Surfaces drawn get no enter or frame feedback: the output still owns that.
	bool render_area(Output &output, wlr_box local_area, wlr_buffer *target);
	// Draws `view` alone — title bar, border, content, popups, nothing over
	// it, minimized or not — into an offscreen ARGB8888 `target` of
	// view_device_size(view, scale): the frame's corner at (0, 0), zoom 1,
	// `scale` device pixels per logical pixel, over transparent. The same
	// draw_view an on-screen frame uses. False (logged) when the pass cannot
	// begin, the view is not mapped, the session is locked or no output exists.
	bool render_view(View &view, float scale, wlr_buffer *target);

private:
	// The context one render target's frame is drawn with.
	RenderCtx make_ctx(Output &output, wlr_render_pass *pass);
	// Everything from the canvas clear to the drag icon, into ctx's target.
	void draw_scene(const RenderCtx &ctx);
	// make_ctx() for an upright offscreen `target`: no capture UI, no cursors,
	// and a scratch surface list of its own that no frame feedback reads.
	RenderCtx make_capture_ctx(Output &output, wlr_render_pass *pass, wlr_buffer *target);
	void draw_lock(const RenderCtx &ctx);

	Server &server_;
	std::vector<DrawnSurface> drawn_; // surfaces drawn this frame, reused
	std::vector<DrawnSurface> capture_drawn_; // the same for captures, reused
};
