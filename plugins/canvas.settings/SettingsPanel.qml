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

	// Wide enough for its tabs' names on one row: six shares of 78 px hold
	// the longest of them ("Shortcuts") without eliding.
	panelWidth: 488

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
		target: api.compositor
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
	// Whether a direction goes `way`; before the settings arrive, its default does.
	function gestureWay(param, way, isDefault) {
		if (!(settings && settings.gestures && settings.gestures[param] !== undefined)) {
			return isDefault
		}
		return settings.gestures[param] === way
	}
	// The direction switches (docs/intents/input/gestures.md): a switch
	// each, on meaning `onWay`, with a line saying which way it moves now.
	readonly property var directions: [
		{param: "mouseScroll", onWay: "natural", offWay: "traditional", onByDefault: true,
			label: "Natural scrolling with a mouse",
			onLine: "The content moves the way the wheel turns",
			offLine: "The content moves against the wheel"},
		{param: "touchpadScroll", onWay: "natural", offWay: "traditional", onByDefault: true,
			label: "Natural scrolling on the touchpad",
			onLine: "The content follows the fingers",
			offLine: "The content moves against the fingers"},
		{param: "pan", onWay: "natural", offWay: "traditional", onByDefault: true,
			label: "Natural looking away",
			onLine: "Super + scroll moves the content the way you scroll",
			offLine: "Super + scroll moves the view the way you scroll"},
		{param: "overviewSwipe", onWay: "down", offWay: "up", onByDefault: false,
			label: "Open the overview swiping down",
			onLine: "Swiping up goes back in",
			offLine: "Swiping up opens it, swiping down goes back in"},
		{param: "hopSwipe", onWay: "away", offWay: "toward", onByDefault: false,
			label: "Swipes push the desktops along",
			onLine: "A swipe left hops to the desktop on the right",
			offLine: "A swipe left hops to the desktop on the left"}
	]

	// ---- the Session tab ----

	// canvas.conf's [session] values, as settings.get reports them.
	readonly property int lockIdleMinutes: settings && settings.session
		? settings.session.lockIdleMinutes : 5

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

	readonly property string toolbarPosition: api.shell.toolbarPosition
	readonly property var barWidgets: api.shell.barWidgets

	function setEdge(edge) {
		api.shell.setToolbarPosition(edge)
	}

	// The app bar shares this page: it is the other strip of the overview
	// (docs/intents/shell/app-bar.md).
	readonly property string appBarPosition: api.shell.appBarPosition
	readonly property bool appBarEnabled: api.shell.appBarEnabled !== false

	function setAppBarEdge(edge) {
		api.shell.setAppBarPosition(edge)
	}

	function setAppBarEnabled(on) {
		api.shell.setAppBarEnabled(on)
	}

	function setHidden(name, hidden) {
		api.shell.setBarHidden(name, hidden)
	}
	// The most the scrollable list may take: the tall card is clamped to
	// the output's top, so what is left beside the tabs, the separator
	// and the footer is every pixel the list has. Fixed metrics only —
	// the card's geometry must not drift with the font.
	readonly property real listMaxHeight: screenH - 2 * UiTheme.edgeMargin
		- 2 * UiTheme.panelPadding - UiTheme.rowHeight - 3 * UiTheme.panelSpacing
		- footerHeight - 1

	property string tab: "shortcuts"

	// The pages plugins declare, each a tab of its own after the fixed ones
	// (docs/intents/shell/plugins.md): the window knows no plugin by name.
	readonly property var pluginPages: api.shell.pluginSettingsPages

	function setPluginSetting(id, key, value) {
		api.shell.setPluginSetting(id, key, value)
	}

	PanelTabs {
		// One tab holds every plugin's own settings: a tab per plugin would
		// grow the row without bound, and the row's width is the card's.
		tabs: {
			var fixed = [
				{id: "shortcuts", label: "Shortcuts"},
				{id: "gestures", label: "Gestures"},
				{id: "session", label: "Session"},
				{id: "toolbar", label: "Toolbar"},
				{id: "bar", label: "Bar"},
			]
			if (root.pluginPages.length > 0) {
				fixed.push({id: "plugins", label: "Plugins"})
			}
			return fixed
		}
		current: root.tab
		onChosen: function(id) { root.tab = id }
	}

	Loader {
		width: parent.width
		sourceComponent: root.tab === "shortcuts" ? shortcutsTab
			: root.tab === "gestures" ? gesturesTab
			: root.tab === "session" ? sessionTab
			: root.tab === "toolbar" ? toolbarTab
			: root.tab === "bar" ? barTab
			: root.tab === "plugins" ? pluginsTab : barTab
	}

	// ---- the plugins' own settings, from their declarations alone ----

	// Every declared setting of every plugin, flattened into rows with the
	// headers that separate them: {header} or {entry, pluginId}. A page
	// built from one model keeps the list scrollable as one.
	readonly property var pluginRows: {
		var rows = []
		for (var i = 0; i < pluginPages.length; i++) {
			var page = pluginPages[i]
			for (var g = 0; g < page.groups.length; g++) {
				var group = page.groups[g]
				rows.push({
					header: group.name === page.name
						? page.name : page.name + " \u00b7 " + group.name,
				})
				for (var e = 0; e < group.entries.length; e++) {
					rows.push({entry: group.entries[e], pluginId: page.id})
				}
			}
		}
		return rows
	}

	Component {
		id: pluginsTab

		Flickable {
			width: parent.width
			implicitHeight: Math.min(contentHeight, root.listMaxHeight)
			height: implicitHeight
			contentWidth: width
			contentHeight: pluginColumn.implicitHeight
			clip: true

			Column {
				id: pluginColumn
				width: parent.width
				spacing: UiTheme.panelSpacing

				Repeater {
					model: root.pluginRows

					Loader {
						required property var modelData

						width: pluginColumn.width
						sourceComponent: modelData.header !== undefined
							? pluginHeader : pluginRow

						property var row: modelData
					}
				}
			}
		}
	}

	Component {
		id: pluginHeader

		PanelSectionHeader {
			text: parent.row.header
		}
	}

	Component {
		id: pluginRow

		PluginSettingRow {
			width: parent.width
			entry: parent.row.entry
			pluginId: parent.row.pluginId
			panel: root
		}
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
			text: api.shell.configPath
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

			Repeater {
				model: root.directions

				delegate: PanelRow {
					id: row
					required property var modelData
					readonly property bool switchedOn: root.gestureWay(modelData.param,
						modelData.onWay, modelData.onByDefault)
					readonly property bool failed: root.faultRow != null
						&& root.faultRow.what === modelData.param

					label: modelData.label
					// A write that failed says why in place of the line.
					detail: failed ? root.faultText
						: switchedOn ? modelData.onLine : modelData.offLine
					marked: failed
					clickable: false

					PanelSwitch {
						anchors.right: parent.right
						anchors.rightMargin: 8
						anchors.verticalCenter: parent.verticalCenter
						checked: row.switchedOn
						onToggled: root.setGesture(row.modelData.param,
							row.switchedOn ? row.modelData.offWay : row.modelData.onWay)
					}
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

			NightLightControl {
				label: "Warm the screen"
				settings: root.settings
				onChose: function(param, value) { root.setSession(param, value) }
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

			PanelSectionHeader {
				text: "The app bar"
			}

			PanelRow {
				label: "Show the app bar"
				detail: "Every open app on an edge of the overview"
				clickable: false

				PanelSwitch {
					anchors.right: parent.right
					anchors.rightMargin: 8
					anchors.verticalCenter: parent.verticalCenter
					checked: root.appBarEnabled
					onToggled: root.setAppBarEnabled(!root.appBarEnabled)
				}
			}

			ChoiceRow {
				label: "Left"
				description: "A column on the left edge"
				chosen: root.appBarPosition === "left"
				onPicked: root.setAppBarEdge("left")
			}

			ChoiceRow {
				label: "Right"
				description: "A column on the right edge"
				chosen: root.appBarPosition === "right"
				onPicked: root.setAppBarEdge("right")
			}

			ChoiceRow {
				label: "Top"
				description: "A row under the bar"
				chosen: root.appBarPosition === "top"
				onPicked: root.setAppBarEdge("top")
			}

			ChoiceRow {
				label: "Bottom"
				description: "A row above the bottom edge"
				chosen: root.appBarPosition === "bottom"
				onPicked: root.setAppBarEdge("bottom")
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
							detail: modelData.description
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
