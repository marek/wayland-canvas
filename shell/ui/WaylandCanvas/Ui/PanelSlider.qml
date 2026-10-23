import QtQuick
import "."

// A slider: a track, an accent fill and a knob. Press-drag and wheel steps
// emit `moved` with the new value (0..1); the caller owns the value, and the
// knob follows `live` so a slow service never yanks it back mid-drag.
// `committed` delivers the last value once as the press ends (a wheel step is
// a gesture of its own).
Item {
	id: root

	property real value: 0
	signal moved(real value)
	signal committed(real value)

	width: parent ? parent.width : 0
	height: UiTheme.rowHeight

	property real live: value
	onValueChanged: if (!mouse.pressed) live = value

	function clamp(v) {
		return Math.max(0, Math.min(1, v))
	}

	function at(x) {
		return clamp(x / Math.max(1, track.width))
	}

	Rectangle {
		id: track
		anchors.left: parent.left
		anchors.right: parent.right
		anchors.verticalCenter: parent.verticalCenter
		height: 4
		radius: 2
		color: UiTheme.muted

		Rectangle {
			anchors.left: parent.left
			anchors.verticalCenter: parent.verticalCenter
			height: parent.height
			radius: parent.radius
			width: parent.width * root.clamp(root.live)
			color: UiTheme.accent
		}
	}

	Rectangle {
		id: knob
		width: 14
		height: 14
		radius: 7
		anchors.verticalCenter: parent.verticalCenter
		x: Math.max(0, Math.min(parent.width - width,
			track.width * root.clamp(root.live) - width / 2))
		color: UiTheme.text
	}

	MouseArea {
		id: mouse

		// True from the press to the one commit: a release and the cancel a
		// rebuilt page can bring right behind it commit the value once.
		property bool gesture: false

		function commit() {
			if (gesture) {
				gesture = false
				root.committed(root.live)
			}
		}

		anchors.fill: parent
		hoverEnabled: true
		cursorShape: Qt.PointingHandCursor
		onPressed: function(m) {
			gesture = true
			root.live = root.at(m.x)
			root.moved(root.live)
		}
		onPositionChanged: function(m) {
			if (pressed) {
				root.live = root.at(m.x)
				root.moved(root.live)
			}
		}
		onReleased: commit()
		onCanceled: commit()
		onWheel: function(w) {
			root.live = root.clamp(root.live + (w.angleDelta.y > 0 ? 0.05 : -0.05))
			root.moved(root.live)
			root.committed(root.live)
		}
	}
}
