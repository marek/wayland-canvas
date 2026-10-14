/* input.c: seats, devices, pointer/keyboard handling, compositor grabs
 * and key bindings. Gesture recognition lives in gesture.c; the actions
 * it and this file trigger live in canvas.c/view.c. */

#include <libinput.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/util/edges.h>
#include <wlr/xcursor.h>
#include <xkbcommon/xkbcommon.h>
#include "config.h"
#include "server.h"

#define SHELF_MAX 64
#define BINDING_MODS (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | \
	WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)

/* Presses swallowed by a binding or a compositor grab must have their
 * release swallowed too, or clients see stuck keys. */
static bool key_consumed[256];

/* Set of buttons whose press was forwarded to a client. */
static uint32_t pressed_buttons;

/* Logo key pressed alone and nothing else happened yet: releasing it opens
 * the launcher. Any other key, button, scroll or gesture cancels the tap. */
static bool super_tap;

/* Maximized-window drag: where the cursor sat on the title bar when the
 * drag started, so the restore can put it back at the same spot. */
static double move_rel_x, move_tb_off;

/* ---- small helpers ---- */

static void notify_activity(struct cv_server *server) {
	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
}

static struct rect box_rect(struct wlr_box box) {
	return (struct rect){box.x, box.y, box.width, box.height};
}

static bool box_contains(const struct wlr_box *box, double x, double y) {
	return x >= box->x && x < box->x + box->width &&
		y >= box->y && y < box->y + box->height;
}

static uint32_t seat_modifiers(struct cv_server *server) {
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	if (keyboard == NULL) {
		return 0;
	}
	return wlr_keyboard_get_modifiers(keyboard) & BINDING_MODS;
}

/* Hover bookkeeping without extra server state: clear every view's hover
 * except the one under the pointer. Returns true when anything changed. */
static bool set_view_hover(struct cv_server *server, struct cv_view *hover,
		enum cv_deco_part part) {
	bool changed = false;
	struct cv_view *view;
	wl_list_for_each(view, &server->views, link) {
		enum cv_deco_part want = view == hover ? part : CV_DECO_NONE;
		if (view->hover != want) {
			view->hover = want;
			view_damage(view);
			changed = true;
		}
	}
	return changed;
}

void cursor_set_image(struct cv_server *server, const char *name) {
	if (name != NULL && server->cursor_image != NULL &&
			strcmp(server->cursor_image, name) == 0) {
		return;
	}
	server->cursor_image = name;
	if (name != NULL) {
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_mgr, name);
	}
}

bool cursor_canvas_pos(struct cv_server *server, struct cv_output **output,
		double *cx, double *cy) {
	struct cv_output *o = output_under_cursor(server);
	if (o == NULL) {
		return false;
	}
	*output = o;
	output_local_to_canvas(o, server->cursor->x - o->lx,
		server->cursor->y - o->ly, cx, cy);
	return true;
}

/* ---- keyboard devices ---- */

static void update_capabilities(struct cv_server *server) {
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
	struct cv_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
	struct cv_server *server = keyboard->server;
	notify_activity(server);
	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct cv_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
	struct cv_server *server = keyboard->server;
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
	if (wl_list_empty(&server->keyboards)) {
		wlr_seat_set_keyboard(server->seat, NULL);
	} else {
		struct cv_keyboard *next =
			wl_container_of(server->keyboards.next, next, link);
		wlr_seat_set_keyboard(server->seat, next->wlr_keyboard);
	}
	update_capabilities(server);
}

static void keyboard_handle_key(struct wl_listener *listener, void *data);

static void new_keyboard(struct cv_server *server,
		struct wlr_input_device *device, bool has_keymap) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
	struct cv_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	if (keyboard == NULL) {
		return;
	}
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;
	if (!has_keymap) {
		// NULL names: xkbcommon applies its defaults, honouring
		// XKB_DEFAULT_RULES/MODEL/LAYOUT/VARIANT/OPTIONS from the environment.
		struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		if (context != NULL) {
			struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
				XKB_KEYMAP_COMPILE_NO_FLAGS);
			if (keymap != NULL) {
				wlr_keyboard_set_keymap(wlr_keyboard, keymap);
				xkb_keymap_unref(keymap);
			}
			xkb_context_unref(context);
		}
	}
	wlr_keyboard_set_repeat_info(wlr_keyboard, REPEAT_RATE, REPEAT_DELAY);
	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);
	wlr_seat_set_keyboard(server->seat, wlr_keyboard);
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void new_pointer(struct cv_server *server, struct wlr_input_device *device) {
	wlr_cursor_attach_input_device(server->cursor, device);
	if (!wlr_input_device_is_libinput(device)) {
		return;
	}
	struct libinput_device *dev = wlr_libinput_get_device_handle(device);
	if (dev == NULL) {
		return;
	}
	if (libinput_device_config_tap_get_finger_count(dev) > 0) {
		libinput_device_config_tap_set_enabled(dev, TAP_TO_CLICK
			? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
	}
	if (libinput_device_config_scroll_has_natural_scroll(dev)) {
		libinput_device_config_scroll_set_natural_scroll_enabled(dev,
			NATURAL_SCROLL);
	}
	if (libinput_device_config_dwt_is_available(dev)) {
		libinput_device_config_dwt_set_enabled(dev, DISABLE_WHILE_TYPING);
	}
}

static void handle_new_input(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		new_keyboard(server, device, false);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		new_pointer(server, device);
		break;
	default:
		break;
	}
	update_capabilities(server);
}

