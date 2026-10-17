import QtQuick
import WaylandCanvas.Ui

// One action's row in the Shortcuts tab: the label with a Reset button
// while the keys differ from the defaults and a Clear button while it
// has keys, its chords as chips on the line under the label (or a
// muted "None"), and a third line only while it records ("Press the
// new keys — Esc cancels") or has something to say — a chord moved from
// someone else, or a failed set's error. A click on the row records;
// the row's own mouse area comes first, so the buttons keep their
// clicks. Fixed line metrics, so a scenario can find each line.
Item {
	id: root

	property var entry: null // {scope, action, label, group, keys, defaults}
	property bool recording: false
	property string note: "" // the moved-chord line
	property string fault: "" // a failed set or recording's error

	signal record()
	signal reset()
	signal clear()

	width: parent ? parent.width : 0
	height: 2 * UiTheme.rowHeight
		+ (root.detail !== "" ? UiTheme.detailHeight : 0)


	readonly property bool hasKeys: entry != null && entry.keys != null
		&& entry.keys.length > 0
	readonly property bool differs: {
		if (entry == null) {
			return false
		}
		var keys = entry.keys || []
		var defaults = entry.defaults || []
		if (keys.length !== defaults.length) {
			return true
		}
		for (var i = 0; i < keys.length; i++) {
			if (keys[i] !== defaults[i]) {
				return true
			}
		}
		return false
	}
	readonly property string detail: root.fault !== "" ? root.fault
		: root.recording ? "Press the new keys — Esc cancels"
		: root.note

	Rectangle {
		anchors.fill: parent
		radius: 4
		visible: mouse.containsMouse
		color: UiTheme.hover
	}

	MouseArea {
		id: mouse
		anchors.fill: parent
		hoverEnabled: true
		cursorShape: Qt.PointingHandCursor
		onClicked: root.record()
	}

	// The label line, with the buttons parked on its right, above the
	// row's own mouse area.
	PanelText {
		x: 8
		y: (UiTheme.rowHeight - implicitHeight) / 2
		width: parent.width - 16 - buttons.width - 8
		elide: Text.ElideRight
		text: root.entry ? root.entry.label : ""
		color: root.fault !== "" ? UiTheme.attention : UiTheme.text
	}

	Row {
		id: buttons
		anchors.right: parent.right
		anchors.rightMargin: 8
		anchors.verticalCenter: parent.top
		anchors.verticalCenterOffset: UiTheme.rowHeight / 2
		spacing: 8

		PanelText {
			visible: root.differs
			text: "Reset"
			color: resetMouse.containsMouse ? UiTheme.attention : UiTheme.muted

			MouseArea {
				id: resetMouse
				anchors.fill: parent
				anchors.margins: -6
				hoverEnabled: true
				cursorShape: Qt.PointingHandCursor
				onClicked: root.reset()
			}
		}

		PanelText {
			visible: root.hasKeys
			text: "Clear"
			color: clearMouse.containsMouse ? UiTheme.attention : UiTheme.muted

			MouseArea {
				id: clearMouse
				anchors.fill: parent
				anchors.margins: -6
				hoverEnabled: true
				cursorShape: Qt.PointingHandCursor
				onClicked: root.clear()
			}
		}
	}

	// The chords line.
	Row {
		x: 8
		y: UiTheme.rowHeight + (UiTheme.rowHeight - height) / 2
		spacing: 4

		Repeater {
			model: root.hasKeys ? root.entry.keys : []

			KeyChip {
				required property var modelData
				chord: modelData
			}
		}

		PanelText {
			visible: !root.hasKeys
			anchors.verticalCenter: parent.verticalCenter
			dim: true
			text: "None"
		}
	}

	// The prompt, the moved-chord line or the error, only while there.
	PanelText {
		visible: root.detail !== ""
		x: 8
		y: 2 * UiTheme.rowHeight
		width: parent.width - 16
		elide: Text.ElideRight
		color: root.fault !== "" ? UiTheme.attention : UiTheme.muted
		text: root.detail
	}
}
