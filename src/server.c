#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>
#include "server.h"

/* Per-surface commit tracking: repaint only the outputs showing the surface. */
struct cv_surface_tracker {
	struct cv_server *server;
	struct wlr_surface *surface;
	struct wl_listener commit, destroy;
};

double now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

void server_spawn(const char *cmd) {
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork failed");
		return;
	}
	if (pid == 0) {
		// Double fork so the command is reparented and never becomes a zombie.
		setsid();
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	waitpid(pid, NULL, 0);
}

static void tracker_handle_commit(struct wl_listener *listener, void *data) {
	struct cv_surface_tracker *t = wl_container_of(listener, t, commit);
	struct wlr_surface *surface = t->surface;
	if (surface->role != NULL && surface->role->name != NULL &&
			strcmp(surface->role->name, "wl_pointer-cursor") == 0) {
		return; // wlr_cursor draws these
	}
	struct wlr_surface *root = wlr_surface_get_root_surface(surface);
	struct wlr_layer_surface_v1 *layer = wlr_layer_surface_v1_try_from_wlr_surface(root);
	struct wlr_xdg_popup *popup = wlr_xdg_popup_try_from_wlr_surface(root);
	while (layer == NULL && popup != NULL && popup->parent != NULL) {
		root = wlr_surface_get_root_surface(popup->parent);
		layer = wlr_layer_surface_v1_try_from_wlr_surface(root);
		popup = wlr_xdg_popup_try_from_wlr_surface(root);
	}
	if (layer != NULL) {
		struct cv_layer *l = layer->data;
		if (l != NULL && l->output != NULL) {
			output_damage(l->output);
		}
		return;
	}
	struct cv_view *view = view_from_surface(root);
	if (view != NULL) {
		view_damage(view);
		return;
	}
	server_damage_all(t->server); // drag icons and other unowned surfaces
}

static void tracker_handle_destroy(struct wl_listener *listener, void *data) {
	struct cv_surface_tracker *t = wl_container_of(listener, t, destroy);
	wl_list_remove(&t->commit.link);
	wl_list_remove(&t->destroy.link);
	free(t);
}

static void handle_new_surface(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, new_surface);
	struct wlr_surface *surface = data;
	struct cv_surface_tracker *t = calloc(1, sizeof(*t));
	if (t == NULL) {
		return;
	}
	t->server = server;
	t->surface = surface;
	t->commit.notify = tracker_handle_commit;
	wl_signal_add(&surface->events.commit, &t->commit);
	t->destroy.notify = tracker_handle_destroy;
	wl_signal_add(&surface->events.destroy, &t->destroy);
}

static void handle_request_activate(struct wl_listener *listener, void *data) {
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	struct cv_view *view = view_from_surface(event->surface);
	// Only honour tokens minted from real input, so apps can't steal focus.
	if (view != NULL && view->mapped && event->token->seat != NULL) {
		view_activate(view);
	}
}

struct cv_inhibitor {
	struct cv_server *server;
	struct wl_listener destroy;
};

static void update_idle_inhibit(struct cv_server *server, int delta) {
	int count = wl_list_length(&server->idle_inhibit->inhibitors) + delta;
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, count > 0);
}

static void handle_inhibitor_destroy(struct wl_listener *listener, void *data) {
	struct cv_inhibitor *inhibitor = wl_container_of(listener, inhibitor, destroy);
	// Still listed while its destroy signal runs.
	update_idle_inhibit(inhibitor->server, -1);
	wl_list_remove(&inhibitor->destroy.link);
	free(inhibitor);
}

static void handle_new_inhibitor(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, new_inhibitor);
	struct wlr_idle_inhibitor_v1 *wlr_inhibitor = data;
	struct cv_inhibitor *inhibitor = calloc(1, sizeof(*inhibitor));
	if (inhibitor == NULL) {
		return;
	}
	inhibitor->server = server;
	inhibitor->destroy.notify = handle_inhibitor_destroy;
	wl_signal_add(&wlr_inhibitor->events.destroy, &inhibitor->destroy);
	update_idle_inhibit(server, 0);
}

static int handle_terminate_signal(int signal, void *data) {
	struct cv_server *server = data;
	wl_display_terminate(server->display);
	return 0;
}

