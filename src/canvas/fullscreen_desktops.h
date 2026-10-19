#pragma once

#include <cstdint>
#include <optional>
#include <vector>
#include "canvas/desktop.h"
#include "core/geometry.h"

class Server;
class View;

// A window going fullscreen over other windows goes to a desktop of its own
// beside the one it was on, so that desktop keeps the rest as it was; when it
// leaves fullscreen, by whatever path, it comes back and the desktop goes
// (docs/intents/windows/window-management.md). The window's own state is the
// truth: what it left is reconciled from it once a frame, so no path out of
// fullscreen has to remember this.
class FullscreenDesktops {
public:
	explicit FullscreenDesktops(Server &server);

	// `view` is going fullscreen over `covers`. With other windows standing
	// there it moves, its dialogs with it, to a new desktop beside its own,
	// and the screen showing it goes there; alone there, or not yet mapped,
	// it goes fullscreen in place.
	void enter(View &view, wlr_box covers);
	// The frame of the desktop `view` holds for itself while fullscreen.
	std::optional<wlr_box> area(const View &view) const;
	// A window no longer fullscreen, or gone, comes back with its dialogs,
	// its desktop goes, and a screen still on it returns to the desktop it
	// came from. Run once a frame.
	void reconcile();

private:
	struct Away {
		uint64_t view = 0; // View::session_id
		DesktopId own{}; // the desktop made for it
		std::optional<DesktopId> home; // the desktop it was on, if any
		Camera home_camera; // where the screen showing it looked from
		int dx = 0, dy = 0; // how far its dialogs moved with it
	};

	const Away *find(const View &view) const;
	View *view_of(uint64_t session_id) const;
	void come_back(const Away &away);

	Server &server_;
	std::vector<Away> away_;
};