static void handle_new_virtual_keyboard(struct wl_listener *listener, void *data) {
	struct cv_server *server =
		wl_container_of(listener, server, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *vk = data;
	// A virtual keyboard (wtype) may carry its own keymap; keep it then.
	new_keyboard(server, &vk->keyboard.base, vk->has_keymap);
	update_capabilities(server);
}

/* ---- key bindings ---- */

union key_arg {
	int i;
	const char *cmd;
	struct {
		int dx, dy;
	} dir;
};

struct keybind {
	uint32_t mods;
	xkb_keysym_t keysym;
	void (*fn)(struct cv_server *server, struct cv_output *output,
		const union key_arg *arg);
	union key_arg arg;
};

static void act_spawn(struct cv_server *server, struct cv_output *output,
		const union key_arg *arg) {
	server_spawn(arg->cmd);
}

static void act_launcher(struct cv_server *server, struct cv_output *output,
		const union key_arg *arg) {
	launcher_toggle(server, output);
}

static void act_close(struct cv_server *server, struct cv_output *output,
		const union key_arg *arg) {
	if (server->focused_view != NULL) {
		view_close(server->focused_view);
	}
}

static void act_minimize(struct cv_server *server, struct cv_output *output,
		const union key_arg *arg) {
	if (server->focused_view != NULL && server->focused_view->mapped) {
		view_set_minimized(server->focused_view, true);
	}
}

static void act_restore_minimized(struct cv_server *server,
		struct cv_output *output, const union key_arg *arg) {
	struct cv_view *view = view_last_minimized(server);
	if (view != NULL) {
		view_activate(view);
	}
}

static void act_maximize(struct cv_server *server, struct cv_output *output,
		const union key_arg *arg) {
	struct cv_view *view = server->focused_view;
	if (view != NULL) {
		view_set_maximized(view, !view->maximized);
	}
}

static void act_fullscreen(struct cv_server *server, struct cv_output *output,
		const union key_arg *arg) {
	struct cv_view *view = server->focused_view;
	if (view != NULL) {
		view_set_fullscreen(view, !view->fullscreen);
	}
}

static void act_overview(struct cv_server *server, struct cv_output *output,
		const union key_arg *arg) {
	if (output == NULL) {
		return;
	}
	if (output->mode == CV_MODE_OVERVIEW) {
		overview_exit(output);
	} else {
		overview_enter(output);
	}
}

static void act_goto_dir(struct cv_server *server, struct cv_output *output,
		const union key_arg *arg) {
	if (output != NULL) {
		canvas_goto_direction(output, arg->dir.dx, arg->dir.dy);
	}
}

/* Move the focused window onto the nearest desktop in the given direction
 * (or by one screen when there is none) and follow it. */
static void act_move_dir(struct cv_server *server, struct cv_output *output,
		const union key_arg *arg) {
	struct cv_view *view = server->focused_view;
	if (view == NULL || !view->mapped || output == NULL) {
		return;
	}
	struct cv_desktop *cur = desktop_of_view(server, view);
	double ox, oy;
	if (cur != NULL) {
		ox = cur->box.x + cur->box.width / 2.0;
		oy = cur->box.y + cur->box.height / 2.0;
	} else {
		struct rect vp = output_viewport(output);
		ox = rect_cx(vp);
		oy = rect_cy(vp);
	}

	size_t n = 0;
	struct cv_desktop *d;
	wl_list_for_each(d, &server->desktops, link) {
		n++;
	}
	struct cv_desktop *target = NULL;
	if (n > 0) {
		struct rect *cands = malloc(n * sizeof(*cands));
		struct cv_desktop **ids = malloc(n * sizeof(*ids));
		if (cands != NULL && ids != NULL) {
			size_t i = 0, cur_idx = n;
			wl_list_for_each(d, &server->desktops, link) {
				ids[i] = d;
				cands[i] = box_rect(d->box);
				if (d == cur) {
					cur_idx = i;
				}
				i++;
			}
			int pick = nav_pick_direction(ox, oy, arg->dir.dx, arg->dir.dy,
				cands, n, cur_idx == n ? -1 : (int)cur_idx, HOP_CONE_DEG);
			if (pick >= 0) {
				target = ids[pick];
			}
		}
		free(cands);
		free(ids);
	}

	if (target != NULL) {
		double tx = target->box.x + target->box.width / 2.0;
		double ty = target->box.y + target->box.height / 2.0;
		view_move(view, (int)round(view->x + tx - ox),
			(int)round(view->y + ty - oy));
		canvas_goto_box(output, target->box);
	} else {
		view_move(view, (int)round(view->x + arg->dir.dx * output->width),
			(int)round(view->y + arg->dir.dy * output->height));
		canvas_reveal_view(server, view);
	}
}

static void act_pan(struct cv_server *server, struct cv_output *output,
		const union key_arg *arg) {
	if (output != NULL) {
		canvas_pan(output, arg->dir.dx * output->width / 3.0,
			arg->dir.dy * output->height / 3.0);
	}
}

static void act_pin(struct cv_server *server, struct cv_output *output,
		const union key_arg *arg) {
	if (output != NULL) {
		canvas_pin_desktop(output);
	}
}

static void act_abandon_current(struct cv_server *server,
		struct cv_output *output, const union key_arg *arg) {
	if (output != NULL) {
		overview_abandon(output, desktop_showing(output));
	}
}

static void act_tile_current(struct cv_server *server, struct cv_output *output,
		const union key_arg *arg) {
	if (output == NULL) {
		return;
	}
	struct cv_desktop *d = desktop_showing(output);
	struct wlr_box box;
	if (d != NULL) {
		box = d->box;
	} else {
		struct rect vp = output_viewport(output);
		box.x = (int)round(vp.x);
		box.y = (int)round(vp.y);
		box.width = (int)round(vp.w);
		box.height = (int)round(vp.h);
	}
	canvas_tile(server, box, output);
}

static void act_goto_desktop(struct cv_server *server, struct cv_output *output,
		const union key_arg *arg) {
	if (output == NULL) {
		return;
	}
	struct cv_desktop *d = desktop_by_id(server, arg->i);
	if (d != NULL) {
		canvas_goto_box(output, d->box);
	}
}

static void act_quit(struct cv_server *server, struct cv_output *output,
		const union key_arg *arg) {
	wl_display_terminate(server->display);
}

static void act_vt(struct cv_server *server, struct cv_output *output,
		const union key_arg *arg) {
	if (server->session != NULL) {
		wlr_session_change_vt(server->session, (unsigned)arg->i);
	}
}

/* dwl-style binding table. Keysyms are matched case-insensitively so
 * Shift+letter bindings still hit. */
static const struct keybind keys[] = {
	{WLR_MODIFIER_LOGO, XKB_KEY_Return, act_spawn, {.cmd = TERMINAL_CMD}},
	{WLR_MODIFIER_LOGO, XKB_KEY_space, act_launcher, {0}},
	{WLR_MODIFIER_LOGO, XKB_KEY_q, act_close, {0}},
	{WLR_MODIFIER_ALT, XKB_KEY_F4, act_close, {0}},
	{WLR_MODIFIER_LOGO, XKB_KEY_m, act_minimize, {0}},
	{WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_m, act_restore_minimized, {0}},
	{WLR_MODIFIER_LOGO, XKB_KEY_Up, act_maximize, {0}},
	{WLR_MODIFIER_LOGO, XKB_KEY_f, act_fullscreen, {0}},
	{WLR_MODIFIER_LOGO, XKB_KEY_Tab, act_overview, {0}},
	{WLR_MODIFIER_LOGO, XKB_KEY_w, act_overview, {0}},
	{WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL, XKB_KEY_Left, act_goto_dir, {.dir = {-1, 0}}},
	{WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL, XKB_KEY_Right, act_goto_dir, {.dir = {1, 0}}},
	{WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL, XKB_KEY_Up, act_goto_dir, {.dir = {0, -1}}},
	{WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL, XKB_KEY_Down, act_goto_dir, {.dir = {0, 1}}},
	{WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT, XKB_KEY_Left, act_move_dir, {.dir = {-1, 0}}},
	{WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT, XKB_KEY_Right, act_move_dir, {.dir = {1, 0}}},
	{WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT, XKB_KEY_Up, act_move_dir, {.dir = {0, -1}}},
	{WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT, XKB_KEY_Down, act_move_dir, {.dir = {0, 1}}},
	{WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT, XKB_KEY_Left, act_pan, {.dir = {-1, 0}}},
	{WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT, XKB_KEY_Right, act_pan, {.dir = {1, 0}}},
	{WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT, XKB_KEY_Up, act_pan, {.dir = {0, -1}}},
	{WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT, XKB_KEY_Down, act_pan, {.dir = {0, 1}}},
	{WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL, XKB_KEY_d, act_pin, {0}},
	{WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL, XKB_KEY_F4, act_abandon_current, {0}},
	{WLR_MODIFIER_LOGO, XKB_KEY_t, act_tile_current, {0}},
	{WLR_MODIFIER_LOGO, XKB_KEY_1, act_goto_desktop, {.i = 1}},
	{WLR_MODIFIER_LOGO, XKB_KEY_2, act_goto_desktop, {.i = 2}},
	{WLR_MODIFIER_LOGO, XKB_KEY_3, act_goto_desktop, {.i = 3}},
	{WLR_MODIFIER_LOGO, XKB_KEY_4, act_goto_desktop, {.i = 4}},
	{WLR_MODIFIER_LOGO, XKB_KEY_5, act_goto_desktop, {.i = 5}},
	{WLR_MODIFIER_LOGO, XKB_KEY_6, act_goto_desktop, {.i = 6}},
	{WLR_MODIFIER_LOGO, XKB_KEY_7, act_goto_desktop, {.i = 7}},
	{WLR_MODIFIER_LOGO, XKB_KEY_8, act_goto_desktop, {.i = 8}},
	{WLR_MODIFIER_LOGO, XKB_KEY_9, act_goto_desktop, {.i = 9}},
	{WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_e, act_quit, {0}},
	{WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, XKB_KEY_BackSpace, act_quit, {0}},
	{WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, XKB_KEY_F1, act_vt, {.i = 1}},
	{WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, XKB_KEY_F2, act_vt, {.i = 2}},
	{WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, XKB_KEY_F3, act_vt, {.i = 3}},
	{WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, XKB_KEY_F4, act_vt, {.i = 4}},
	{WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, XKB_KEY_F5, act_vt, {.i = 5}},
	{WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, XKB_KEY_F6, act_vt, {.i = 6}},
	{WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, XKB_KEY_F7, act_vt, {.i = 7}},
	{WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, XKB_KEY_F8, act_vt, {.i = 8}},
	{WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, XKB_KEY_F9, act_vt, {.i = 9}},
	{WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, XKB_KEY_F10, act_vt, {.i = 10}},
	{WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, XKB_KEY_F11, act_vt, {.i = 11}},
	{WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, XKB_KEY_F12, act_vt, {.i = 12}},
};

static bool handle_global_keys(struct cv_server *server,
		struct cv_output *output, const xkb_keysym_t *syms, int nsyms,
		uint32_t mods) {
	for (int i = 0; i < nsyms; i++) {
		xkb_keysym_t sym = xkb_keysym_to_lower(syms[i]);
		for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
			if (keys[k].keysym == sym && keys[k].mods == mods) {
				keys[k].fn(server, output, &keys[k].arg);
				return true;
			}
		}
	}
	return false;
}

