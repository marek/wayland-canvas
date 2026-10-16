import QtQuick
import WaylandCanvas.Ui

// The agents panel: one tab per AI provider family. Claude and GPT carry
// their limits windows beside the token figures; every other family with
// usage shows tokens only. The hero's mark is the provider's own — the
// Claude starburst, the GPT hexagon — with the plan (or a fault) as the
// meta line; a fault also brings the sign-in card with the CLI command
// that restores authoritative usage. The day and model figures are a
// read-out; the only controls are the provider switch's equal chips under
// the hero — any number of families fits the card.
PanelCard {
	id: root

	panelWidth: 360
	panelName: "agents"

	// The tab the switch is on; the panel opens on the first provider and
	// keeps its tab across refreshes while it still exists.
	property int current: 0
	readonly property var providers: model ? model.providers : []
	readonly property var provider: current >= 0 && current < providers.length
		? providers[current] : null
	// The countdowns' clock: bumped every half minute while the panel is
	// open, so "Resets in 4h 12m" stays true without a re-probe.
	property real nowMs: Date.now()

	onOpenChanged: if (open) refreshOnOpen()

	function refreshOnOpen() {
		if (current >= providers.length) {
			current = Math.max(providers.length - 1, 0)
		}
		nowMs = Date.now()
		if (model != null) {
			model.refresh()
		}
	}

	Timer {
		interval: 30000
		repeat: true
		running: root.open
		onTriggered: root.nowMs = Date.now()
	}

	PanelText {
		visible: root.providers.length === 0
		dim: true
		width: parent.width
		height: UiTheme.sectionHeight
		text: "No agent usage yet"
	}

	PanelHero {
		visible: root.provider != null
		title: root.provider ? root.provider.title : ""
		meta: root.provider ? heroMeta(root.provider) : ""
		metaAttention: root.provider != null
			&& root.provider.usageStatusText !== ""
		iconComponent: root.provider == null ? null
			: root.provider.id === "claude" || root.provider.id === "anthropic"
			? claudeMark
			: root.provider.id === "gpt" ? gptMark
			: glyphMark

	}

	// The provider switch: a full-width row of equal chips, so any number
	// of families fits the card — the current one held in the accent's
	// tint.
	Row {
		id: providerSwitch
		visible: root.providers.length > 1
		width: parent.width
		spacing: UiTheme.panelSpacing

		readonly property real chipWidth: root.providers.length > 0
			? (width - spacing * (root.providers.length - 1))
				/ root.providers.length
			: 0

		Repeater {
			model: root.providers

			Rectangle {
				id: chip
				required property var modelData
				required property int index
				height: 24
				width: providerSwitch.chipWidth
				radius: 12
				color: chip.index === root.current ? UiTheme.selected : UiTheme.fill

				Text {
					id: chipText
					anchors.centerIn: parent
					width: parent.width - 12
					horizontalAlignment: Text.AlignHCenter
					textFormat: Text.PlainText
					renderType: Text.NativeRendering
					elide: Text.ElideRight
					text: chip.modelData.name
					color: UiTheme.text
					font {
						family: UiTheme.fontFamily
						pixelSize: UiTheme.fontSmall
						bold: chip.index === root.current
					}
				}

				MouseArea {
					anchors.fill: parent
					onClicked: root.current = chip.index
				}
			}
		}
	}

	// The sign-in card: a fault in the meta line always comes with the
	// command that fixes it.
	Rectangle {
		visible: root.provider != null && root.provider.usageStatusText !== ""
		width: parent.width
		height: 48
		radius: 4
		color: Qt.rgba(UiTheme.attention.r, UiTheme.attention.g,
			UiTheme.attention.b, UiTheme.light ? 0.10 : 0.14)

		Text {
			anchors.fill: parent
			anchors.leftMargin: 8
			anchors.rightMargin: 8
			textFormat: Text.PlainText
			renderType: Text.NativeRendering
			wrapMode: Text.WordWrap
			verticalAlignment: Text.AlignVCenter
			text: root.provider ? root.provider.authHelpText : ""
			color: UiTheme.attention
			font {
				family: UiTheme.fontFamily
				pixelSize: UiTheme.fontBody
			}
		}
	}

	PanelSeparator {
		visible: root.provider != null
	}

	PanelSectionHeader {
		visible: root.provider != null && root.provider.limits.length > 0
		text: "Limits"
	}

	Repeater {
		model: root.provider ? root.provider.limits : []

		// One window: its name and share, the meter, and the moment it
		// resets — the fixed 39 px the scenario reads band by band.
		Item {
			id: window
			required property var modelData
			readonly property var limit: modelData
			readonly property bool spent: limit.percent >= 0.9
			width: parent.width
			height: 39

			Text {
				text: window.limit.title
				color: UiTheme.text
				font {
					family: UiTheme.fontFamily
					pixelSize: UiTheme.fontBody
				}
				renderType: Text.NativeRendering
			}

			Text {
				anchors.right: parent.right
				text: Math.round(window.limit.percent * 100) + "%"
				color: window.spent ? UiTheme.attention : UiTheme.text
				font {
					family: UiTheme.fontFamily
					pixelSize: UiTheme.fontBody
					bold: true
				}
				renderType: Text.NativeRendering
			}

			// The meter: the track in the panel's own grey, the share in
			// the accent — the attention colour once it is nearly spent.
			Rectangle {
				id: track
				y: 19
				width: parent.width
				height: 5
				radius: 2
				color: UiTheme.track

				Rectangle {
					width: Math.max(0, Math.min(window.limit.percent, 1))
						* parent.width
					height: parent.height
					radius: 2
					color: window.spent ? UiTheme.attention : UiTheme.accent
				}
			}

			Text {
				y: 27
				width: parent.width
				text: root.resetsIn(window.limit.resetsAt)
				color: UiTheme.muted
				font {
					family: UiTheme.fontFamily
					pixelSize: UiTheme.fontSmall
				}
				renderType: Text.NativeRendering
			}
		}
	}

	PanelSectionHeader {
		visible: root.provider != null
		text: "Tokens by day"
	}

	Column {
		visible: root.provider != null
		width: parent.width
		spacing: 2

		Repeater {
			model: root.provider ? root.provider.days : []

			// One day: the weekday on the left, the tokens on the right;
			// today's row is the one drawn bold.
			Item {
				id: day
				required property var modelData
				width: parent.width
				height: 22

				Text {
					text: day.modelData.label
					color: day.modelData.today ? UiTheme.text : UiTheme.muted
					font {
						family: UiTheme.fontFamily
						pixelSize: UiTheme.fontBody
						bold: day.modelData.today
					}
					renderType: Text.NativeRendering
				}

				Text {
					anchors.right: parent.right
					text: day.modelData.tokensText
					color: day.modelData.today ? UiTheme.text : UiTheme.muted
					font {
						family: UiTheme.fontFamily
						pixelSize: UiTheme.fontBody
						bold: day.modelData.today
					}
					renderType: Text.NativeRendering
				}
			}
		}
	}

	PanelSectionHeader {
		visible: root.provider != null
		text: "Tokens by model"
	}

	Column {
		visible: root.provider != null
		width: parent.width
		spacing: 4

		Repeater {
			model: root.provider ? root.provider.models : []

			// One model: its share of the heaviest model behind the row,
			// the display name on the left, the total on the right.
			Item {
				id: modelRow
				required property var modelData
				readonly property var heaviest: root.provider
					&& root.provider.models.length > 0
					? root.provider.models[0].total : 0
				width: parent.width
				height: 26

				Rectangle {
					width: modelRow.heaviest > 0
						? Math.max(0, Math.min(modelRow.modelData.total
							/ modelRow.heaviest, 1)) * parent.width : 0
					height: parent.height
					radius: 3
					color: UiTheme.fill
				}

				Text {
					x: 6
					anchors.verticalCenter: parent.verticalCenter
					text: modelRow.modelData.name
					color: UiTheme.text
					font {
						family: UiTheme.fontFamily
						pixelSize: UiTheme.fontBody
					}
					renderType: Text.NativeRendering
				}

				Text {
					anchors.right: parent.right
					anchors.rightMargin: 6
					anchors.verticalCenter: parent.verticalCenter
					text: modelRow.modelData.totalText
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

	// The provider's own mark in the hero: Claude's starburst in its clay
	// orange, GPT's hexagon knot in its green, a star for every other
	// family — drawn, not shipped, so the panel carries no assets.
	Component {
		id: claudeMark

		Canvas {
			anchors.margins: 2
			onPaint: {
				var ctx = getContext("2d")
				ctx.clearRect(0, 0, width, height)
				ctx.strokeStyle = "#d97757"
				ctx.lineWidth = width / 9
				ctx.lineCap = "round"
				var cx = width / 2
				var cy = height / 2
				// The asterisk: eight rays out of the centre, the way the
				// Claude mark reads at glyph size.
				for (var i = 0; i < 8; i++) {
					var angle = Math.PI / 8 + i * Math.PI / 4
					ctx.beginPath()
					ctx.moveTo(cx + Math.cos(angle) * width * 0.10,
						cy + Math.sin(angle) * height * 0.10)
					ctx.lineTo(cx + Math.cos(angle) * width * 0.42,
						cy + Math.sin(angle) * height * 0.42)
					ctx.stroke()
				}
			}
		}
	}

	Component {
		id: gptMark

		Canvas {
			anchors.margins: 3
			onPaint: {
				var ctx = getContext("2d")
				ctx.clearRect(0, 0, width, height)
				ctx.strokeStyle = "#10a37f"
				ctx.lineWidth = width / 10
				ctx.lineJoin = "round"
				var cx = width / 2
				var cy = height / 2
				var r = width * 0.40
				ctx.beginPath()
				for (var i = 0; i < 6; i++) {
					var angle = Math.PI / 6 + i * Math.PI / 3
					var x = cx + Math.cos(angle) * r
					var y = cy + Math.sin(angle) * r
					if (i === 0) {
						ctx.moveTo(x, y)
					} else {
						ctx.lineTo(x, y)
					}
				}
				ctx.closePath()
				ctx.stroke()
			}
		}
	}

	Component {
		id: glyphMark

		Text {
			text: "★"
			textFormat: Text.PlainText
			renderType: Text.NativeRendering
			horizontalAlignment: Text.AlignHCenter
			verticalAlignment: Text.AlignVCenter
			color: UiTheme.accent
			font {
				family: UiTheme.fontFamily
				pixelSize: UiTheme.heroIcon - 4
			}
		}
	}

	// The meta line: a fault first, then the plan beside the week's
	// tokens — the two things the hero of a usage panel is for.
	function heroMeta(provider) {
		if (provider.usageStatusText !== "") {
			return provider.usageStatusText
		}
		var week = 0
		for (var i = 0; i < provider.days.length; i++) {
			week += provider.days[i].tokens
		}
		var label = "7 Days " + formatTokens(week)
		if (provider.tierLabel !== "") {
			return provider.tierLabel + " · " + label
		}
		return label
	}

	function formatTokens(tokens) {
		if (tokens < 1000) {
			return String(tokens)
		}
		var units = ["K", "M", "B"]
		var value = tokens
		var unit = -1
		while (value >= 1000 && unit < units.length - 1) {
			value /= 1000
			unit++
		}
		return Math.round(value * 10) / 10 + units[unit]
	}

	// "Resets in 4h 12m" from the window's own reset time; a window with
	// no readable time says nothing rather than guess.
	function resetsIn(iso) {
		if (iso == null || iso === "") {
			return ""
		}
		var ms = Date.parse(iso) - nowMs
		if (isNaN(ms)) {
			return ""
		}
		if (ms <= 0) {
			return "Resets soon"
		}
		var minutes = Math.floor(ms / 60000)
		var hours = Math.floor(minutes / 60)
		var days = Math.floor(hours / 24)
		if (days > 0) {
			return "Resets in " + days + "d " + (hours % 24) + "h"
		}
		if (hours > 0) {
			return "Resets in " + hours + "h " + (minutes % 60) + "m"
		}
		return "Resets in " + minutes + "m"
	}
}
