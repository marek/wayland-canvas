import Quickshell.Services.Pipewire
import QtQuick
import WaylandCanvas.Ui

// The audio panel: the default sink's volume and mute, and the output device
// to make default.
PanelCard {
	id: root

	panelWidth: 280

	readonly property var sink: Pipewire.defaultAudioSink

	// Every output device: the sinks that are not an application's stream.
	readonly property var sinks: {
		var list = []
		var nodes = Pipewire.nodes ? Pipewire.nodes.values : []
		for (var i = 0; i < nodes.length; i++) {
			if (nodes[i].isSink && !nodes[i].isStream) {
				list.push(nodes[i])
			}
		}
		return list
	}

	// A node's volume, mute and names are filled in only while it is bound.
	PwObjectTracker {
		objects: root.sinks
	}

	function sinkLabel(node) {
		if (node == null) {
			return ""
		}
		var name = node.nickname || node.description || node.name || ""
		return String(name)
	}

	PanelSlider {
		value: root.sink && root.sink.audio ? root.sink.audio.volume : 0
		onMoved: function(v) {
			if (root.sink != null && root.sink.audio != null) {
				root.sink.audio.volume = v
			}
		}
	}

	PanelRow {
		label: "Mute"
		onClicked: if (root.sink != null && root.sink.audio != null)
			root.sink.audio.muted = !root.sink.audio.muted

		PanelSwitch {
			anchors.right: parent.right
			anchors.rightMargin: 8
			anchors.verticalCenter: parent.verticalCenter
			checked: root.sink != null && root.sink.audio != null && root.sink.audio.muted
			onToggled: if (root.sink != null && root.sink.audio != null)
				root.sink.audio.muted = !root.sink.audio.muted
		}
	}

	PanelText {
		dim: true
		width: parent.width
		height: UiTheme.sectionHeight
		text: "Output device"
	}

	Repeater {
		model: root.sinks

		PanelRow {
			id: row
			required property var modelData
			readonly property var node: modelData
			readonly property bool isDefault: root.sink != null && node === root.sink
			label: root.sinkLabel(node)
			marked: isDefault
			onClicked: Pipewire.preferredDefaultAudioSink = node

			Text {
				visible: row.isDefault
				anchors.right: parent.right
				anchors.rightMargin: 8
				anchors.verticalCenter: parent.verticalCenter
				text: "● default"
				color: UiTheme.muted
				font {
					family: UiTheme.fontFamily
					pixelSize: UiTheme.fontBody
				}
				renderType: Text.NativeRendering
			}
		}
	}

	PanelText {
		visible: root.sinks.length === 0
		dim: true
		width: parent.width
		height: UiTheme.sectionHeight
		text: "No output devices"
	}
}
