#pragma once

#include <memory>
#include <vector>

#include "core/util.h"

class Server;
class View;
struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request;

// ext-image-copy-capture-v1: the copy manager, the output capture sources
// (wlroots' own, fed by the output's committed frames — the renderer already
// answers wlroots' needs_frame) and one capture source per window, backed by
// the offscreen window renderer (render/capture.h). Also owns
// ext-foreign-toplevel-list-v1, whose handles the views carry (view.cpp) so
// capture clients can name a window to capture.
class ImageCapture {
public:
	struct ToplevelSource; // the per-window capture source (image_capture.cpp)

	explicit ImageCapture(Server &server);
	~ImageCapture();
	ImageCapture(const ImageCapture &) = delete;
	ImageCapture &operator=(const ImageCapture &) = delete;

	void init(); // creates the protocol globals
	void finish(); // drops the window sources before the display goes
	void forget(const View &view); // a window went: its casts end

	wlr_ext_foreign_toplevel_list_v1 *list = nullptr; // views' handles go here

private:
	// A client wants to capture the window an ext-foreign-toplevel handle
	// names: make it a source and answer the request.
	void handle_new_request(
		wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *request);

	Server &server_;
	wlr_ext_image_copy_capture_manager_v1 *copy_mgr_ = nullptr;
	wlr_ext_output_image_capture_source_manager_v1 *output_mgr_ = nullptr;
	wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *toplevel_mgr_ = nullptr;
	std::vector<std::unique_ptr<ToplevelSource>> sources_;
	Listener new_request_;
};
