import QtQuick
import "."

// One row of tabs, each an equal share of the row's width: the chosen one in
// the accent's tint and text, the rest muted. A click chooses; the caller owns
// the current id. A label is elided inside its share so two never run into
// each other; the card is sized to hold the names (PanelCard's panelWidth).
Item {
	id: root

	property var tabs: [] // {id, label}
	property string current: ""
	signal chosen(string id)

	width: parent ? parent.width : 0
	height: UiTheme.rowHeight

	Row {
		anchors.fill: parent

		Repeater {
			model: root.tabs

			Rectangle {
				id: tab
				required property var modelData
				width: root.width / root.tabs.length
				height: UiTheme.rowHeight
				radius: UiTheme.radius
				color: tab.modelData.id === root.current
					? UiTheme.selected : "transparent"

				PanelText {
					anchors.centerIn: parent
					width: tab.width - 6
					horizontalAlignment: Text.AlignHCenter
					elide: Text.ElideRight
					text: tab.modelData.label
					color: tab.modelData.id === root.current
						? UiTheme.accent : UiTheme.muted
				}

				MouseArea {
					anchors.fill: parent
					hoverEnabled: true
					cursorShape: Qt.PointingHandCursor
					onClicked: root.chosen(tab.modelData.id)
				}
			}
		}
	}
}
