// An X11 client for the scenarios: maps one window of a known size, colour
// and title through xcb (and with --dialog, on its first key, a modal dialog
// transient for it; with --moveresize, on its first button, a
// _NET_WM_MOVERESIZE request to size from the top left corner), and says on
// stdout what reaches it —
//
//   MAPPED <w>x<h>         the window is up
//   KEY <keycode> <state>  a key press arrived, with X's modifier mask
//   BUTTON <n> <x>,<y>     a pointer button arrived at that point of the window
//   MOVERESIZE             the resize request went to the window manager
//   CONFIGURED <w>x<h>     the window manager resized it
//   POPUP <x>,<y>          an override-redirect window was mapped there
//   DIALOG <w>x<h>         the modal dialog is up, titled <title>-dialog
//   OFFERED                the client owns the X11 clipboard
//   PASTED <text>          what the X11 clipboard holds, read back
//   CLOSING                WM_DELETE_WINDOW arrived; the client exits
//
// so the compositor's X11 half can be proven without installing an X11
// application.
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>

// The atom of a name, interned once.
static xcb_atom_t intern(xcb_connection_t *connection, const char *name) {
	xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection,
		xcb_intern_atom(connection, 0, (uint16_t)strlen(name), name), NULL);
	xcb_atom_t atom = reply != NULL ? reply->atom : XCB_ATOM_NONE;
	free(reply);
	return atom;
}

static void say(const char *format, ...) {
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	fflush(stdout);
}