static void overview_new_desktop_at(struct cv_server *server,
		struct cv_output *output) {
	if (output->target_desktop == NULL) {
		// Free spot: plant the new desktop on the target's centre.
		overview_new_desktop(output,
			output->target.x + output->target.width / 2.0,
			output->target.y + output->target.height / 2.0);
		return;
	}
	struct cv_output *under;
	double cx, cy;
	if (cursor_canvas_pos(server, &under, &cx, &cy)) {
		overview_new_desktop(output, cx, cy);
	} else {
		overview_new_desktop(output,
			output->target.x + output->target.width / 2.0,
			output->target.y + output->target.height / 2.0);
	}
}

/* Keys while the output under the cursor is in the overview. Every press
 * is compositor-owned; callers swallow unmatched ones too. */
static void handle_overview_keys(struct cv_server *server,
		struct cv_output *output, const xkb_keysym_t *syms, int nsyms,
		uint32_t mods) {
	for (int i = 0; i < nsyms; i++) {
		xkb_keysym_t sym = xkb_keysym_to_lower(syms[i]);
		switch (sym) {
		case XKB_KEY_Escape:
			overview_cancel(output);
			return;
		case XKB_KEY_Return:
		case XKB_KEY_KP_Enter:
			overview_exit(output);
			return;
		case XKB_KEY_Left:
			overview_hop(output, -1, 0);
			return;
		case XKB_KEY_Right:
			overview_hop(output, 1, 0);
			return;
		case XKB_KEY_Up:
			overview_hop(output, 0, -1);
			return;
		case XKB_KEY_Down:
			overview_hop(output, 0, 1);
			return;
		case XKB_KEY_Delete:
		case XKB_KEY_BackSpace:
			if (output->target_desktop != NULL) {
				overview_abandon(output, output->target_desktop);
			}
			return;
		case XKB_KEY_t:
			overview_tile(output, output->target_desktop);
			return;
		case XKB_KEY_m:
			overview_combine(output, output->target_desktop);
			return;
		case XKB_KEY_n:
			overview_new_desktop_at(server, output);
			return;
		case XKB_KEY_plus:
		case XKB_KEY_equal:
			overview_zoom_by(output, 1.25, output->width / 2.0,
				output->height / 2.0);
			return;
		case XKB_KEY_minus:
			overview_zoom_by(output, 0.8, output->width / 2.0,
				output->height / 2.0);
			return;
		case XKB_KEY_Tab:
		case XKB_KEY_w:
			if (mods & WLR_MODIFIER_LOGO) {
				overview_exit(output);
			}
			return;
		default:
			break;
		}
	}
}

static bool is_logo_key(const xkb_keysym_t *syms, int nsyms) {
	for (int i = 0; i < nsyms; i++) {
		if (syms[i] == XKB_KEY_Super_L || syms[i] == XKB_KEY_Super_R) {
			return true;
		}
	}
	return false;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct cv_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct cv_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;
	notify_activity(server);

	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state,
		keycode, &syms);
	uint32_t all_mods = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
	uint32_t mods = all_mods & BINDING_MODS;
	bool logo = is_logo_key(syms, nsyms);

	bool handled = false;
	bool toggle_launcher = false;
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		// A logo key pressed on its own may become a tap (see release).
		super_tap = logo && keyboard->wlr_keyboard->num_keycodes == 1;
		struct cv_output *output = output_under_cursor(server);
		if (launcher_is_open(server)) {
			// The launcher owns the keyboard while open.
			if (nsyms > 0 && !logo) {
				if (xkb_keysym_to_lower(syms[0]) == XKB_KEY_space &&
						(mods & WLR_MODIFIER_LOGO)) {
					launcher_close(server);
				} else {
					launcher_key(server, syms[0], xkb_state_key_get_utf32(
						keyboard->wlr_keyboard->xkb_state, keycode), all_mods);
				}
			}
			handled = true;
		} else if (output != NULL && output->mode == CV_MODE_OVERVIEW) {
			handle_overview_keys(server, output, syms, nsyms, mods);
			handled = true;
		} else if (nsyms > 0) {
			handled = handle_global_keys(server, output, syms, nsyms, mods);
		}
		if (handled && event->keycode < 256) {
			key_consumed[event->keycode] = true;
		}
	} else {
		// Logo pressed and released with nothing in between, like Windows.
		toggle_launcher = super_tap && logo;
		super_tap = false;
		if (event->keycode < 256 && key_consumed[event->keycode]) {
			// Release of a press the client never saw.
			key_consumed[event->keycode] = false;
			handled = true;
		}
	}

	if (!handled) {
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode,
			event->state);
	}
	if (toggle_launcher) {
		launcher_toggle(server, output_under_cursor(server));
	}
}

/* ---- pointer hit testing ---- */

struct pointer_hit {
	struct wlr_surface *surface; /* client surface under the pointer */
	struct cv_view *view;        /* view frame under the pointer (or NULL) */
	struct cv_layer *layer;      /* layer surface's owner (or NULL) */
	double sx, sy;               /* surface-local coordinates */
	enum cv_deco_part part;      /* decoration part when surface == NULL */
	uint32_t edges;              /* resize edges for CV_DECO_RESIZE */
};

