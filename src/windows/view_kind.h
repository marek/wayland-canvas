#pragma once

// What a window is to the window manager. Its shell answers once
// (View::kind()); every layout rule reads the answer through the functions
// below: where the window opens, whether it tiles, whether the compositor
// gives it the keyboard. No rule asks the shells itself, so a new kind of
// window is a row here, not a check added to each rule
// (docs/intents/windows/window-management.md).
enum class ViewKind {
	normal, // placed, tiled and focused by the compositor
	dialog, // belongs to another window and leaves it usable
	modal, // belongs to another window and blocks it until it goes
	client_placed, // an X11 menu, tooltip or drag icon, placed by its own client
};

// Its client says where it goes: the compositor neither places it nor
// settles it on a board when it maps.
constexpr bool placed_by_client(ViewKind kind) {
	return kind == ViewKind::client_placed;
}

// Opens centred over the window it belongs to.
constexpr bool opens_over_parent(ViewKind kind) {
	return kind == ViewKind::dialog || kind == ViewKind::modal;
}

// Takes a slice of a tiled board. A modal dialog floats over the window it
// blocks; a client-placed window stays where its client put it.
constexpr bool tiles(ViewKind kind) {
	return kind == ViewKind::normal || kind == ViewKind::dialog;
}

// May be given the keyboard when the compositor picks a window to focus.
constexpr bool takes_focus(ViewKind kind) {
	return kind != ViewKind::client_placed;
}
