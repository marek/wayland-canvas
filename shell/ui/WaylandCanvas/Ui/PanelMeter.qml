import QtQuick
import "."

// A bar: a track with a share of it filled. `value` is the share, 0..1; `fill`
// is the share's colour. The track is the item's own `color`, the panel's
// grey unless the caller sets another (a transparent one for a share drawn
// behind a row); a caller sets `height` and `radius` for a thicker bar.
Rectangle {
	id: root

	property real value: 0
	property color fill: UiTheme.accent

	width: parent ? parent.width : implicitWidth
	implicitWidth: 100
	height: 5
	radius: Math.floor(height / 2)
	color: UiTheme.track

	Rectangle {
		width: root.width * Math.max(0, Math.min(root.value, 1))
		height: parent.height
		radius: parent.radius
		color: root.fill
	}
}