static void pointer_hit_test(struct cv_server *server, struct cv_output *output,
		struct pointer_hit *hit) {
	memset(hit, 0, sizeof(*hit));
	hit->part = CV_DECO_NONE;
	double ox = server->cursor->x - output->lx;
	double oy = server->cursor->y - output->ly;

	struct wlr_surface *surface;
	struct cv_layer *layer;
	double sx, sy;
	for (int l = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
			l >= ZWLR_LAYER_SHELL_V1_LAYER_TOP; l--) {
		surface = layer_surface_at(output, l, ox, oy, &sx, &sy, &layer);
		if (surface != NULL) {
			hit->surface = surface;
			hit->layer = layer;
			hit->sx = sx;
			hit->sy = sy;
			return;
		}
	}

	double cx, cy;
	struct camera cam = output_camera(output);
	output_local_to_canvas(output, ox, oy, &cx, &cy);
	struct cv_view *view = view_at(server, cx, cy, cam.zoom, &surface,
		&sx, &sy, &hit->part, &hit->edges);
	if (view != NULL) {
		hit->view = view;
		hit->surface = surface;
		hit->sx = sx;
		hit->sy = sy;
		return;
	}

	for (int l = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
			l >= ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND; l--) {
		surface = layer_surface_at(output, l, ox, oy, &sx, &sy, &layer);
		if (surface != NULL) {
			hit->surface = surface;
			hit->layer = layer;
			hit->sx = sx;
			hit->sy = sy;
			return;
		}
	}
}

static uint32_t quadrant_edges(struct cv_view *view, double cx, double cy) {
	struct wlr_box frame = view_frame_box(view);
	uint32_t edges = 0;
	edges |= (cx - frame.x) * 2 < frame.width ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
	edges |= (cy - frame.y) * 2 < frame.height ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
	return edges;
}

/* ---- compositor grabs ---- */

static void begin_move(struct cv_server *server, struct cv_view *view,
		struct cv_output *output) {
	server->cursor_mode = CV_CURSOR_MOVE;
	server->grab_view = view;
	server->grab_output = output;
	server->grab_cam = output_camera(output);
	server->grab_box = view_content_box(view);
	server->grab_lx = server->cursor->x;
	server->grab_ly = server->cursor->y;
	output_local_to_canvas(output, server->cursor->x - output->lx,
		server->cursor->y - output->ly, &server->grab_cx, &server->grab_cy);
	server->grab_moved = false;
	if (view->maximized) {
		// Remember where on the title bar the grab started so that the
		// drag-restore below can keep the cursor at the same spot.
		struct wlr_box frame = view_frame_box(view);
		move_rel_x = frame.width > 0
			? clampd((server->grab_cx - frame.x) / frame.width, 0.0, 1.0) : 0.5;
		move_tb_off = clampd(server->grab_cy - frame.y, 0.0,
			(double)deco_titlebar(view));
	}
}

static void begin_resize(struct cv_server *server, struct cv_view *view,
		struct cv_output *output, uint32_t edges) {
	if (edges == 0) {
		edges = WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT;
	}
	server->cursor_mode = CV_CURSOR_RESIZE;
	server->grab_view = view;
	server->grab_output = output;
	server->grab_cam = output_camera(output);
	server->grab_box = view_content_box(view);
	server->grab_lx = server->cursor->x;
	server->grab_ly = server->cursor->y;
	server->grab_edges = edges;
	server->grab_moved = false;
	// view.c anchors the moving (left/top) edges on commit while resizing.
	view->resize_edges = edges;
	view->resize_end_serial = 0;
	view->resize_start = server->grab_box;
	if (view->toplevel != NULL) {
		wlr_xdg_toplevel_set_resizing(view->toplevel, true);
	}
	cursor_set_image(server, wlr_xcursor_get_resize_name(edges));
}

static void begin_pan(struct cv_server *server, struct cv_output *output) {
	server->cursor_mode = CV_CURSOR_PAN;
	server->grab_output = output;
	server->grab_cam = output_camera(output);
	server->grab_lx = server->cursor->x;
	server->grab_ly = server->cursor->y;
	server->grab_moved = false;
}

static void begin_deco_button(struct cv_server *server, struct cv_view *view,
		struct cv_output *output, enum cv_deco_part part) {
	server->cursor_mode = CV_CURSOR_DECO_BUTTON;
	server->grab_view = view;
	server->grab_output = output;
	server->grab_part = part;
	server->grab_lx = server->cursor->x;
	server->grab_ly = server->cursor->y;
	server->grab_moved = false;
	view->pressed = part;
	view_damage(view);
}

static void process_move_grab(struct cv_server *server) {
	struct cv_view *view = server->grab_view;
	struct cv_output *output = server->grab_output;
	if (view == NULL || output == NULL) {
		input_cancel_grab(server);
		return;
	}
	double dist = hypot(server->cursor->x - server->grab_lx,
		server->cursor->y - server->grab_ly);
	if (view->maximized) {
		if (dist < DRAG_THRESHOLD) {
			return;
		}
		// Restore under the cursor: same relative x across the title bar,
		// same height into it.
		view_set_maximized(view, false);
		struct wlr_box restored = view->saved;
		if (restored.width <= 0 || restored.height <= 0) {
			restored = view_content_box(view);
		}
		int border = deco_border(view);
		int title = deco_titlebar(view);
		double ccx, ccy;
		output_local_to_canvas(output, server->cursor->x - output->lx,
			server->cursor->y - output->ly, &ccx, &ccy);
		double frame_w = restored.width + 2.0 * border;
		int frame_left = (int)round(ccx - move_rel_x * frame_w);
		int frame_top = (int)round(ccy - move_tb_off);
		view_move(view, frame_left + border, frame_top + title + border);
		server->grab_box.x = view->x;
		server->grab_box.y = view->y;
		server->grab_box.width = restored.width;
		server->grab_box.height = restored.height;
		server->grab_lx = server->cursor->x;
		server->grab_ly = server->cursor->y;
		server->grab_moved = true;
		return;
	}
	if (dist >= DRAG_THRESHOLD) {
		server->grab_moved = true;
	}
	double dx = (server->cursor->x - server->grab_lx) / server->grab_cam.zoom;
	double dy = (server->cursor->y - server->grab_ly) / server->grab_cam.zoom;
	view_move(view, (int)round(server->grab_box.x + dx),
		(int)round(server->grab_box.y + dy));
}

static void process_resize_grab(struct cv_server *server) {
	struct cv_view *view = server->grab_view;
	struct cv_output *output = server->grab_output;
	if (view == NULL || view->toplevel == NULL || output == NULL) {
		return;
	}
	double dx = (server->cursor->x - server->grab_lx) / server->grab_cam.zoom;
	double dy = (server->cursor->y - server->grab_ly) / server->grab_cam.zoom;
	double x1 = server->grab_box.x;
	double y1 = server->grab_box.y;
	double x2 = x1 + server->grab_box.width;
	double y2 = y1 + server->grab_box.height;
	if (server->grab_edges & WLR_EDGE_LEFT) {
		x1 += dx;
	}
	if (server->grab_edges & WLR_EDGE_RIGHT) {
		x2 += dx;
	}
	if (server->grab_edges & WLR_EDGE_TOP) {
		y1 += dy;
	}
	if (server->grab_edges & WLR_EDGE_BOTTOM) {
		y2 += dy;
	}

	struct wlr_xdg_toplevel *toplevel = view->toplevel;
	double min_w = 100, min_h = 60;
	if (toplevel->current.min_width > min_w) {
		min_w = toplevel->current.min_width;
	}
	if (toplevel->current.min_height > min_h) {
		min_h = toplevel->current.min_height;
	}
	double max_w = toplevel->current.max_width > 0
		? toplevel->current.max_width : 1e9;
	double max_h = toplevel->current.max_height > 0
		? toplevel->current.max_height : 1e9;
	double w = clampd(x2 - x1, min_w, max_w);
	double h = clampd(y2 - y1, min_h, max_h);
	wlr_xdg_toplevel_set_size(toplevel, (int32_t)round(w), (int32_t)round(h));
}

