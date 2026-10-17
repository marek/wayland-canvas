import QtQuick
import WaylandCanvas.Ui
import "."

// The settings panel: canvas.conf's shortcuts, commands and gestures
// through the compositor's socket, the shell's own bar and toolbar
// through api.shell — one card, four tabs. It follows the `settings`
// event, so a hand edit saved under it shows at once; a set or a
// recording that fails shows its error on the row that made it, and
// closing the panel cancels a recording under way, so the compositor
// never holds the keyboard for a closed window. The Shortcuts list
// scrolls: the registry's every action is taller than an output.
PanelCard {
	id: root

	panelWidth: 360
	panelName: "settings"

	// ---- the settings object: asked once, kept current by the event ----

	property var settings: null // the settings object (docs/intents/platform/ipc.md)
	property string settingsError: "" // settings.get itself failed

	function assignSettings(result) {
		var next = {}
		for (var key in result) {
			if (key !== "moved") {
				next[key] = result[key]
			}
		}
		settings = next
	}

	// The panel's api arrives after the instance is created (the host
	// hands it over once its own wiring is done), so the first read rides
	// the opening, not the completion. Closing cancels a recording under
	// way: the compositor never holds the keyboard for a closed window.
	onOpenChanged: {
		if (open) {
			refresh()
		} else if (recording != null) {
			cancelRecording()
		}
	}

	function refresh() {
		api.compositor.call("settings.get", undefined, function(result, error) {
			if (error !== undefined && error !== null && error !== "") {
				settingsError = String(error)
				return
			}
			settingsError = ""
			assignSettings(result)
		})
	}

	Connections {
		target: api != null ? api.compositor : null
		function onEvent(name, params) {
			if (name === "settings") {
				settingsError = ""
				settings = params
			}
		}
	}

	// ---- recording a chord ----

	property var recording: null // {scope, action} while a capture runs


	function cancelRecording() {
		if (recording == null) {
			return
		}
		recording = null
		api.compositor.call("shortcut.cancel", undefined, function() {})
	}

	// Arms the capture for one row; a new call supersedes the one under
	// way (the compositor cancels it), and its late answer is dropped.
	function capture(scope, action, onKeys) {
		cancelRecording()
		recording = {scope: scope, action: action}
		api.compositor.call("shortcut.capture", {scope: scope}, function(result, error) {
			if (recording == null || recording.scope !== scope
					|| recording.action !== action) {
				return
			}
			recording = null
			if (error !== undefined && error !== null && error !== "") {
				onKeys(null, String(error))
				return
			}
			onKeys(result != null ? result.keys : null, "")
		})
	}

	// ---- the Shortcuts tab ----

	// The fault line and the moved-chord line: one row each, the latest.
	property var faultRow: null // {scope, action} or {what} or {scope: "commands"}
	property string faultText: ""
	property var movedRow: null // {scope, action} of the last successful set
	property string movedText: ""

	function rowFault(entry) {
		return faultRow != null && faultRow.scope === entry.scope
			&& faultRow.action === entry.action ? faultText : ""
	}

	function rowNote(entry) {
		return movedRow != null && movedRow.scope === entry.scope
			&& movedRow.action === entry.action ? movedText : ""
	}

	// Where a moved chord came from: the loser action's label, or the
	// command text when the loser was a [commands] entry.
	function movedFrom(moved) {
		var shortcuts = settings && settings.shortcuts ? settings.shortcuts : []
		for (var i = 0; i < shortcuts.length; i++) {
			if (shortcuts[i].scope === moved.scope
					&& shortcuts[i].action === moved.action) {
				return shortcuts[i].label
			}
		}
		return "the command " + (moved.command || "")
	}

	function setKeys(scope, action, keys) {
		api.compositor.call("settings.set",
			{scope: scope, action: action, keys: keys},
			function(result, error) {
				if (error !== undefined && error !== null && error !== "") {
					faultRow = {scope: scope, action: action}
					faultText = String(error)
					return
				}
				assignSettings(result)
				movedRow = {scope: scope, action: action}
				movedText = ""
				var moved = result != null ? result.moved : null
				for (var i = 0; moved != null && i < moved.length; i++) {
					movedText += (movedText === "" ? "" : "  ")
						+ (moved[i].keys || []).join(", ") + " taken from "
						+ movedFrom(moved[i])
				}
			})
	}

	// The groups in registry order, overview last, each with its rows.
	function groups() {
		var out = []
		var byName = ({})
		var shortcuts = settings && settings.shortcuts ? settings.shortcuts : []
		for (var i = 0; i < shortcuts.length; i++) {
			var entry = shortcuts[i]
			if (byName[entry.group] == null) {
				byName[entry.group] = {name: entry.group, rows: []}
				out.push(byName[entry.group])
			}
			byName[entry.group].rows.push(entry)
		}
		return out
	}

	// The Shortcuts tab's first lines: the file's parse errors, then
	// the socket's own fault — one list, so a quiet file shows nothing.
	function errorLines() {
		var out = []
		if (settingsError !== "") {
			out.push({line: 0, text: settingsError})
		}
		var errors = settings && settings.errors ? settings.errors : []
		for (var i = 0; i < errors.length; i++) {
			out.push(errors[i])
		}
		return out
	}

	// ---- the commands, one section of the Shortcuts tab ----

	// A command being added: the file never holds half of it. The chord
	// and the command line wait here until both are given; one write
	// then adds the line.
	property var draft: null // {keys, command}, either still ""

	function commands() {
		var real = settings && settings.commands ? settings.commands : []
		return draft != null ? real.concat([draft]) : real
	}

	function commandCount() {
		return settings && settings.commands ? settings.commands.length : 0
	}

	function clearCommandsFault() {
		if (faultRow != null && faultRow.scope === "commands") {
			faultRow = null
		}
	}

	function saveCommands(commands) {
		api.compositor.call("settings.set", {commands: commands},
			function(result, error) {
				if (error !== undefined && error !== null && error !== "") {
					faultRow = {scope: "commands"}
					faultText = String(error)
					return
				}
				clearCommandsFault()
				assignSettings(result)
			})
	}

	// The draft's command line is what the field holds when its recording
	// starts: set in place, the row stays up with the text in it until
	// the chord arrives.
	function holdDraftText(index, text) {
		if (draft != null && index === commandCount()) {
			draft.command = text
		}
	}

	function commandKeys(index, keys) {
		var commands = settings && settings.commands ? settings.commands.slice() : []
		if (draft != null && index === commands.length) {
			clearCommandsFault()
			draft = {keys: keys, command: draft.command}
			if (draft.command.trim() !== "") {
				commands.push({keys: keys, command: draft.command})
				saveCommands(commands)
			}
			return
		}
		if (index < commands.length) {
			commands[index] = {keys: keys, command: commands[index].command}
			saveCommands(commands)
		}
	}

	function commandText(index, text) {
		if (text.trim() === "") {
			return // an emptied field is not a command; the file keeps its line
		}
		var commands = settings && settings.commands ? settings.commands.slice() : []
		if (draft != null && index === commands.length) {
			draft = {keys: draft.keys, command: text}
			if (draft.keys == null || draft.keys === "") {
				faultRow = {scope: "commands"}
				faultText = "Record a chord first"
				return
			}
			commands.push({keys: draft.keys, command: text})
			saveCommands(commands)
			return
		}
		if (index < commands.length) {
			commands[index] = {keys: commands[index].keys, command: text}
			saveCommands(commands)
		}
	}

	// A recorded command lands in the file: the draft it filled is the
	// real row now. Watch the settings, not the draft, for when that is.
	onSettingsChanged: if (draft != null && draft.keys != null && draft.keys !== "") {
		var real = settings && settings.commands ? settings.commands : []
		for (var i = 0; i < real.length; i++) {
			if (real[i].keys === draft.keys) {
				draft = null
				break
			}
		}
	}

	function removeCommand(index) {
		if (draft != null) {
			if (index === commandCount()) {
				draft = null
				return
			}
			draft = null // a remove also drops an unfilled draft
		}
		var commands = settings && settings.commands ? settings.commands.slice() : []
		if (index < commands.length) {
			commands.splice(index, 1)
			saveCommands(commands)
		}
	}

	// ---- the Gestures tab ----

	function setGesture(what, value) {
		var params = {}
		params[what] = value
		api.compositor.call("settings.set", params, function(result, error) {
			if (error !== undefined && error !== null && error !== "") {
				faultRow = {what: what}
				faultText = String(error)
				return
			}
			faultRow = null
			assignSettings(result)
		})
	}

	readonly property bool pinchScale: !(settings && settings.gestures)
		|| settings.gestures.pinchScale !== false
	readonly property bool invert: settings && settings.gestures
		? settings.gestures.invert === true : false

	// ---- the Session tab ----

	// canvas.conf's [session] values, as settings.get reports them.
	readonly property int lockIdleMinutes: settings && settings.session
		? settings.session.lockIdleMinutes : 5
	readonly property bool nightLight: settings && settings.session
		? settings.session.nightLight === true : false
	readonly property int nightLightTemperature: settings && settings.session
		? settings.session.nightLightTemperature : 4000

	// The idle timeouts the page offers, in minutes; 0 is never.
	readonly property var idleChoices: [1, 5, 10, 30, 0]

	function setSession(what, value) {
		var params = {}
		params[what] = value
		api.compositor.call("settings.set", params, function(result, error) {
			if (error !== undefined && error !== null && error !== "") {
				faultRow = {what: what}
				faultText = String(error)
				return
			}
			faultRow = null
			assignSettings(result)
		})
	}

	function idleLabel(minutes) {
		if (minutes === 0) {
			return "Never"
		}
		return minutes === 1 ? "After a minute" : "After " + minutes + " minutes"
	}

	// ---- the Toolbar and Bar tabs, through api.shell ----

	readonly property string toolbarPosition: api != null && api.shell != null
		? api.shell.toolbarPosition : "right"
	readonly property var barWidgets: api != null && api.shell != null
		? api.shell.barWidgets : []

	function setEdge(edge) {
		if (api != null && api.shell != null) {
			api.shell.setToolbarPosition(edge)
		}
	}

	function setHidden(name, hidden) {
		if (api != null && api.shell != null) {
			api.shell.setBarHidden(name, hidden)
		}
	}
	// The most the scrollable list may take: the tall card is clamped to
	// the output's top, so what is left beside the tabs, the separator
	// and the footer is every pixel the list has. Fixed metrics only —
	// the card's geometry must not drift with the font.
	readonly property real listMaxHeight: screenH - 2 * UiTheme.edgeMargin
		- 2 * UiTheme.panelPadding - UiTheme.rowHeight - 3 * UiTheme.panelSpacing
		- footerHeight - 1

	property string tab: "shortcuts"

	PanelTabs {
		tabs: [
			{id: "shortcuts", label: "Shortcuts"},
			{id: "gestures", label: "Gestures"},
			{id: "session", label: "Session"},
			{id: "toolbar", label: "Toolbar"},
			{id: "bar", label: "Bar"},
		]
		current: root.tab
		onChosen: function(id) { root.tab = id }
	}

	Loader {
		width: parent.width
		sourceComponent: root.tab === "shortcuts" ? shortcutsTab
			: root.tab === "gestures" ? gesturesTab
			: root.tab === "session" ? sessionTab
			: root.tab === "toolbar" ? toolbarTab : barTab
	}

	// Both files the panel edits, by path: the compositor's and the
	// shell's own. Two fixed-height lines, like every metric the card's
	// geometry is computed from.
	readonly property int footerHeight: 2 * UiTheme.detailHeight + 2

	PanelSeparator {}

	Column {
		width: parent.width
		spacing: 2

		PanelText {
			height: UiTheme.detailHeight
			verticalAlignment: Text.AlignVCenter
			width: parent.width
			elide: Text.ElideRight
			dim: true
			font.pixelSize: UiTheme.fontSmall
			text: root.settings && root.settings.path ? root.settings.path : ""
		}

		PanelText {
			height: UiTheme.detailHeight
			verticalAlignment: Text.AlignVCenter
			width: parent.width
			elide: Text.ElideRight
			dim: true
			font.pixelSize: UiTheme.fontSmall
			text: api != null && api.shell != null ? api.shell.configPath : ""
		}
	}

	// ---- Shortcuts ----

	Component {
		id: shortcutsTab

		Flickable {
			id: list
			width: parent.width
			implicitHeight: Math.min(contentHeight, root.listMaxHeight)
			height: implicitHeight
			contentWidth: width
			contentHeight: column.implicitHeight
			clip: true

			Column {
				id: column
				width: list.width
				spacing: UiTheme.panelSpacing

				// The file's own complaints first: every parse error the
				// reload found, and the socket's own fault when
				// settings.get failed. Nothing to say: no line at all.
				Repeater {
					model: root.errorLines()

					PanelText {
						required property var modelData
						height: UiTheme.detailHeight
						verticalAlignment: Text.AlignVCenter
						width: parent.width
						elide: Text.ElideRight
						color: UiTheme.attention
						text: modelData.line > 0
							? "canvas.conf line " + modelData.line + ": " + modelData.text
							: modelData.text
					}
				}


				Repeater {
					model: root.groups()

					Column {
						id: group
						required property var modelData
						width: parent.width
						spacing: UiTheme.panelSpacing

						PanelSectionHeader {
							text: group.modelData.name
						}

						Repeater {
							model: group.modelData.rows

							ShortcutRow {
								id: actionRow
								required property var modelData
								entry: modelData
								recording: root.recording != null
									&& root.recording.scope === modelData.scope
									&& root.recording.action === modelData.action
								note: root.rowNote(modelData)
								fault: root.rowFault(modelData)
								onRecord: root.capture(modelData.scope, modelData.action,
									function(keys, fault) {
										if (fault !== "") {
											root.faultRow = {scope: modelData.scope, action: modelData.action}
											root.faultText = fault
											return
										}
										if (keys != null) {
											root.setKeys(modelData.scope, modelData.action, [keys])
										}
									})
								onReset: root.setKeys(modelData.scope, modelData.action, null)
								onClear: root.setKeys(modelData.scope, modelData.action, [])
							}
						}
					}
				}

				PanelSectionHeader {
					text: "Commands"
				}

				Repeater {
					model: root.commands()

					CommandRow {
						id: commandRow
						required property var modelData
						required property int index
						command: modelData
						draft: root.draft != null && index === root.commandCount()
						recording: root.recording != null
							&& root.recording.action === "command" + index
						fault: root.faultRow != null && root.faultRow.scope === "commands"
							? root.faultText : ""
						onRecord: function(text) {
							var at = commandRow.index
							root.holdDraftText(at, text)
							root.capture("global", "command" + at, function(keys, fault) {
								if (fault !== "") {
									root.faultRow = {scope: "commands"}
									root.faultText = fault
									return
								}
								if (keys != null) {
									root.commandKeys(at, keys)
								}
							})
						}
						onSave: function(text) { root.commandText(commandRow.index, text) }
						onRemove: root.removeCommand(commandRow.index)
					}
				}

				PanelRow {
					glyph: "+"
					label: "Add a command"
					onClicked: root.draft = {keys: "", command: ""}
				}
			}
		}
	}

	// ---- Gestures ----

	Component {
		id: gesturesTab

		Column {
			width: parent.width
			spacing: UiTheme.panelSpacing

			PanelSectionHeader {
				text: "Swipes"
			}

			ChoiceRow {
				label: "Three-finger hops"
				description: "3 fingers hop, 4 open the overview"
				chosen: root.settings && root.settings.gestures
					&& root.settings.gestures.swipes === "three-finger-hops"
				note: root.faultRow != null && root.faultRow.what === "swipes"
					? root.faultText : ""
				onPicked: root.setGesture("swipes", "three-finger-hops")
			}

			ChoiceRow {
				label: "Four-finger hops"
				description: "4 fingers hop, 3 open the overview"
				chosen: root.settings && root.settings.gestures
					&& root.settings.gestures.swipes === "four-finger-hops"
				note: root.faultRow != null && root.faultRow.what === "swipes"
					? root.faultText : ""
				onPicked: root.setGesture("swipes", "four-finger-hops")
			}

			ChoiceRow {
				label: "Off"
				description: "No 3- or 4-finger compositor gesture"
				chosen: root.settings && root.settings.gestures
					&& root.settings.gestures.swipes === "off"
				note: root.faultRow != null && root.faultRow.what === "swipes"
					? root.faultText : ""
				onPicked: root.setGesture("swipes", "off")
			}

			PanelSectionHeader {
				text: "Pinch"
			}

			PanelRow {
				label: "Pinch to scale a desktop"
				detail: "Super + 2 fingers"
				clickable: false

				PanelSwitch {
					anchors.right: parent.right
					anchors.rightMargin: 8
					anchors.verticalCenter: parent.verticalCenter
					checked: root.pinchScale
					onToggled: root.setGesture("pinchScale", !root.pinchScale)
				}
			}

			PanelSectionHeader {
				text: "Direction"
			}

			PanelRow {
				label: "Invert scrolling and swipes"
				// A write that failed says why in place of the explanation.
				detail: root.faultRow != null && root.faultRow.what === "invert"
					? root.faultText
					: "The pointer, the touchpad and the hops together"
				marked: root.faultRow != null && root.faultRow.what === "invert"
				clickable: false

				PanelSwitch {
					anchors.right: parent.right
					anchors.rightMargin: 8
					anchors.verticalCenter: parent.verticalCenter
					checked: root.invert
					onToggled: root.setGesture("invert", !root.invert)
				}
			}
		}
	}

	// ---- Session ----

	Component {
		id: sessionTab

		Column {
			width: parent.width
			spacing: UiTheme.panelSpacing

			PanelSectionHeader {
				text: "Lock the screen"
			}

			Repeater {
				model: root.idleChoices

				delegate: ChoiceRow {
					required property int modelData

					label: root.idleLabel(modelData)
					description: modelData === 0
						? "The screen never locks on its own" : "Of no input"
					chosen: root.lockIdleMinutes === modelData
					note: root.faultRow != null
						&& root.faultRow.what === "lockIdleMinutes"
						? root.faultText : ""
					onPicked: root.setSession("lockIdleMinutes", modelData)
				}
			}

			PanelSectionHeader {
				text: "Night light"
			}

			PanelRow {
				label: "Warm the screen"
				detail: root.nightLightTemperature + " K"
				clickable: false

				PanelSwitch {
					anchors.right: parent.right
					anchors.rightMargin: 8
					anchors.verticalCenter: parent.verticalCenter
					checked: root.nightLight
					onToggled: root.setSession("nightLight", !root.nightLight)
				}
			}

			// 1000 K to 6500 K, warm at the left.
			PanelSlider {
				value: (root.nightLightTemperature - 1000) / 5500
				onMoved: function(v) {
					root.setSession("nightLightTemperature",
						Math.round(1000 + v * 5500))
				}
			}
		}
	}

	// ---- Toolbar ----

	Component {
		id: toolbarTab

		Column {
			width: parent.width
			spacing: UiTheme.panelSpacing

			PanelSectionHeader {
				text: "The toolbar's edge"
			}

			ChoiceRow {
				label: "Right"
				description: "A column on the right edge"
				chosen: root.toolbarPosition === "right"
				onPicked: root.setEdge("right")
			}

			ChoiceRow {
				label: "Left"
				description: "A column on the left edge"
				chosen: root.toolbarPosition === "left"
				onPicked: root.setEdge("left")
			}

			ChoiceRow {
				label: "Top"
				description: "A row under the bar"
				chosen: root.toolbarPosition === "top"
				onPicked: root.setEdge("top")
			}

			ChoiceRow {
				label: "Bottom"
				description: "A row above the bottom edge"
				chosen: root.toolbarPosition === "bottom"
				onPicked: root.setEdge("bottom")
			}
		}
	}

	// ---- Bar ----

	Component {
		id: barTab

		Column {
			width: parent.width
			spacing: UiTheme.panelSpacing

			Repeater {
				model: ["left", "center", "right"]

				Column {
					id: section
					required property string modelData
					width: parent.width
					spacing: UiTheme.panelSpacing

					PanelSectionHeader {
						text: section.modelData === "left" ? "Left"
							: section.modelData === "center" ? "Center" : "Right"
					}

					Repeater {
						model: root.barWidgets.filter(function(w) {
							return w.section === section.modelData
						})

						PanelRow {
							id: widgetRow
							required property var modelData
							label: modelData.pluginName
							detail: modelData.name === "canvas.agents/agents"
								? "Shows while an AI tool has usage" : ""
							marked: modelData.hidden
							clickable: false

							PanelSwitch {
								anchors.right: parent.right
								anchors.rightMargin: 8
								anchors.verticalCenter: parent.verticalCenter
								checked: !widgetRow.modelData.hidden
								onToggled: root.setHidden(widgetRow.modelData.name,
									!widgetRow.modelData.hidden)
							}
						}
					}
				}
			}
		}
	}
}
