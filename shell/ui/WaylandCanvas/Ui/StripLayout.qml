pragma Singleton
import QtQuick
import "."

// Where the overview's strips sit and where a card goes beside one
// (docs/intents/shell/app-bar.md). Every EdgeStrip joins on creation, leaves
// on destruction and publishes its own `length`: the strips of one output
// that share an edge centre as a single strip, in `order`, with a gap
// between them. So the toolbar and the app bar line up without either
// plugin knowing the other.
QtObject {
	// The gap between two strips, with the separator line drawn in it.
	readonly property int separator: 9

	property var strips: []

	function join(strip) {
		strips = strips.concat([strip])
	}

	function leave(strip) {
		strips = strips.filter(function(other) { return other !== strip })
	}

	// The strips with something to show on `strip`'s edge of its output,
	// first part first.
	function sharing(strip) {
		return strips.filter(function(other) {
			return other.length > 0 && other.edge === strip.edge
				&& other.modelData.name === strip.modelData.name
		}).sort(function(a, b) { return a.order - b.order })
	}

	// Whether a strip comes before `strip` on its edge: the separator is
	// drawn in the gap between the two.
	function hasLeader(strip) {
		return strip.length > 0 && sharing(strip).some(function(other) {
			return other.order < strip.order
		})
	}

	// Where `strip`'s part starts along its edge, measured from the screen's
	// leading corner, with every part on the edge centred as one.
	function start(strip) {
		var parts = sharing(strip)
		var total = 0
		for (var i = 0; i < parts.length; i++) {
			total += parts[i].length + (i > 0 ? separator : 0)
		}
		var screenLength = strip.vertical ? strip.modelData.height : strip.modelData.width
		var at = Math.round((screenLength - total) / 2)
		for (var j = 0; j < parts.length && parts[j] !== strip; j++) {
			at += parts[j].length + separator
		}
		return at
	}

	// `want` as the near edge of a `size` long card on an `extent` long
	// output, held one edge margin inside it.
	function clampInside(want, size, extent) {
		return Math.round(Math.max(UiTheme.edgeMargin,
			Math.min(want, extent - size - UiTheme.edgeMargin)))
	}

	// The top-left corner of a `width` x `height` card beside `box`, a strip
	// or one of its squares in output coordinates, on the strip's inner
	// side — toward the screen's centre — `gap` away from it. Along the
	// strip the card is centred on the box, or starts where the box does.
	// It stays inside `bounds`, the output.
	function placeBeside(edge, box, width, height, bounds, gap, centred) {
		var x = centred ? box.x + box.width / 2 - width / 2 : box.x
		var y = centred ? box.y + box.height / 2 - height / 2 : box.y
		if (edge === "right") {
			x = box.x - gap - width
		} else if (edge === "left") {
			x = box.x + box.width + gap
		} else if (edge === "top") {
			y = box.y + box.height + gap
		} else {
			y = box.y - gap - height
		}
		return Qt.point(clampInside(x, width, bounds.width),
			clampInside(y, height, bounds.height))
	}
}