static void process_pan_grab(struct cv_server *server) {
	struct cv_output *output = server->grab_output;
	if (output == NULL) {
		return;
	}
	// Content follows the cursor.
	struct camera cam = server->grab_cam;
	cam.cx -= (server->cursor->x - server->grab_lx) / cam.zoom;
	cam.cy -= (server->cursor->y - server->grab_ly) / cam.zoom;
	camera_set(output, cam);
}

static void process_deco_button_grab(struct cv_server *server) {
	struct cv_view *view = server->grab_view;
	if (view == NULL) {
		return;
	}
	struct cv_output *output;
	double cx, cy;
	if (!cursor_canvas_pos(server, &output, &cx, &cy)) {
		return;
	}
	uint32_t edges = 0;
	enum cv_deco_part part = deco_hit(view, cx, cy, output_camera(output).zoom,
		&edges);
	enum cv_deco_part pressed = part == server->grab_part
		? server->grab_part : CV_DECO_NONE;
	if (view->pressed != pressed) {
		view->pressed = pressed;
		view_damage(view);
	}
}

static void process_desktop_drag_grab(struct cv_server *server) {
	struct cv_desktop *desktop = server->grab_desktop;
	if (desktop == NULL) {
		return;
	}
	double dx = (server->cursor->x - server->grab_lx) / server->grab_cam.zoom;
	double dy = (server->cursor->y - server->grab_ly) / server->grab_cam.zoom;
	desktop->box.x = (int)round(server->grab_box.x + dx);
	desktop->box.y = (int)round(server->grab_box.y + dy);

	// Combine when another desktop is overlapped by at least half of the
	// dragged frame; largest overlap wins.
	struct rect box = box_rect(desktop->box);
	double area = desktop->box.width * (double)desktop->box.height;
	struct cv_desktop *drop = NULL, *d;
	double best = 0;
	wl_list_for_each(d, &server->desktops, link) {
		if (d == desktop) {
			continue;
		}
		double overlap = rect_overlap_area(box, box_rect(d->box));
		if (overlap >= COMBINE_OVERLAP * area && overlap > best) {
			best = overlap;
			drop = d;
		}
	}
	server->drop_desktop = drop;
	server_damage_all(server);
}

static void process_overview_press_grab(struct cv_server *server) {
	double dist = hypot(server->cursor->x - server->grab_lx,
		server->cursor->y - server->grab_ly);
	if (dist < DRAG_THRESHOLD) {
		return;
	}
	server->grab_moved = true;
	switch (server->grab_hit) {
	case CV_HIT_DESKTOP:
		if (server->grab_desktop == NULL) {
			break;
		}
		server->cursor_mode = CV_CURSOR_DESKTOP_DRAG;
		server->grab_box = server->grab_desktop->box;
		server->grab_lx = server->cursor->x;
		server->grab_ly = server->cursor->y;
		server->drop_desktop = NULL;
		break;
	case CV_HIT_VIEW:
		if (server->grab_view != NULL && server->grab_output != NULL) {
			begin_move(server, server->grab_view, server->grab_output);
		}
		break;
	case CV_HIT_EMPTY:
		if (server->grab_output != NULL) {
			begin_pan(server, server->grab_output);
		}
		break;
	default:
		// Tab buttons and shelf items stay clicks.
		break;
	}
}

static void process_grab_motion(struct cv_server *server) {
	// Grabs begun outside input.c (xdg request_move/request_resize from CSD
	// clients) fill the grab fields but not grab_cam; capture it on the
	// first motion. cancel_grab zeroes it, so it is never stale.
	if (server->grab_output != NULL && server->grab_cam.zoom <= 0) {
		server->grab_cam = output_camera(server->grab_output);
	}
	switch (server->cursor_mode) {
	case CV_CURSOR_MOVE:
		process_move_grab(server);
		break;
	case CV_CURSOR_RESIZE:
		process_resize_grab(server);
		break;
	case CV_CURSOR_PAN:
		process_pan_grab(server);
		break;
	case CV_CURSOR_DECO_BUTTON:
		process_deco_button_grab(server);
		break;
	case CV_CURSOR_OVERVIEW_PRESS:
		process_overview_press_grab(server);
		break;
	case CV_CURSOR_DESKTOP_DRAG:
		process_desktop_drag_grab(server);
		break;
	default:
		break;
	}
}

void input_cancel_grab(struct cv_server *server) {
	if (server->cursor_mode == CV_CURSOR_PASSTHROUGH) {
		return;
	}
	struct cv_view *view = server->grab_view;
	if (view != NULL) {
		view->pressed = CV_DECO_NONE;
		if (view->resize_edges != 0 && view->resize_end_serial == 0) {
			// view.c keeps anchoring until the client acks this configure.
			view->resize_end_serial =
				wlr_xdg_toplevel_set_resizing(view->toplevel, false);
		}
		view_damage(view);
	}
	server->cursor_mode = CV_CURSOR_PASSTHROUGH;
	server->grab_view = NULL;
	server->grab_desktop = NULL;
	server->drop_desktop = NULL;
	server->grab_output = NULL;
	server->grab_moved = false;
	server->grab_cam.zoom = 0; // invalidated; owner re-fills on next grab
	server_damage_all(server);
	cursor_set_image(server, "default");
}

/* ---- pointer motion ---- */

static void process_cursor_motion(struct cv_server *server, uint32_t time) {
	if (server->cursor_mode != CV_CURSOR_PASSTHROUGH) {
		process_grab_motion(server);
		return;
	}

	struct cv_output *output =
		output_at(server, server->cursor->x, server->cursor->y);
	if (output == NULL) {
		wlr_seat_pointer_notify_clear_focus(server->seat);
		cursor_set_image(server, "default");
		return;
	}
	if (launcher_pointer_motion(server, output, server->cursor->x - output->lx,
			server->cursor->y - output->ly)) {
		wlr_seat_pointer_notify_clear_focus(server->seat);
		set_view_hover(server, NULL, CV_DECO_NONE);
		cursor_set_image(server, "default");
		return;
	}

	if (!output_interactive(output)) {
		// Overview / animating: the compositor owns the pointer. Keep the
		// title bar hover state of the view under the cursor alive so the
		// buttons light up, but no client sees any event.
		wlr_seat_pointer_notify_clear_focus(server->seat);
		struct camera cam = output_camera(output);
		double cx, cy;
		output_local_to_canvas(output, server->cursor->x - output->lx,
			server->cursor->y - output->ly, &cx, &cy);
		struct wlr_surface *surface;
		double sx, sy;
		enum cv_deco_part part = CV_DECO_NONE;
		uint32_t edges = 0;
		struct cv_view *view = view_at(server, cx, cy, cam.zoom, &surface,
			&sx, &sy, &part, &edges);
		if (set_view_hover(server, surface != NULL ? NULL : view, part)) {
			output_damage(output);
		}
		cursor_set_image(server, "default");
		return;
	}

	struct pointer_hit hit;
	pointer_hit_test(server, output, &hit);
	if (hit.surface != NULL) {
		set_view_hover(server, NULL, CV_DECO_NONE);
		wlr_seat_pointer_notify_enter(server->seat, hit.surface, hit.sx, hit.sy);
		wlr_seat_pointer_notify_motion(server->seat, time, hit.sx, hit.sy);
		// The client owns the cursor image while it has pointer focus.
	} else {
		wlr_seat_pointer_notify_clear_focus(server->seat);
		if (hit.view != NULL) {
			set_view_hover(server, hit.view, hit.part);
			cursor_set_image(server, hit.part == CV_DECO_RESIZE
				? wlr_xcursor_get_resize_name(hit.edges) : "default");
		} else {
			set_view_hover(server, NULL, CV_DECO_NONE);
			cursor_set_image(server, "default");
		}
	}
}

