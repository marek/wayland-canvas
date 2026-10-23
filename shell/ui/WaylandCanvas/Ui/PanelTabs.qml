import QtQuick
import "."

// One row of tabs, each an equal share of the row's width: the chosen tab
// carries the accent's tint and the accent's text, the rest stay muted. A
// click chooses; the caller owns which id is current. A label is bounded by
// its own share and elided inside it, so two labels can never run into one
// another — the card is sized to hold its tabs' names (PanelCard's
// panelWidth), and the elide is the backstop.
//
// Fixed metrics and equal shares, so a scenario finds tab i centred at
// (i + 0.5) / n of the row's width.
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
