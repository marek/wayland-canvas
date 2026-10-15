import QtQuick

// A slider: a track, an accent fill and a knob. Press-drag and wheel steps
// emit `moved` with the new value (0..1). The caller owns the value: bind it
// to the real state and apply what `moved` delivers — the knob follows
// `live`, the in-hand position, so a slow service never yanks it back
// mid-drag.
Item {
	id: root

	property var theme: null
	property real value: 0
	signal moved(real value)

	width: parent ? parent.width : 0
	height: theme ? theme.rowHeight : 32

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
		color: root.theme ? root.theme.muted : "#565f89"

		Rectangle {
			anchors.left: parent.left
			anchors.verticalCenter: parent.verticalCenter
			height: parent.height
			radius: parent.radius
			width: parent.width * root.clamp(root.live)
			color: root.theme ? root.theme.accent : "#7aa2f7"
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
		color: root.theme ? root.theme.text : "#a9b1d6"
	}

	MouseArea {
		id: mouse
		anchors.fill: parent
		hoverEnabled: true
		cursorShape: Qt.PointingHandCursor
		onPressed: function(m) {
			root.live = root.at(m.x)
			root.moved(root.live)
		}
		onPositionChanged: function(m) {
			if (pressed) {
				root.live = root.at(m.x)
				root.moved(root.live)
			}
		}
		onWheel: function(w) {
			root.live = root.clamp(root.live + (w.angleDelta.y > 0 ? 0.05 : -0.05))
			root.moved(root.live)
		}
	}
}