void cursor_rebase(struct cv_server *server) {
	if (server->cursor_mode != CV_CURSOR_PASSTHROUGH) {
		return;
	}
	process_cursor_motion(server, (uint32_t)now_ms());
}

/* ---- overview pointer interaction ---- */

/* Records what an overview press landed on, following the render order:
 * shelf item, tab buttons, tab body, window, desktop interior, empty. */
static void record_overview_hit(struct cv_server *server,
		struct cv_output *output, double ox, double oy,
		double cx, double cy, double zoom) {
	struct cv_view *shelf_views[SHELF_MAX];
	struct wlr_box shelf_boxes[SHELF_MAX];
	size_t count = overview_shelf_layout(output, shelf_views, shelf_boxes,
		SHELF_MAX);
	for (size_t i = 0; i < count; i++) {
		if (box_contains(&shelf_boxes[i], ox, oy)) {
			server->grab_hit = CV_HIT_SHELF;
			server->grab_view = shelf_views[i];
			return;
		}
	}

	struct cv_desktop *desktop;
	wl_list_for_each(desktop, &server->desktops, link) {
		struct wlr_box tab, tile, abandon;
		if (!overview_tab_layout(output, desktop, &tab, &tile, &abandon)) {
			continue;
		}
		if (box_contains(&tile, ox, oy)) {
			server->grab_hit = CV_HIT_TAB_TILE;
			server->grab_desktop = desktop;
			return;
		}
		if (box_contains(&abandon, ox, oy)) {
			server->grab_hit = CV_HIT_TAB_ABANDON;
			server->grab_desktop = desktop;
			return;
		}
		if (box_contains(&tab, ox, oy)) {
			server->grab_hit = CV_HIT_DESKTOP;
			server->grab_desktop = desktop;
			return;
		}
	}

	struct wlr_surface *surface = NULL;
	double sx = 0, sy = 0;
	enum cv_deco_part part = CV_DECO_NONE;
	uint32_t edges = 0;
	struct cv_view *view = view_at(server, cx, cy, zoom, &surface, &sx, &sy,
		&part, &edges);
	if (view != NULL) {
		server->grab_hit = CV_HIT_VIEW;
		server->grab_view = view;
		return;
	}

	desktop = desktop_at(server, cx, cy);
	if (desktop != NULL) {
		server->grab_hit = CV_HIT_DESKTOP;
		server->grab_desktop = desktop;
		return;
	}
	server->grab_hit = CV_HIT_EMPTY;
}

/* Target the desktop a view belongs to, or a screen-sized free spot
 * centred on the view, and leave the overview onto it. */
static void overview_target_view(struct cv_server *server,
		struct cv_output *output, struct cv_view *view) {
	struct cv_desktop *desktop = desktop_of_view(server, view);
	if (desktop != NULL) {
		overview_set_target(output, desktop->box, desktop, true);
	} else {
		struct wlr_box frame = view_frame_box(view);
		struct wlr_box box = {
			.x = frame.x + frame.width / 2 - output->width / 2,
			.y = frame.y + frame.height / 2 - output->height / 2,
			.width = output->width,
			.height = output->height,
		};
		overview_set_target(output, box, NULL, true);
	}
}

static void overview_click(struct cv_server *server) {
	struct cv_output *output = server->grab_output;
	if (output == NULL) {
		return;
	}
	struct cv_view *view = server->grab_view;
	struct cv_desktop *desktop = server->grab_desktop;
	switch (server->grab_hit) {
	case CV_HIT_SHELF:
		if (view == NULL) {
			break;
		}
		view_set_minimized(view, false);
		view_focus(view);
		overview_target_view(server, output, view);
		overview_exit(output);
		break;
	case CV_HIT_TAB_TILE:
		if (desktop != NULL) {
			overview_tile(output, desktop);
		}
		break;
	case CV_HIT_TAB_ABANDON:
		if (desktop != NULL) {
			overview_abandon(output, desktop);
		}
		break;
	case CV_HIT_DESKTOP:
		if (desktop == NULL) {
			break;
		}
		overview_set_target(output, desktop->box, desktop, true);
		overview_exit(output);
		break;
	case CV_HIT_VIEW:
		if (view == NULL) {
			break;
		}
		view_focus(view);
		overview_target_view(server, output, view);
		overview_exit(output);
		break;
	case CV_HIT_EMPTY: {
		struct cv_output *under;
		double cx, cy;
		if (cursor_canvas_pos(server, &under, &cx, &cy)) {
			overview_target_point(output, cx, cy);
			overview_exit(output);
		}
		break;
	}
	default:
		break;
	}
}

static void overview_press(struct cv_server *server, struct cv_output *output,
		uint32_t button) {
	struct camera cam = output_camera(output);
	double ox = server->cursor->x - output->lx;
	double oy = server->cursor->y - output->ly;
	double cx, cy;
	output_local_to_canvas(output, ox, oy, &cx, &cy);

	if (button == BTN_RIGHT) {
		// Right click on the empty canvas or a desktop's interior (not
		// over a window) creates a desktop there.
		struct wlr_surface *surface;
		double sx, sy;
		enum cv_deco_part part = CV_DECO_NONE;
		uint32_t edges = 0;
		if (view_at(server, cx, cy, cam.zoom, &surface, &sx, &sy, &part,
				&edges) == NULL) {
			overview_new_desktop(output, cx, cy);
		}
		return;
	}
	if (button != BTN_LEFT) {
		return;
	}

	server->cursor_mode = CV_CURSOR_OVERVIEW_PRESS;
	server->grab_output = output;
	server->grab_cam = cam;
	server->grab_lx = server->cursor->x;
	server->grab_ly = server->cursor->y;
	server->grab_cx = cx;
	server->grab_cy = cy;
	server->grab_view = NULL;
	server->grab_desktop = NULL;
	server->grab_moved = false;
	record_overview_hit(server, output, ox, oy, cx, cy, cam.zoom);
}

/* ---- pointer buttons ---- */

static void forward_press(struct cv_server *server, uint32_t time,
		uint32_t button) {
	pressed_buttons |= 1u << (button & 31);
	wlr_seat_pointer_notify_button(server->seat, time, button,
		WL_POINTER_BUTTON_STATE_PRESSED);
}

static void cursor_button_press(struct cv_server *server, uint32_t time,
		uint32_t button) {
	super_tap = false;
	if (server->cursor_mode != CV_CURSOR_PASSTHROUGH) {
		return; // extra buttons during a compositor grab are swallowed
	}
	struct cv_output *output =
		output_at(server, server->cursor->x, server->cursor->y);
	if (output == NULL) {
		return;
	}
	if (launcher_pointer_button(server, output, server->cursor->x - output->lx,
			server->cursor->y - output->ly, button, true)) {
		return;
	}
	if (!output_interactive(output)) {
		overview_press(server, output, button);
		return;
	}

	bool super = seat_modifiers(server) & WLR_MODIFIER_LOGO;
	struct pointer_hit hit;
	pointer_hit_test(server, output, &hit);

	if (hit.view != NULL && super && button == BTN_LEFT) {
		view_focus(hit.view);
		begin_move(server, hit.view, output);
		return;
	}
	if (hit.view != NULL && super && button == BTN_RIGHT) {
		view_focus(hit.view);
		double cx, cy;
		output_local_to_canvas(output, server->cursor->x - output->lx,
			server->cursor->y - output->ly, &cx, &cy);
		begin_resize(server, hit.view, output, quadrant_edges(hit.view, cx, cy));
		return;
	}

	if (hit.surface != NULL) {
		if (hit.view != NULL) {
			view_focus(hit.view);
		} else if (hit.layer != NULL && hit.layer->layer_surface->current
				.keyboard_interactive
				!= ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
			focus_surface_keyboard(server, hit.surface);
		}
		forward_press(server, time, button);
		return;
	}

	if (hit.view != NULL) {
		// Somewhere on the server-side frame.
		view_focus(hit.view);
		switch (hit.part) {
		case CV_DECO_TITLE:
			if (button == BTN_LEFT) {
				if (server->last_title_view == hit.view &&
						time - server->last_title_click <= DOUBLE_CLICK_MS) {
					server->last_title_view = NULL;
					view_set_maximized(hit.view, !hit.view->maximized);
				} else {
					server->last_title_click = time;
					server->last_title_view = hit.view;
					begin_move(server, hit.view, output);
				}
			}
			return;
		case CV_DECO_MINIMIZE:
		case CV_DECO_MAXIMIZE:
		case CV_DECO_CLOSE:
			if (button == BTN_LEFT) {
				begin_deco_button(server, hit.view, output, hit.part);
			}
			return;
		case CV_DECO_RESIZE:
			if (button == BTN_LEFT || button == BTN_RIGHT) {
				begin_resize(server, hit.view, output, hit.edges);
			}
			return;
		default:
			return;
		}
	}

	if (button == BTN_LEFT) {
		begin_pan(server, output);
	}
}