bool server_init(struct cv_server *server, const struct cv_options *options) {
	server->options = *options;
	wl_list_init(&server->outputs);
	wl_list_init(&server->views);
	wl_list_init(&server->desktops);
	wl_list_init(&server->keyboards);

	server->display = wl_display_create();
	server->loop = wl_display_get_event_loop(server->display);
	wl_event_loop_add_signal(server->loop, SIGINT, handle_terminate_signal, server);
	wl_event_loop_add_signal(server->loop, SIGTERM, handle_terminate_signal, server);

	server->backend = wlr_backend_autocreate(server->loop, &server->session);
	if (server->backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return false;
	}
	server->renderer = wlr_renderer_autocreate(server->backend);
	if (server->renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return false;
	}
	wlr_renderer_init_wl_display(server->renderer, server->display);
	server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
	if (server->allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return false;
	}

	struct wl_display *d = server->display;
	server->compositor = wlr_compositor_create(d, 6, server->renderer);
	wlr_subcompositor_create(d);
	wlr_data_device_manager_create(d);
	wlr_primary_selection_v1_device_manager_create(d);
	wlr_data_control_manager_v1_create(d);
	wlr_ext_data_control_manager_v1_create(d, 1);
	wlr_viewporter_create(d);
	wlr_single_pixel_buffer_manager_v1_create(d);
	wlr_fractional_scale_manager_v1_create(d, 1);
	server->presentation = wlr_presentation_create(d, server->backend, 2);
	wlr_export_dmabuf_manager_v1_create(d);
	wlr_screencopy_manager_v1_create(d);
	wlr_relative_pointer_manager_v1_create(d);
	server->pointer_gestures = wlr_pointer_gestures_v1_create(d);
	server->foreign_mgr = wlr_foreign_toplevel_manager_v1_create(d);
	server->idle_notifier = wlr_idle_notifier_v1_create(d);
	server->idle_inhibit = wlr_idle_inhibit_v1_create(d);
	server->new_inhibitor.notify = handle_new_inhibitor;
	wl_signal_add(&server->idle_inhibit->events.new_inhibitor, &server->new_inhibitor);

	struct wlr_xdg_activation_v1 *activation = wlr_xdg_activation_v1_create(d);
	server->request_activate.notify = handle_request_activate;
	wl_signal_add(&activation->events.request_activate, &server->request_activate);

	server->new_surface.notify = handle_new_surface;
	wl_signal_add(&server->compositor->events.new_surface, &server->new_surface);

	server->output_layout = wlr_output_layout_create(d);
	wlr_xdg_output_manager_v1_create(d, server->output_layout);

	server->seat = wlr_seat_create(d, "seat0");
	server->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
	const char *size_env = getenv("XCURSOR_SIZE");
	int cursor_size = size_env != NULL && atoi(size_env) > 0 ? atoi(size_env) : 24;
	server->xcursor_mgr = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"), cursor_size);

	output_init(server);
	view_init(server);
	layer_init(server);
	decoration_init(server);
	input_init(server);
	return true;
}

bool server_start(struct cv_server *server) {
	const char *socket = wl_display_add_socket_auto(server->display);
	if (socket == NULL) {
		wlr_log(WLR_ERROR, "failed to open a Wayland socket");
		return false;
	}
	if (!wlr_backend_start(server->backend)) {
		wlr_log(WLR_ERROR, "failed to start backend");
		return false;
	}
	setenv("WAYLAND_DISPLAY", socket, true);
	unsetenv("DISPLAY");
	wlr_log(WLR_INFO, "Running wayland-canvas on WAYLAND_DISPLAY=%s", socket);
	if (server->options.startup_cmd != NULL) {
		server_spawn(server->options.startup_cmd);
	}
	return true;
}

void server_finish(struct cv_server *server) {
	wl_display_destroy_clients(server->display);
	struct cv_desktop *desktop, *tmp;
	wl_list_for_each_safe(desktop, tmp, &server->desktops, link) {
		desktop_destroy(desktop);
	}
	launcher_finish(server);
	icons_finish();
	render_finish(server);
	// wlroots asserts that destroyed objects have no listeners left.
	input_finish(server);
	decoration_finish(server);
	layer_finish(server);
	view_finish(server);
	output_finish(server);
	wl_list_remove(&server->new_surface.link);
	wl_list_remove(&server->request_activate.link);
	wl_list_remove(&server->new_inhibitor.link);
	wlr_xcursor_manager_destroy(server->xcursor_mgr);
	wlr_cursor_destroy(server->cursor);
	wlr_backend_destroy(server->backend);
	wlr_allocator_destroy(server->allocator);
	wlr_renderer_destroy(server->renderer);
	wl_display_destroy(server->display);
}
