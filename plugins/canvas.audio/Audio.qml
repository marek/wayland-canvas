import Quickshell.Services.Pipewire
import QtQuick
import WaylandCanvas.Ui

// Audio: the default sink, crossed out when muted. Left opens the panel
// (the manifest names it); the wheel still steps the volume by 5 %.
BarButton {
	id: root

	readonly property var sink: Pipewire.defaultAudioSink
	readonly property bool muted: sink != null && sink.audio != null && sink.audio.muted

	// The sink's volume and mute are filled in only while it is bound.
	PwObjectTracker {
		objects: root.sink ? [root.sink] : []
	}

	icon: true
	text: muted ? "\ueee8" : sinkGlyph()

	onWheeled: function(delta) {
		if (sink == null || sink.audio == null) {
			return
		}
		var volume = sink.audio.volume + Math.round(delta / 120) * 0.05
		sink.audio.volume = Math.max(0, Math.min(1, volume))
	}

	// The sink's own glyph, by how it describes itself.
	function sinkGlyph() {
		if (sink == null) {
			return "󰓃"
		}
		var blob = [sink.name, sink.description, sink.nickname].join(" ").toLowerCase()
		if (blob.indexOf("headphone") !== -1) {
			return "󰋋"
		}
		if (blob.indexOf("bluetooth") !== -1) {
			return "󰂯"
		}
		if (blob.indexOf("hdmi") !== -1 || blob.indexOf("display") !== -1) {
			return "󰍹"
		}
		return "󰓃"
	}
}
