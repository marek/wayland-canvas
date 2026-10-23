#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include "windows/decoration.h"
#include "canvas/desktop.h"
#include "core/geometry.h"
#include "core/util.h"

class LayerSurface;
class Output;
class Server;
class View;

// What an overview press landed on, decided when it was pressed.
struct PressOnEmpty {};
struct PressOnView { View *view; };
struct PressOnDesktop { DesktopId desktop; };
struct PressOnTile { DesktopId desktop; };
struct PressOnAbandon { DesktopId desktop; };
struct PressOnShelf { View *view; };
using PressTarget = std::variant<PressOnEmpty, PressOnView, PressOnDesktop,
	PressOnTile, PressOnAbandon, PressOnShelf>;

// Where a grab began: the output's camera and the cursor's layout position
// then. What the cursor has travelled since is measured against both.
struct GrabAnchor {
	Camera cam;
	double lx, ly;

	// The cursor's travel to layout (x, y), in canvas units at the camera's zoom.
	Point travel(double x, double y) const {
		return {(x - lx) / cam.zoom, (y - ly) / cam.zoom};
	}
	// The straight distance to layout (x, y), in layout px.
	double distance(double x, double y) const { return std::hypot(x - lx, y - ly); }
};

// The compositor-owned pointer interaction in progress, one state per kind
// of grab.
class PointerGrab {
public:
	struct None {};
	struct Move { View *view; Output *output; GrabAnchor anchor; wlr_box start; bool moved; double rel_x, titlebar_offset; };
	struct Resize { View *view; Output *output; GrabAnchor anchor; wlr_box start; uint32_t edges; };
	struct Pan { Output *output; GrabAnchor anchor; };
	struct TitleButton { View *view; Output *output; DecoPart part; };
	struct OverviewPress { Output *output; GrabAnchor anchor; PressTarget target; };
	struct DesktopDrag { Output *output; DesktopId desktop; GrabAnchor anchor; wlr_box start; std::optional<DesktopId> drop; };
	// A plugin's surface on the plane, moved (no edges) or sized by its
	// edges for the client that asked (docs/intents/platform/ipc.md).
	struct SurfaceDrag { LayerSurface *layer; Output *output; GrabAnchor anchor; wlr_box start; uint32_t edges; int min_w, min_h; };
	using State = std::variant<None, Move, Resize, Pan, TitleButton, OverviewPress,
		DesktopDrag, SurfaceDrag>;

	explicit PointerGrab(Server &server);
	PointerGrab(const PointerGrab &) = delete;
	PointerGrab &operator=(const PointerGrab &) = delete;

	bool active() const;
	const State &state() const;
	void begin_move(View &view, Output &output);
	void begin_resize(View &view, Output &output, uint32_t edges);
	void begin_pan(Output &output);
	void begin_title_button(View &view, Output &output, DecoPart part);
	void begin_overview_press(Output &output, PressTarget target);
	// From the press at layout (lx, ly), which the surface's client was
	// sent and answered by asking for this drag.
	void begin_surface_drag(LayerSurface &layer, Output &output, double lx, double ly,
		uint32_t edges, int min_w, int min_h);
	void motion();
	void release(); // a button release: acts (title button, overview click, combine or raise) and ends
	void cancel();
	bool panning(const Output &output) const;
	std::optional<DesktopId> drop_target() const; // the desktop a dragged frame would combine into
	void forget(const View &view); // unmapped or destroyed
	void output_gone(const Output &output); // unplugged
	void forget(const LayerSurface &layer); // unmapped or destroyed

private:
	void move_motion(Move &move);
	void resize_motion(Resize &resize);
	void pan_motion(Pan &pan);
	void title_button_motion(TitleButton &button);
	void overview_press_motion(OverviewPress &press);
	void desktop_drag_motion(DesktopDrag &drag);
	void surface_drag_motion(SurfaceDrag &drag);
	void release_title_button(TitleButton &button);
	// An overview click that never turned into a drag.
	void overview_click(OverviewPress &press);
	// Ends a dragged desktop frame: combine into the drop target, or raise.
	void drop_desktop_drag(DesktopDrag &drag);
	// Ends a grab that held `view`.
	void end_view_grab(View &view);

	Server &server_;
	State state_ = None{};
};
