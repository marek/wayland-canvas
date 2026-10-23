// The notes plugin through the control socket, its storage, and the note's card on a shot.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

// The one output, as the compositor has it.
Output *the_output() {
	return server->outputs.empty() ? nullptr : server->outputs.front().get();
}

} // namespace

void run_msg(std::string_view method, std::string_view params, const char *name) {
	// Meson passes the built CLI's path; a manual run without it falls
	// back to the name on PATH, never to a null format argument.
	const char *bin = std::getenv("WAYLAND_CANVAS_BIN");
	if (bin == nullptr) {
		bin = "wayland-canvas";
	}
	for (const char *suffix : {".out", ".err", ".code"}) {
		unlink(std::format("{}/{}{}", shot_dir, name, suffix).c_str());
	}
	std::string cmd = std::format("\"{}\" msg {}", bin, method);
	if (!params.empty()) {
		cmd += std::format(" '{}'", params);
	}
	cmd += std::format(" > {}/{}.out 2> {}/{}.err; echo $? > {}/{}.code",
		shot_dir, name, shot_dir, name, shot_dir, name);
	const char *const argv[] = {"/bin/sh", "-c", cmd.c_str(),
		static_cast<const char *>(nullptr)};
	Spawned child = spawn(argv);
	if (child.pid > 0) {
		msg_children.push_back(child.pid);
	}
	if (child.out >= 0) {
		close(child.out); // the captures go to files, not this pipe
	}
}

bool msg_done(const char *name) {
	struct stat st{};
	return stat(std::format("{}/{}.code", shot_dir, name).c_str(), &st) == 0;
}

int msg_code(const char *name) {
	return std::atoi(read_file(std::format("{}/{}.code", shot_dir, name)).c_str());
}

void stop_children() {
	for (pid_t pid : msg_children) {
		kill(pid, SIGKILL);
		waitpid(pid, nullptr, 0);
	}
	msg_children.clear();
}

std::string json_string(JsonObject *object, const char *name) {
	if (object == nullptr || !json_object_has_member(object, name)) {
		return {};
	}
	JsonNode *node = json_object_get_member(object, name);
	return node != nullptr && json_node_get_node_type(node) == JSON_NODE_VALUE
			&& json_node_get_value_type(node) == G_TYPE_STRING
		? std::string{json_node_get_string(node)} : std::string{};
}

bool is_note_surface(const LayerSurface &l) {
	return l.mapped && l.wlr->namespace_ != nullptr
		&& std::string_view{l.wlr->namespace_}.starts_with(note_surface_prefix);
}

LayerSurface *note_surface(std::string_view id) {
	for (LayerSurface *l : server->canvas_surfaces.bottom_up()) {
		if (is_note_surface(*l) && (id.empty()
				|| std::string_view{l->wlr->namespace_}.substr(note_surface_prefix.size())
					== id)) {
			return l;
		}
	}
	return nullptr;
}

wlr_box note_box(std::string_view id) {
	LayerSurface *surface = note_surface(id);
	return surface != nullptr && surface->canvas_box.has_value()
		? *surface->canvas_box : wlr_box{};
}

Output *out() {
	return server->outputs.front().get();
}

Point note_local(std::string_view id, double dx, double dy) {
	wlr_box box = note_box(id);
	return out()->canvas_to_local(box.x + (dx < 0 ? box.width + dx : dx),
		box.y + (dy < 0 ? box.height + dy : dy));
}

void travel(double x0, double y0, double x1, double y1) {
	for (int step_n = 1; step_n <= 10; step_n++) {
		move_to(x0 + (x1 - x0) * step_n / 10.0, y0 + (y1 - y0) * step_n / 10.0);
	}
}

double bar_ink_edge(cairo_surface_t *s, const wlr_box &bar, bool light) {
	if (s == nullptr) {
		return 0;
	}
	unsigned char *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	double edge = 0;
	int limit = screen_w / 2 - 250; // short of the centred widgets
	for (int x = 0; bar.x + x < limit; x++) {
		for (int y = 0; y < bar.height; y++) {
			unsigned char *p = data + (bar.y + y) * stride + (bar.x + x) * 4;
			int sum = p[0] + p[1] + p[2];
			if (light ? sum < 550 : sum > 300) { // ink, this mode's way
				edge = bar.x + x;
				break;
			}
		}
	}
	return edge;
}

bool overview_settled(bool active) {
	Output *output = the_output();
	return output != nullptr && output->overview.active() == active
		&& !output->camera.panning() && !output->camera.zooming();
}

} // namespace bar
