pragma Singleton

import QtQuick

// The tray item whose menu is showing — shared between the widget that
// opens the menu and the menu panel the host instantiates.
QtObject {
	property var activeItem: null
}