static void release_deco_button(struct cv_server *server) {
	struct cv_view *view = server->grab_view;
	if (view == NULL) {
		return;
	}
	struct cv_output *output;
	double cx, cy;
	if (cursor_canvas_pos(server, &output, &cx, &cy)) {
		uint32_t edges = 0;
		enum cv_deco_part part = deco_hit(view, cx, cy,
			output_camera(output).zoom, &edges);
		if (part == server->grab_part) {
			switch (part) {
			case CV_DECO_MINIMIZE:
				view_set_minimized(view, true);
				break;
			case CV_DECO_MAXIMIZE:
				view_set_maximized(view, !view->maximized);
				break;
			case CV_DECO_CLOSE:
				view_close(view);
				break;
			default:
				break;
			}
		}
	}
	if (view->pressed != CV_DECO_NONE) {
		view->pressed = CV_DECO_NONE;
		view_damage(view);
	}
}

static void cursor_button_release(struct cv_server *server, uint32_t time,
		uint32_t button) {
	if (server->cursor_mode == CV_CURSOR_PASSTHROUGH) {
		uint32_t bit = 1u << (button & 31);
		if (pressed_buttons & bit) {
			pressed_buttons &= ~bit;
			wlr_seat_pointer_notify_button(server->seat, time, button,
				WL_POINTER_BUTTON_STATE_RELEASED);
		} else {
			struct cv_output *output =
				output_at(server, server->cursor->x, server->cursor->y);
			if (output != NULL) {
				launcher_pointer_button(server, output, server->cursor->x - output->lx,
					server->cursor->y - output->ly, button, false);
			}
		}
		return;
	}
	switch (server->cursor_mode) {
	case CV_CURSOR_DECO_BUTTON:
		release_deco_button(server);
		break;
	case CV_CURSOR_OVERVIEW_PRESS:
		overview_click(server);
		break;
	case CV_CURSOR_DESKTOP_DRAG:
		if (server->drop_desktop != NULL && server->grab_desktop != NULL) {
			canvas_combine(server, server->drop_desktop, server->grab_desktop,
				server->grab_box);
		} else if (server->grab_desktop != NULL) {
			desktop_raise(server->grab_desktop);
		}
		break;
	default:
		break;
	}
	input_cancel_grab(server);
}

/* ---- cursor event handlers ---- */

static void handle_cursor_motion(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	notify_activity(server);
	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void handle_cursor_motion_absolute(struct wl_listener *listener,
		void *data) {
	struct cv_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	notify_activity(server);
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
		event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

static void handle_cursor_button(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	notify_activity(server);
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		cursor_button_press(server, event->time_msec, event->button);
	} else {
		cursor_button_release(server, event->time_msec, event->button);
	}
}

static void handle_cursor_axis(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	notify_activity(server);
	super_tap = false;
	if (server->cursor_mode != CV_CURSOR_PASSTHROUGH) {
		return;
	}
	struct cv_output *output =
		output_at(server, server->cursor->x, server->cursor->y);
	if (output == NULL) {
		return;
	}
	if (event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL &&
			launcher_pointer_axis(server, output, server->cursor->x - output->lx,
				server->cursor->y - output->ly, event->delta)) {
		return;
	}
	if (output->mode == CV_MODE_OVERVIEW) {
		if (seat_modifiers(server) & WLR_MODIFIER_CTRL) {
			overview_zoom_by(output, exp(-event->delta / 200.0),
				server->cursor->x - output->lx, server->cursor->y - output->ly);
		} else {
			double delta = event->delta * SCROLL_PAN_SPEED;
			struct camera cam = output_camera(output);
			if (event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL) {
				cam.cy += delta / cam.zoom;
			} else {
				cam.cx += delta / cam.zoom;
			}
			camera_set(output, cam);
		}
		return;
	}
	if (server->seat->pointer_state.focused_surface != NULL) {
		wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
			event->orientation, event->delta, event->delta_discrete,
			event->source, event->relative_direction);
		return;
	}
	// Scrolling over the empty canvas pans; libinput already applied the
	// natural-scroll direction.
	if (output_interactive(output)) {
		double delta = event->delta * SCROLL_PAN_SPEED;
		if (event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL) {
			canvas_pan(output, 0, delta);
		} else {
			canvas_pan(output, delta, 0);
		}
	}
}

static void handle_cursor_frame(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

/* ---- touchpad gestures (recognition is delegated to gesture.c) ---- */

static void handle_swipe_begin(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, swipe_begin);
	struct wlr_pointer_swipe_begin_event *event = data;
	notify_activity(server);
	super_tap = false;
	gesture_swipe_begin(server, event->time_msec, event->fingers);
}

static void handle_swipe_update(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, swipe_update);
	struct wlr_pointer_swipe_update_event *event = data;
	notify_activity(server);
	gesture_swipe_update(server, event->time_msec, event->dx, event->dy);
}

static void handle_swipe_end(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, swipe_end);
	struct wlr_pointer_swipe_end_event *event = data;
	notify_activity(server);
	gesture_swipe_end(server, event->time_msec, event->cancelled);
}

static void handle_pinch_begin(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, pinch_begin);
	struct wlr_pointer_pinch_begin_event *event = data;
	notify_activity(server);
	super_tap = false;
	gesture_pinch_begin(server, event->time_msec, event->fingers);
}

static void handle_pinch_update(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, pinch_update);
	struct wlr_pointer_pinch_update_event *event = data;
	notify_activity(server);
	gesture_pinch_update(server, event->time_msec, event->dx, event->dy,
		event->scale, event->rotation);
}

static void handle_pinch_end(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, pinch_end);
	struct wlr_pointer_pinch_end_event *event = data;
	notify_activity(server);
	gesture_pinch_end(server, event->time_msec, event->cancelled);
}

static void handle_hold_begin(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, hold_begin);
	struct wlr_pointer_hold_begin_event *event = data;
	notify_activity(server);
	gesture_hold_begin(server, event->time_msec, event->fingers);
}

static void handle_hold_end(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, hold_end);
	struct wlr_pointer_hold_end_event *event = data;
	notify_activity(server);
	gesture_hold_end(server, event->time_msec, event->cancelled);
}

/* ---- seat requests ---- */

static void handle_request_cursor(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused =
		server->seat->pointer_state.focused_client;
	if (focused == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
		// The client owns the image now; our cached name is stale.
		server->cursor_image = NULL;
	}
}

