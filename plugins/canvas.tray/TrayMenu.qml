import Quickshell
import QtQuick
import WaylandCanvas.Ui
import "."

// The menu of one tray item, opened by a right click on its icon: the item's
// own entries with their checkboxes and separators, a submenu swapping in
// (with ‹ back out of it). Triggering an entry closes the menu. The item
// whose menu is showing lives in the plugin's TrayState; the host's panel
// manager anchors the card under the icon that was clicked.
PanelCard {
	id: root

	panelWidth: 200
	panelName: "tray"

	readonly property var entry: TrayState.activeItem

	// Openers below the top level: [{opener, title}]. Each level keeps its
	// own live opener — a child entry is owned by its parent's model, so one
	// shared opener would destroy the entry being shown.
	property var stack: []

	// Another item's menu was asked for: start over from its top level.
	onEntryChanged: resetStack()

	Component {
		id: submenuOpener
		QsMenuOpener {}
	}

	QsMenuOpener {
		id: base
		menu: root.entry ? root.entry.menu : null
	}

	readonly property var opener: stack.length > 0
		? stack[stack.length - 1].opener : base

	function resetStack() {
		var openers = stack
		stack = []
		for (var i = 0; i < openers.length; i++) {
			openers[i].opener.destroy()
		}
	}

	function push(entry, title) {
		var next = stack.slice()
		next.push({opener: submenuOpener.createObject(root, {menu: entry}), title: title})
		stack = next
	}

	function pop() {
		var next = stack.slice()
		var top = next.pop()
		stack = next
		if (top != null) {
			top.opener.destroy()
		}
	}

	// The header names the item, or offers the way back out of a submenu.
	Item {
		width: parent.width
		height: UiTheme.sectionHeight

		PanelText {
			visible: root.stack.length === 0
			anchors.left: parent.left
			dim: true
			text: root.entry ? (root.entry.title !== "" ? root.entry.title : root.entry.id) : ""
		}

		Text {
			visible: root.stack.length > 0
			anchors.left: parent.left
			text: "‹ " + (root.stack.length > 0 ? root.stack[root.stack.length - 1].title : "")
			color: backMouse.containsMouse ? UiTheme.attention : UiTheme.text
			font {
				family: UiTheme.fontFamily
				pixelSize: UiTheme.fontBody
			}
			renderType: Text.NativeRendering

			MouseArea {
				id: backMouse
				anchors.fill: parent
				anchors.margins: -6
				hoverEnabled: true
				cursorShape: Qt.PointingHandCursor
				onClicked: root.pop()
			}
		}
	}

	Repeater {
		model: root.opener.children

		Item {
			id: entry
			required property var modelData
			readonly property var menuEntry: modelData
			width: parent ? parent.width : 0
			height: menuEntry.isSeparator
				? 7
				: UiTheme.rowHeight
			visible: menuEntry.isSeparator || menuEntry.text !== ""

			// A separator: a hairline, nothing clickable.
			Rectangle {
				visible: entry.menuEntry.isSeparator
				anchors.verticalCenter: parent.verticalCenter
				x: 8
				width: parent.width - 16
				height: 1
				color: UiTheme.muted
			}

			PanelRow {
				visible: !entry.menuEntry.isSeparator
				width: parent.width
				height: parent.height
				enabled: entry.menuEntry.enabled
				label: {
					var mark = entry.menuEntry.checkState === Qt.Checked ? "✓ "
						: entry.menuEntry.checkState === Qt.PartiallyChecked ? "◐ " : ""
					return mark + entry.menuEntry.text
				}
				onClicked: {
					if (entry.menuEntry.hasChildren) {
						root.push(entry.menuEntry, entry.menuEntry.text)
					} else {
						entry.menuEntry.triggered()
						root.close()
					}
				}

				Text {
					visible: entry.menuEntry.hasChildren
					anchors.right: parent.right
					anchors.rightMargin: 8
					anchors.verticalCenter: parent.verticalCenter
					text: "▸"
					color: UiTheme.muted
					font {
						family: UiTheme.fontFamily
						pixelSize: UiTheme.fontBody
					}
					renderType: Text.NativeRendering
				}
			}
		}
	}
}
