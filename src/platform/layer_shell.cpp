#include "platform/layer_shell.h"

#include <memory>

#include "core/server.h"
#include "platform/layer.h"
#include "platform/output.h"

void LayerShell::init() {
	wlr_ = wlr_layer_shell_v1_create(server_.display, 4);
	if (wlr_ == nullptr) {
		wlr_log(WLR_ERROR, "failed to create the layer shell");
		return;
	}
	new_surface_.connect<&LayerShell::handle_new_surface>(wlr_->events.new_surface, this);
}

void LayerShell::finish() {
	new_surface_.disconnect();
}

void LayerShell::handle_new_surface(wlr_layer_surface_v1 *ls) {
	Output *output = ls->output != nullptr
		? static_cast<Output *>(ls->output->data)
		: server_.outputs.under_cursor();
	if (output == nullptr) {
		wlr_log(WLR_ERROR, "layer surface with no output to place it on");
		wlr_layer_surface_v1_destroy(ls);
		return;
	}
	ls->output = output->wlr;

	// Head of a layer vector is the topmost (newest) surface.
	auto &vec = output->layers[ls->pending.layer];
	vec.insert(vec.begin(), std::make_unique<LayerSurface>(server_, *output, ls));
}