static void handle_request_set_shape(struct wl_listener *listener, void *data) {
	struct cv_server *server =
		wl_container_of(listener, server, request_cursor_shape);
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (event->device_type != WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER) {
		return;
	}
	if (server->seat->pointer_state.focused_client == event->seat_client) {
		cursor_set_image(server, wlr_cursor_shape_v1_name(event->shape));
	}
}

static void handle_request_set_selection(struct wl_listener *listener,
		void *data) {
	struct cv_server *server =
		wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void handle_request_set_primary_selection(struct wl_listener *listener,
		void *data) {
	struct cv_server *server =
		wl_container_of(listener, server, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

static void handle_request_start_drag(struct wl_listener *listener,
		void *data) {
	struct cv_server *server =
		wl_container_of(listener, server, request_start_drag);
	struct wlr_seat_request_start_drag_event *event = data;
	if (!wlr_seat_validate_pointer_grab_serial(server->seat, event->origin,
			event->serial)) {
		if (event->drag->source != NULL) {
			wlr_data_source_destroy(event->drag->source);
		}
		return;
	}
	wlr_seat_request_start_drag(server->seat, event->drag, event->origin,
		event->serial);
}

static void handle_drag_icon_commit(struct wl_listener *listener, void *data) {
	struct cv_server *server =
		wl_container_of(listener, server, drag_icon_commit);
	struct wlr_surface *surface = data;
	server->drag_icon_dx += surface->current.dx;
	server->drag_icon_dy += surface->current.dy;
	server_damage_all(server);
}

static void handle_drag_icon_destroy(struct wl_listener *listener,
		void *data) {
	struct cv_server *server =
		wl_container_of(listener, server, drag_icon_destroy);
	wl_list_remove(&server->drag_icon_commit.link);
	wl_list_init(&server->drag_icon_commit.link);
	wl_list_remove(&server->drag_icon_destroy.link);
	wl_list_init(&server->drag_icon_destroy.link);
	server->drag_icon = NULL;
	server->drag_icon_dx = 0;
	server->drag_icon_dy = 0;
	server_damage_all(server);
}

static void handle_start_drag(struct wl_listener *listener, void *data) {
	struct cv_server *server = wl_container_of(listener, server, start_drag);
	struct wlr_drag *drag = data;
	server->drag_icon_dx = 0;
	server->drag_icon_dy = 0;
	server->drag_icon = drag->icon;
	if (drag->icon == NULL) {
		return;
	}
	server->drag_icon_commit.notify = handle_drag_icon_commit;
	wl_signal_add(&drag->icon->surface->events.commit, &server->drag_icon_commit);
	server->drag_icon_destroy.notify = handle_drag_icon_destroy;
	wl_signal_add(&drag->icon->events.destroy, &server->drag_icon_destroy);
	server_damage_all(server);
}

/* ---- module lifecycle ---- */

void input_init(struct cv_server *server) {
	server->new_input.notify = handle_new_input;
	wl_signal_add(&server->backend->events.new_input, &server->new_input);

	// Virtual keyboards (wtype & friends) behave like real ones.
	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboards =
		wlr_virtual_keyboard_manager_v1_create(server->display);
	server->new_virtual_keyboard.notify = handle_new_virtual_keyboard;
	wl_signal_add(&virtual_keyboards->events.new_virtual_keyboard,
		&server->new_virtual_keyboard);

	struct wlr_cursor_shape_manager_v1 *cursor_shapes =
		wlr_cursor_shape_manager_v1_create(server->display, 1);
	server->request_cursor_shape.notify = handle_request_set_shape;
	wl_signal_add(&cursor_shapes->events.request_set_shape,
		&server->request_cursor_shape);

	server->cursor_motion.notify = handle_cursor_motion;
	wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
	server->cursor_motion_absolute.notify = handle_cursor_motion_absolute;
	wl_signal_add(&server->cursor->events.motion_absolute,
		&server->cursor_motion_absolute);
	server->cursor_button.notify = handle_cursor_button;
	wl_signal_add(&server->cursor->events.button, &server->cursor_button);
	server->cursor_axis.notify = handle_cursor_axis;
	wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
	server->cursor_frame.notify = handle_cursor_frame;
	wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);

	server->swipe_begin.notify = handle_swipe_begin;
	wl_signal_add(&server->cursor->events.swipe_begin, &server->swipe_begin);
	server->swipe_update.notify = handle_swipe_update;
	wl_signal_add(&server->cursor->events.swipe_update, &server->swipe_update);
	server->swipe_end.notify = handle_swipe_end;
	wl_signal_add(&server->cursor->events.swipe_end, &server->swipe_end);
	server->pinch_begin.notify = handle_pinch_begin;
	wl_signal_add(&server->cursor->events.pinch_begin, &server->pinch_begin);
	server->pinch_update.notify = handle_pinch_update;
	wl_signal_add(&server->cursor->events.pinch_update, &server->pinch_update);
	server->pinch_end.notify = handle_pinch_end;
	wl_signal_add(&server->cursor->events.pinch_end, &server->pinch_end);
	server->hold_begin.notify = handle_hold_begin;
	wl_signal_add(&server->cursor->events.hold_begin, &server->hold_begin);
	server->hold_end.notify = handle_hold_end;
	wl_signal_add(&server->cursor->events.hold_end, &server->hold_end);

	server->request_cursor.notify = handle_request_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor,
		&server->request_cursor);
	server->request_set_selection.notify = handle_request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection,
		&server->request_set_selection);
	server->request_set_primary_selection.notify =
		handle_request_set_primary_selection;
	wl_signal_add(&server->seat->events.request_set_primary_selection,
		&server->request_set_primary_selection);
	server->request_start_drag.notify = handle_request_start_drag;
	wl_signal_add(&server->seat->events.request_start_drag,
		&server->request_start_drag);
	server->start_drag.notify = handle_start_drag;
	wl_signal_add(&server->seat->events.start_drag, &server->start_drag);
}

void input_finish(struct cv_server *server) {
	wl_list_remove(&server->new_input.link);
	wl_list_remove(&server->new_virtual_keyboard.link);
	wl_list_remove(&server->request_cursor_shape.link);
	wl_list_remove(&server->cursor_motion.link);
	wl_list_remove(&server->cursor_motion_absolute.link);
	wl_list_remove(&server->cursor_button.link);
	wl_list_remove(&server->cursor_axis.link);
	wl_list_remove(&server->cursor_frame.link);
	wl_list_remove(&server->swipe_begin.link);
	wl_list_remove(&server->swipe_update.link);
	wl_list_remove(&server->swipe_end.link);
	wl_list_remove(&server->pinch_begin.link);
	wl_list_remove(&server->pinch_update.link);
	wl_list_remove(&server->pinch_end.link);
	wl_list_remove(&server->hold_begin.link);
	wl_list_remove(&server->hold_end.link);
	wl_list_remove(&server->request_cursor.link);
	wl_list_remove(&server->request_set_selection.link);
	wl_list_remove(&server->request_set_primary_selection.link);
	wl_list_remove(&server->request_start_drag.link);
	wl_list_remove(&server->start_drag.link);
	if (server->drag_icon != NULL) {
		wl_list_remove(&server->drag_icon_commit.link);
		wl_list_init(&server->drag_icon_commit.link);
		wl_list_remove(&server->drag_icon_destroy.link);
		wl_list_init(&server->drag_icon_destroy.link);
		server->drag_icon = NULL;
		server->drag_icon_dx = 0;
		server->drag_icon_dy = 0;
	}
}