int main(int argc, char **argv) {
	int width = 400, height = 300;
	// Where the menu goes, in the same space the window manager places
	// windows in.
	int popup_x = 120, popup_y = 140, popup_w = 160, popup_h = 90;
	uint32_t colour = 0x00c04040;
	const char *title = "canvas-x11";
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--size=", 7) == 0) {
			sscanf(argv[i] + 7, "%dx%d", &width, &height);
		} else if (strncmp(argv[i], "--colour=", 9) == 0) {
			colour = (uint32_t)strtoul(argv[i] + 9, NULL, 16);
		} else if (strncmp(argv[i], "--title=", 8) == 0) {
			title = argv[i] + 8;
		}
	}
	const char *offer = NULL;
	int popup = 0, paste = 0, paste_tries = 0, dialog = 0, moveresize = 0;
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--offer=", 8) == 0) {
			offer = argv[i] + 8;
		} else if (strcmp(argv[i], "--popup") == 0) {
			popup = 1;
		} else if (strcmp(argv[i], "--paste") == 0) {
			paste = 1;
		} else if (strcmp(argv[i], "--dialog") == 0) {
			dialog = 1;
		} else if (strcmp(argv[i], "--moveresize") == 0) {
			moveresize = 1;
		}
	}
	int screen_number = 0;
	xcb_connection_t *connection = xcb_connect(NULL, &screen_number);
	if (connection == NULL || xcb_connection_has_error(connection)) {
		fprintf(stderr, "x11-client: cannot reach the X server\n");
		return 2;
	}
	const xcb_setup_t *setup = xcb_get_setup(connection);
	xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
	for (int i = 0; i < screen_number; i++) {
		xcb_screen_next(&screens);
	}
	xcb_screen_t *screen = screens.data;

	xcb_window_t window = xcb_generate_id(connection);
	uint32_t values[2] = {colour,
		XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS
		| XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
	xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root,
		0, 0, (uint16_t)width, (uint16_t)height, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
		XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);

	xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
		XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, (uint32_t)strlen(title), title);
	const char *class_hint = "canvas-x11\0canvas-x11";
	xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
		XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, 22, class_hint);

	// WM_DELETE_WINDOW, so the title bar's × closes it politely.
	xcb_intern_atom_cookie_t protocols_cookie =
		xcb_intern_atom(connection, 1, 12, "WM_PROTOCOLS");
	xcb_intern_atom_reply_t *protocols =
		xcb_intern_atom_reply(connection, protocols_cookie, NULL);
	xcb_intern_atom_cookie_t delete_cookie =
		xcb_intern_atom(connection, 0, 16, "WM_DELETE_WINDOW");
	xcb_intern_atom_reply_t *delete_window =
		xcb_intern_atom_reply(connection, delete_cookie, NULL);
	if (protocols != NULL && delete_window != NULL) {
		xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
			protocols->atom, XCB_ATOM_ATOM, 32, 1, &delete_window->atom);
	}

	xcb_map_window(connection, window);
	xcb_flush(connection);
	say("MAPPED %dx%d\n", width, height);

	// A menu of our own: an override-redirect window beside the first one,
	// which the window manager must place where we put it and decorate not
	// at all.
	xcb_window_t menu = XCB_NONE;
	if (popup) {
		menu = xcb_generate_id(connection);
		uint32_t menu_values[2] = {0x0040c040, 1}; // colour, override-redirect
		xcb_create_window(connection, XCB_COPY_FROM_PARENT, menu, screen->root,
			(int16_t)popup_x, (int16_t)popup_y, (uint16_t)popup_w,
			(uint16_t)popup_h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT,
			menu_values);
		xcb_map_window(connection, menu);
		xcb_flush(connection);
		say("POPUP %d,%d\n", popup_x, popup_y);
	}

	// The clipboard, this side of it: owning the X11 selection, and
	// reading back whatever owns it (a wayland client's copy arrives the
	// same way, through the compositor's bridge).
	xcb_atom_t clipboard = intern(connection, "CLIPBOARD");
	xcb_atom_t utf8 = intern(connection, "UTF8_STRING");
	xcb_atom_t targets = intern(connection, "TARGETS");
	xcb_atom_t selection_prop = intern(connection, "CANVAS_SELECTION");
	if (offer != NULL) {
		xcb_set_selection_owner(connection, window, clipboard, XCB_CURRENT_TIME);
		xcb_flush(connection);
		say("OFFERED\n");
	}
	if (paste) {
		xcb_convert_selection(connection, window, clipboard, utf8,
			selection_prop, XCB_CURRENT_TIME);
		xcb_flush(connection);
	}

	// The window manager passes an X11 selection on for the window that
	// holds the focus, and it learns of one when its owner changes: a
	// client that took the clipboard before it was focused says so again,
	// every quarter second, until it has been asked for it once.
	struct pollfd pfd = {.fd = xcb_get_file_descriptor(connection),
		.events = POLLIN};
	int asserts = offer != NULL ? 60 : 0;
	int asked = 0;
	xcb_generic_event_t *event = NULL;
	while (1) {
		while ((event = xcb_poll_for_event(connection)) == NULL) {
			xcb_flush(connection);
			poll(&pfd, 1, 250);
			if (xcb_connection_has_error(connection)) {
				return 0;
			}
			if (asserts > 0 && asked == 0) {
				asserts--;
				xcb_set_selection_owner(connection, window, clipboard,
					XCB_CURRENT_TIME);
				xcb_flush(connection);
			}
		}
		if ((event->response_type & ~0x80) == XCB_SELECTION_REQUEST) {
			asked = 1;
		}
		switch (event->response_type & ~0x80) {
		case XCB_KEY_PRESS:
			// With --dialog, the first key opens a modal dialog of our own:
			// transient for the window, of the dialog type and
			// _NET_WM_STATE_MODAL, as toolkits make them.
			if (dialog == 1) {
				dialog = 2;
				xcb_window_t modal = xcb_generate_id(connection);
				xcb_create_window(connection, XCB_COPY_FROM_PARENT, modal, screen->root,
					0, 0, 300, 200, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
					XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
				char dialog_title[128];
				snprintf(dialog_title, sizeof dialog_title, "%s-dialog", title);
				xcb_change_property(connection, XCB_PROP_MODE_REPLACE, modal,
					XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, (uint32_t)strlen(dialog_title),
					dialog_title);
				xcb_change_property(connection, XCB_PROP_MODE_REPLACE, modal,
					XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1, &window);
				xcb_atom_t type = intern(connection, "_NET_WM_WINDOW_TYPE");
				xcb_atom_t type_dialog = intern(connection, "_NET_WM_WINDOW_TYPE_DIALOG");
				xcb_change_property(connection, XCB_PROP_MODE_REPLACE, modal, type,
					XCB_ATOM_ATOM, 32, 1, &type_dialog);
				xcb_atom_t state = intern(connection, "_NET_WM_STATE");
				xcb_atom_t state_modal = intern(connection, "_NET_WM_STATE_MODAL");
				xcb_change_property(connection, XCB_PROP_MODE_REPLACE, modal, state,
					XCB_ATOM_ATOM, 32, 1, &state_modal);
				xcb_map_window(connection, modal);
				xcb_flush(connection);
				say("DIALOG 300x200\n");
			}
			say("KEY %u %u\n", ((xcb_key_press_event_t *)event)->detail,
				((xcb_key_press_event_t *)event)->state);
			break;
		case XCB_BUTTON_PRESS: {
			xcb_button_press_event_t *press = (xcb_button_press_event_t *)event;
			say("BUTTON %u %d,%d\n", press->detail, press->event_x, press->event_y);
			if (moveresize == 1) {
				// Toolkits ask the window manager to run a resize the user
				// started in their own client area: a client message to the
				// root (EWMH _NET_WM_MOVERESIZE, direction 0 is the top left
				// corner, source 1 an application).
				moveresize = 2;
				xcb_client_message_event_t request = {
					.response_type = XCB_CLIENT_MESSAGE,
					.format = 32,
					.window = window,
					.type = intern(connection, "_NET_WM_MOVERESIZE"),
				};
				request.data.data32[0] = (uint32_t)press->root_x;
				request.data.data32[1] = (uint32_t)press->root_y;
				request.data.data32[2] = 0;
				request.data.data32[3] = press->detail;
				request.data.data32[4] = 1;
				xcb_send_event(connection, 0, screen->root,
					XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT
					| XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, (const char *)&request);
				xcb_flush(connection);
				say("MOVERESIZE\n");
			}
			break;
		}
		case XCB_CONFIGURE_NOTIFY: {
			xcb_configure_notify_event_t *configure =
				(xcb_configure_notify_event_t *)event;
			say("CONFIGURED %dx%d\n", configure->width, configure->height);
			break;
		}
		case XCB_SELECTION_REQUEST: {
			// Someone (the compositor, for a wayland client) wants what we
			// put on the clipboard.
			xcb_selection_request_event_t *request =
				(xcb_selection_request_event_t *)event;
			xcb_selection_notify_event_t notify = {
				.response_type = XCB_SELECTION_NOTIFY,
				.time = request->time,
				.requestor = request->requestor,
				.selection = request->selection,
				.target = request->target,
				.property = request->property,
			};
			if (offer != NULL && request->target == utf8) {
				xcb_change_property(connection, XCB_PROP_MODE_REPLACE,
					request->requestor, request->property, utf8, 8,
					(uint32_t)strlen(offer), offer);
			} else if (offer != NULL && request->target == targets) {
				xcb_atom_t list[2] = {targets, utf8};
				xcb_change_property(connection, XCB_PROP_MODE_REPLACE,
					request->requestor, request->property, XCB_ATOM_ATOM, 32,
					2, list);
			} else {
				notify.property = XCB_NONE;
			}
			xcb_send_event(connection, 0, request->requestor, 0,
				(const char *)&notify);
			xcb_flush(connection);
			break;
		}
		case XCB_SELECTION_NOTIFY: {
			xcb_selection_notify_event_t *notify =
				(xcb_selection_notify_event_t *)event;
			// The offer may not have crossed the compositor's bridge yet:
			// ask again rather than report an empty clipboard.
			if (notify->property == XCB_NONE) {
				if (++paste_tries < 20) {
					usleep(200000);
					xcb_convert_selection(connection, window, clipboard, utf8,
						selection_prop, XCB_CURRENT_TIME);
					xcb_flush(connection);
					break;
				}
				say("PASTED\n");
				break;
			}
			xcb_get_property_reply_t *value = xcb_get_property_reply(connection,
				xcb_get_property(connection, 1, window, notify->property,
					XCB_GET_PROPERTY_TYPE_ANY, 0, 1024), NULL);
			int length = value != NULL ? xcb_get_property_value_length(value) : 0;
			if (length == 0 && ++paste_tries < 20) {
				free(value);
				usleep(200000);
				xcb_convert_selection(connection, window, clipboard, utf8,
					selection_prop, XCB_CURRENT_TIME);
				xcb_flush(connection);
				break;
			}
			say("PASTED %.*s\n", length,
				value != NULL ? (char *)xcb_get_property_value(value) : "");
			free(value);
			break;
		}
		case XCB_CLIENT_MESSAGE: {
			xcb_client_message_event_t *message = (xcb_client_message_event_t *)event;
			if (delete_window != NULL
					&& message->data.data32[0] == delete_window->atom) {
				say("CLOSING\n");
				free(event);
				xcb_disconnect(connection);
				return 0;
			}
			break;
		}
		default:
			break;
		}
		free(event);
	}
	xcb_disconnect(connection);
	return 0;
}
