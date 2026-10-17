// An X11 client for the scenarios: maps one window of a known size and
// colour through xcb, and says on stdout what reaches it —
//
//   MAPPED <w>x<h>    the window is up
//   KEY <keycode>     a key press arrived
//   BUTTON <n>        a pointer button arrived
//   CONFIGURED <w>x<h> the window manager resized it
//   CLOSING           WM_DELETE_WINDOW arrived; the client exits
//
// so the compositor's X11 half can be proven without installing an X11
// application.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>

static void say(const char *format, ...) {
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	fflush(stdout);
}

int main(int argc, char **argv) {
	int width = 400, height = 300;
	uint32_t colour = 0x00c04040;
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--size=", 7) == 0) {
			sscanf(argv[i] + 7, "%dx%d", &width, &height);
		} else if (strncmp(argv[i], "--colour=", 9) == 0) {
			colour = (uint32_t)strtoul(argv[i] + 9, NULL, 16);
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

	const char *title = "canvas-x11";
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

	xcb_generic_event_t *event = NULL;
	while ((event = xcb_wait_for_event(connection)) != NULL) {
		switch (event->response_type & ~0x80) {
		case XCB_KEY_PRESS:
			say("KEY %u\n", ((xcb_key_press_event_t *)event)->detail);
			break;
		case XCB_BUTTON_PRESS:
			say("BUTTON %u\n", ((xcb_button_press_event_t *)event)->detail);
			break;
		case XCB_CONFIGURE_NOTIFY: {
			xcb_configure_notify_event_t *configure =
				(xcb_configure_notify_event_t *)event;
			say("CONFIGURED %dx%d\n", configure->width, configure->height);
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
