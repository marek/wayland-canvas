import QtQuick
import WaylandCanvas.Ui
import "."

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

	// The provider the switch is on, by id: the collector reorders the
	// families after Claude and GPT by their week's tokens, so a position
	// would show another family after a refresh. The panel opens on the
	// first provider and keeps its choice while that provider exists.
	property string currentId: ""
	readonly property var providers: AgentsState.providers
	readonly property int current: {
		for (var i = 0; i < providers.length; i++) {
			if (providers[i].id === currentId) {
				return i
			}
		}
		return 0
	}
	readonly property var provider: current < providers.length
		? providers[current] : null
	// The hero's mark per provider id; any other family gets the star.
	readonly property var marks: ({
		"claude": claudeMark,
		"anthropic": claudeMark,
		"gpt": gptMark
	})
	// The countdowns' clock: bumped every half minute while the panel is
	// open, so "Resets in 4h 12m" stays true without a re-probe.
	property real nowMs: Date.now()

	onOpenChanged: if (open) refreshOnOpen()

	function refreshOnOpen() {
		nowMs = Date.now()
		AgentsState.refresh()
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
			: root.marks[root.provider.id] || glyphMark
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

				PanelText {
					id: chipText
					anchors.centerIn: parent
					width: parent.width - 12
					horizontalAlignment: Text.AlignHCenter
					elide: Text.ElideRight
					text: chip.modelData.name
					font.pixelSize: UiTheme.fontSmall
					font.bold: chip.index === root.current
				}

				MouseArea {
					anchors.fill: parent
					onClicked: root.currentId = chip.modelData.id
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
		radius: UiTheme.radius
		color: Qt.rgba(UiTheme.attention.r, UiTheme.attention.g,
			UiTheme.attention.b, UiTheme.light ? 0.10 : 0.14)

		PanelText {
			anchors.fill: parent
			anchors.leftMargin: UiTheme.rowInset
			anchors.rightMargin: UiTheme.rowInset
			wrapMode: Text.WordWrap
			verticalAlignment: Text.AlignVCenter
			text: root.provider ? root.provider.authHelpText : ""
			color: UiTheme.attention
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

			PanelText {
				text: window.limit.title
			}

			PanelText {
				anchors.right: parent.right
				text: Math.round(window.limit.percent * 100) + "%"
				color: window.spent ? UiTheme.attention : UiTheme.text
				font.bold: true
			}

			// The share in the accent — the attention colour once it is
			// nearly spent.
			PanelMeter {
				y: 19
				value: window.limit.percent
				fill: window.spent ? UiTheme.attention : UiTheme.accent
			}

			PanelText {
				y: 27
				width: parent.width
				dim: true
				text: root.resetsIn(window.limit.resetsAt)
				font.pixelSize: UiTheme.fontSmall
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

			// One day: the weekday on the left, a bar scaled to the
			// busiest day of the seven, the tokens on the right; today's
			// row is the one drawn bold, its fill the brighter text
			// colour where the rest carry the muted one.
			Item {
				id: day
				required property var modelData
				readonly property real busiest: root.provider
					&& root.provider.days ? Math.max(
					0, ...root.provider.days.map(one => one.tokens)) : 0
				width: parent.width
				height: 22

				PanelText {
					dim: !day.modelData.today
					text: day.modelData.label
					font.bold: day.modelData.today
				}

				PanelMeter {
					x: 60
					width: parent.width - 116
					anchors.verticalCenter: parent.verticalCenter
					value: day.busiest > 0 ? day.modelData.tokens / day.busiest : 0
					fill: day.modelData.today ? UiTheme.text : UiTheme.muted
				}

				PanelText {
					anchors.right: parent.right
					dim: !day.modelData.today
					text: day.modelData.tokensText
					font.bold: day.modelData.today
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

				PanelMeter {
					width: parent.width
					height: parent.height
					radius: 3
					color: "transparent"
					value: modelRow.heaviest > 0
						? modelRow.modelData.total / modelRow.heaviest : 0
					fill: UiTheme.fill
				}

				PanelText {
					x: 6
					anchors.verticalCenter: parent.verticalCenter
					text: modelRow.modelData.name
				}

				PanelText {
					anchors.right: parent.right
					anchors.rightMargin: 6
					anchors.verticalCenter: parent.verticalCenter
					dim: true
					text: modelRow.modelData.totalText
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

		PanelText {
			text: "★"
			horizontalAlignment: Text.AlignHCenter
			verticalAlignment: Text.AlignVCenter
			color: UiTheme.accent
			font.pixelSize: UiTheme.heroIcon - 4
		}
	}

	// The meta line: a fault first, then the plan beside the week's
	// tokens — the two things the hero of a usage panel is for.
	function heroMeta(provider) {
		if (provider.usageStatusText !== "") {
			return provider.usageStatusText
		}
		var label = "7 Days " + provider.weekText
		if (provider.tierLabel !== "") {
			return provider.tierLabel + " · " + label
		}
		return label
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
